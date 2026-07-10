// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/PowerPolicy.h"

namespace rabbitears {
namespace {

bool blocks(WakeTimerSetting s) {
    return s == WakeTimerSetting::Disable || s == WakeTimerSetting::ImportantOnly;
}

}  // namespace

WakeVerdict decideWakePolicy(const WakePolicyInputs& in) {
    WakeVerdict v;

    // Hardware trumps policy: a machine that cannot arm an RTC wake at all will not wake however
    // the power plan is configured, so say that instead of pointing the user at a setting.
    if (!in.rtcWakeCapable) {
        v.willWake = false;
        v.reason = WakeBlock::NoRtcCapability;
        return v;
    }

    // A machine cannot run on battery without having one. If the probe reads "on battery" but
    // cannot see the battery — an unreadable gauge reports BatteryFlag 0xFF, whose bit 7 is set
    // exactly like "no system battery" — believe the power source, not the gauge. Falling back to
    // the AC index there would promise a wake that the DC policy is about to refuse.
    const bool discharging = in.onBattery;
    const bool hasBattery = in.hasBattery || in.onBattery;
    switch (discharging ? in.dc : in.ac) {
        case WakeTimerSetting::Disable:
            v.willWake = false;
            v.reason = WakeBlock::TimersDisabled;
            return v;
        case WakeTimerSetting::ImportantOnly:
            v.willWake = false;
            v.reason = WakeBlock::ImportantOnly;
            return v;
        case WakeTimerSetting::Enable:
        case WakeTimerSetting::Unknown:
            break;
    }

    // The recording is in the future, and so is the user's next trip to the power cable.
    if (hasBattery) v.otherSourceBlocked = blocks(discharging ? in.ac : in.dc);
    return v;
}

}  // namespace rabbitears
