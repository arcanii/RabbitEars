// SPDX-License-Identifier: GPL-3.0-or-later
// See AudioLevelTap.h.
//
// Design: libVLC plays audio normally through its own Core Audio output; we do
// NOT sit in that path. Instead we create a Core Audio *process tap* (macOS 14.2+)
// on our own process, wrap it in a private aggregate device, and install an IOProc
// that receives a copy of the output audio, computes a peak, and hands it to the
// meter. Because the tap is CATapUnmuted the real audio still reaches the speakers
// in sync — the tap only observes. This is what makes it desync-proof, unlike the
// old approach of taking over libVLC's audio output.
#import "AudioLevelTap.h"

#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>   // AudioHardwareCreate/DestroyProcessTap (macOS 14.2+)
#import <CoreAudio/CATapDescription.h>

#include "platform/Log.h"

#include <cmath>
#include <string>
#include <unistd.h>

@interface AudioLevelTap ()
- (AudioObjectID)processObjectForCurrentPID;
- (BOOL)start API_AVAILABLE(macos(14.2));
@end

@implementation AudioLevelTap {
    AudioObjectID       _tapID;      // the process tap
    AudioObjectID       _aggregate;  // private aggregate device wrapping the tap
    AudioDeviceIOProcID _ioProc;     // installed on _aggregate
    void (^_handler)(float);
    BOOL                _running;
}

- (nullable instancetype)initWithLevelHandler:(void (^)(float))handler {
    if (!(self = [super init])) return nil;
    _tapID = kAudioObjectUnknown;
    _aggregate = kAudioObjectUnknown;
    _ioProc = NULL;
    _handler = [handler copy];
    if (@available(macOS 14.2, *)) {
        if ([self start]) return self;
    }
    // Older macOS, or the tap couldn't be built — clean up and report no meter.
    [self stop];
    return nil;
}

// Translate our PID into the Core Audio process AudioObjectID (the thing a tap
// description references).
- (AudioObjectID)processObjectForCurrentPID {
    pid_t pid = getpid();
    AudioObjectID obj = kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyTranslatePIDToProcessObject,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(obj);
    OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                             sizeof(pid), &pid, &size, &obj);
    return (st == noErr) ? obj : kAudioObjectUnknown;
}

- (BOOL)start {
    // 1. A stereo-mixdown tap of THIS process, UNMUTED so playback still reaches
    //    the speakers — we only observe.
    AudioObjectID proc = [self processObjectForCurrentPID];
    if (proc == kAudioObjectUnknown) {
        rabbitears::diag::error(L"AudioLevelTap: could not resolve this process's audio object");
        return NO;
    }

    CATapDescription* desc =
        [[CATapDescription alloc] initStereoMixdownOfProcesses:@[ @(proc) ]];
    desc.name = @"RabbitEars Level Meter";
    desc.privateTap = YES;             // visible only to us
    desc.muteBehavior = CATapUnmuted;  // keep audio audible (also the default)

    OSStatus st = AudioHardwareCreateProcessTap(desc, &_tapID);
    if (st != noErr || _tapID == kAudioObjectUnknown) {
        rabbitears::diag::error(L"AudioLevelTap: AudioHardwareCreateProcessTap failed (status "
                                + std::to_wstring(st) + L") — possibly an audio-capture consent issue");
        return NO;
    }

    // 2. The tap's UID, to reference it from a private aggregate device.
    CFStringRef tapUID = NULL;
    AudioObjectPropertyAddress uidAddr = {
        kAudioTapPropertyUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 uidSize = sizeof(tapUID);
    st = AudioObjectGetPropertyData(_tapID, &uidAddr, 0, NULL, &uidSize, &tapUID);
    if (st != noErr || !tapUID) {
        rabbitears::diag::error(L"AudioLevelTap: could not read the tap UID (status "
                                + std::to_wstring(st) + L")");
        return NO;
    }
    NSString* tapUIDStr = (__bridge_transfer NSString*)tapUID;

    // 3. A PRIVATE aggregate device containing only this tap, so we can install an
    //    IOProc and receive the tapped audio as its input.
    NSString* aggUID = [NSString stringWithFormat:@"com.rabbitears.metertap.%d", getpid()];
    NSDictionary* aggDesc = @{
        @(kAudioAggregateDeviceNameKey):      @"RabbitEars Meter Aggregate",
        @(kAudioAggregateDeviceUIDKey):       aggUID,
        @(kAudioAggregateDeviceIsPrivateKey): @YES,
        @(kAudioAggregateDeviceTapListKey):   @[ @{ @(kAudioSubTapUIDKey): tapUIDStr } ],
    };
    st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggDesc, &_aggregate);
    if (st != noErr || _aggregate == kAudioObjectUnknown) {
        rabbitears::diag::error(L"AudioLevelTap: AudioHardwareCreateAggregateDevice failed (status "
                                + std::to_wstring(st) + L")");
        return NO;
    }

    // 4. IOProc: peak over the tapped buffers → the level handler. Runs on a
    //    real-time audio thread, so keep it trivial (pushLevel: is a lock-free
    //    store). Tap audio is 32-bit float; clamp so a format surprise can't peg
    //    past full-scale.
    void (^levelHandler)(float) = _handler;
    st = AudioDeviceCreateIOProcIDWithBlock(&_ioProc, _aggregate, NULL,
        ^(const AudioTimeStamp* inNow, const AudioBufferList* inInputData,
          const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
          const AudioTimeStamp* inOutputTime) {
            (void)inNow; (void)inInputTime; (void)outOutputData; (void)inOutputTime;
            float peak = 0.0f;
            if (inInputData) {
                for (UInt32 b = 0; b < inInputData->mNumberBuffers; ++b) {
                    const AudioBuffer buf = inInputData->mBuffers[b];
                    const float* samples = (const float*)buf.mData;
                    if (!samples) continue;
                    const size_t n = buf.mDataByteSize / sizeof(float);
                    for (size_t i = 0; i < n; ++i) {
                        const float a = fabsf(samples[i]);
                        if (a > peak) peak = a;
                    }
                }
            }
            if (levelHandler) levelHandler(peak > 1.0f ? 1.0f : peak);
        });
    if (st != noErr || !_ioProc) {
        rabbitears::diag::error(L"AudioLevelTap: AudioDeviceCreateIOProcIDWithBlock failed (status "
                                + std::to_wstring(st) + L")");
        return NO;
    }

    st = AudioDeviceStart(_aggregate, _ioProc);
    if (st != noErr) {
        rabbitears::diag::error(L"AudioLevelTap: AudioDeviceStart failed (status "
                                + std::to_wstring(st) + L")");
        return NO;
    }
    _running = YES;
    rabbitears::diag::info(L"AudioLevelTap: metering started");
    return YES;
}

- (void)stop {
    if (@available(macOS 14.2, *)) {
        if (_aggregate != kAudioObjectUnknown && _ioProc) {
            if (_running) AudioDeviceStop(_aggregate, _ioProc);
            AudioDeviceDestroyIOProcID(_aggregate, _ioProc);
        }
        if (_aggregate != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(_aggregate);
        if (_tapID != kAudioObjectUnknown)     AudioHardwareDestroyProcessTap(_tapID);
    }
    _ioProc = NULL;
    _aggregate = kAudioObjectUnknown;
    _tapID = kAudioObjectUnknown;
    _running = NO;
}

- (void)dealloc {
    [self stop];
}

@end
