// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/PowerPolicy.h"

#include <windows.h>

#include <powrprof.h>
#if __has_include(<powersetting.h>)
#include <powersetting.h>  // PowerReadAC/DCValueIndex on SDKs that split them out of powrprof.h
#endif

#pragma comment(lib, "powrprof.lib")  // GetSystemPowerStatus is kernel32 and needs no lib

namespace rabbitears {
namespace {

// winnt.h only DECLARES these (DEFINE_GUID without INITGUID) and no import library exports them,
// so define them here rather than fight the include order. Values match `powercfg /q SUB_SLEEP`.
constexpr GUID kSleepSubgroup = {
    0x238c9fa8, 0x0aad, 0x41ed, {0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20}};
constexpr GUID kAllowRtcWake = {
    0xbd3b718a, 0x0680, 0x4d9d, {0x8a, 0xb2, 0xe1, 0xd2, 0xb4, 0xac, 0x80, 0x6d}};

WakeTimerSetting fromIndex(DWORD v) {
    switch (v) {
        case 0: return WakeTimerSetting::Disable;
        case 1: return WakeTimerSetting::Enable;
        case 2: return WakeTimerSetting::ImportantOnly;
        default: return WakeTimerSetting::Unknown;
    }
}

}  // namespace

WakeVerdict queryWakePolicy() {
    WakePolicyInputs in;  // permissive defaults; every probe below can only ever tighten them

    SYSTEM_POWER_CAPABILITIES caps{};
    bool haveCaps = CallNtPowerInformation(SystemPowerCapabilities, nullptr, 0, &caps,
                                           sizeof(caps)) == 0;  // NTSTATUS: 0 == STATUS_SUCCESS
    if (haveCaps) {
        // A Modern Standby (AoAc) machine has no S1-S3 states, so RtcWake reads as
        // PowerSystemUnspecified even though timer wakes work fine through the S0 resiliency
        // phase. Only a NON-AoAc machine reporting no RTC wake state genuinely cannot wake.
        in.rtcWakeCapable = caps.AoAc || caps.RtcWake != PowerSystemUnspecified;
        in.hasBattery = caps.SystemBatteriesPresent != 0;
    }

    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) {
        in.onBattery = (sps.ACLineStatus == 0);  // 1 = AC, 255 = unknown -> assume AC
        // Fallback only: BatteryFlag's bit 7 means "no system battery", but the whole-byte 0xFF
        // sentinel ("gauge unreadable") sets it too, so 255 must not be read as a missing battery.
        if (!haveCaps) in.hasBattery = sps.BatteryFlag != 255 && (sps.BatteryFlag & 128) == 0;
    }

    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
        DWORD idx = 0;
        if (PowerReadACValueIndex(nullptr, scheme, &kSleepSubgroup, &kAllowRtcWake, &idx) ==
            ERROR_SUCCESS)
            in.ac = fromIndex(idx);
        if (PowerReadDCValueIndex(nullptr, scheme, &kSleepSubgroup, &kAllowRtcWake, &idx) ==
            ERROR_SUCCESS)
            in.dc = fromIndex(idx);
        LocalFree(scheme);
    }

    return decideWakePolicy(in);
}

std::wstring wakeVerdictText(const WakeVerdict& v) {
    switch (v.reason) {
        case WakeBlock::TimersDisabled:
            return L"Windows wake timers are turned off for the current power plan, so this PC "
                   L"will not wake itself to record. Turn them on under Power Options ▸ Sleep ▸ "
                   L"Allow wake timers.";
        case WakeBlock::ImportantOnly:
            // Microsoft documents the class but not its membership; the consensus is that an
            // app's task is not "important". Hedge the wording rather than overclaim.
            return L"Windows is allowing important wake timers only, which may not include "
                   L"RabbitEars — this PC may not wake itself to record. Set Power Options ▸ "
                   L"Sleep ▸ Allow wake timers to Enable.";
        case WakeBlock::NoRtcCapability:
            return L"This PC reports no wake-timer support, so it cannot wake itself to record. "
                   L"Scheduled recordings still run whenever RabbitEars is already awake.";
        case WakeBlock::None:
            if (v.otherSourceBlocked)
                return L"This PC will wake for scheduled recordings while it stays on its current "
                       L"power source. Wake timers are off for the other one, so switching "
                       L"between battery and mains before a recording will stop it waking.";
            return std::wstring();
    }
    return std::wstring();
}

}  // namespace rabbitears
