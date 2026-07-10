// SPDX-License-Identifier: GPL-3.0-or-later
// PowerPolicy — the pure decision core for the wake-timer preflight.
//
// Registering the Task Scheduler wake task (Win32/platform/WakeScheduler) is not enough on its
// own: Windows only arms the underlying RTC wake timer when the active power plan's "Allow wake
// timers" setting permits it, and that setting is stored PER POWER SOURCE. A laptop plugged in
// today can therefore silently miss every recording tomorrow on battery — the task looks healthy
// in Task Scheduler and simply never fires until the user wakes the machine themselves.
//
// This maps the raw setting indices + the current power source onto a verdict. No Win32, no DB —
// so the decision table is unit-tested in the CLI selftest. The platform probe
// (Win32/platform/PowerPolicy) reads the indices from the OS; the UI turns the reason into copy.
#pragma once

namespace rabbitears {

// "Allow wake timers" (GUID_ALLOW_RTC_WAKE) index values, exactly as powercfg reports them.
enum class WakeTimerSetting {
    Disable = 0,
    Enable = 1,
    ImportantOnly = 2,  // only "important" (system) timers — see WakeBlock::ImportantOnly
    Unknown = 3,        // unreadable, or a value Windows adds later: always fail OPEN
};

enum class WakeBlock {
    None = 0,         // an RTC wake armed right now would fire
    TimersDisabled,   // the current source's setting is Disable
    ImportantOnly,    // the current source's setting is ImportantOnly
    NoRtcCapability,  // the firmware cannot RTC-wake from any sleep state at all
};

// Raw inputs, deliberately free of Win32 types so the decision stays headless + testable.
struct WakePolicyInputs {
    bool hasBattery = false;     // a battery exists at all (a desktop has none, so DC is moot)
    bool onBattery = false;      // the current source is the battery
    bool rtcWakeCapable = true;  // the machine can wake on an RTC timer from some sleep state
    WakeTimerSetting ac = WakeTimerSetting::Enable;
    WakeTimerSetting dc = WakeTimerSetting::Enable;
};

struct WakeVerdict {
    bool willWake = true;
    WakeBlock reason = WakeBlock::None;
    // Set only when willWake is true and a battery exists: the CURRENT source would wake but the
    // OTHER one would not. A plugged-in laptop with wake timers off on battery records tonight
    // and silently misses tomorrow's airing if it is unplugged first — worth a quieter warning.
    bool otherSourceBlocked = false;
};

// Pure, clock-free, OS-free. Reads the index for the source in use (DC only when a battery
// exists and is in use, else AC) and maps it. An unreadable setting never blocks: we would
// rather miss a warning than nag a machine whose policy we could not determine.
WakeVerdict decideWakePolicy(const WakePolicyInputs& in);

}  // namespace rabbitears
