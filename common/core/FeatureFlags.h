// SPDX-License-Identifier: GPL-3.0-or-later
//
// Beta / testing feature flags — the switchboard behind Settings ▸ System… ▸ Beta features.
//
// WHY: it lets a feature ship to real users while still OFF by default, so testers can turn it
// on deliberately and the rest of the userbase is unaffected. That is the difference between
// "we shipped untested code" and "we shipped code nobody runs unless they opt in".
//
// Rules for a flag:
//   • Default OFF. A beta feature that defaults on is just a feature.
//   • The gated code must be inert when off — no threads, no network, no timers, no DB writes.
//   • The id is a STABLE token: it is the settings key suffix, and flipping it would silently
//     reset every tester's choice. Retire an id by leaving it here, not by reusing it.
//   • When a feature graduates, delete the flag AND its branch — don't leave a dead toggle.
//
// Header-only + inline on purpose (same reasoning as diag::Level in common/platform/Log.h): the
// state lives in a function-local static atomic, so any translation unit on either platform can
// query a flag with no link dependency and no platform implementation to keep in sync.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace rabbitears {

enum class BetaFeature : uint32_t {
    DeadLinkChecker = 1u << 0,  // background probe that marks unreachable channels dead_status
    // next: 1u << 1, …
};

// Stable settings-key suffix — persisted as "beta_<id>" = "1"/"0".
inline const char* betaFeatureId(BetaFeature f) {
    switch (f) {
        case BetaFeature::DeadLinkChecker: return "dead_link_checker";
    }
    return "";
}
inline std::string betaFeatureSettingKey(BetaFeature f) {
    return std::string("beta_") + betaFeatureId(f);
}

// Every flag, in the order the System… dialog lists them.
inline const BetaFeature* allBetaFeatures(int& count) {
    static const BetaFeature kAll[] = {BetaFeature::DeadLinkChecker};
    count = static_cast<int>(sizeof(kAll) / sizeof(kAll[0]));
    return kAll;
}

inline std::atomic<uint32_t>& betaMaskRef() {
    static std::atomic<uint32_t> mask{0};  // all off until startup loads the persisted set
    return mask;
}
inline bool betaEnabled(BetaFeature f) {
    return (betaMaskRef().load(std::memory_order_relaxed) & static_cast<uint32_t>(f)) != 0;
}
inline void setBetaEnabled(BetaFeature f, bool on) {
    const uint32_t bit = static_cast<uint32_t>(f);
    uint32_t cur = betaMaskRef().load(std::memory_order_relaxed);
    while (!betaMaskRef().compare_exchange_weak(cur, on ? (cur | bit) : (cur & ~bit),
                                                std::memory_order_relaxed))
        ;  // retry — the mask is written from the UI thread but read from workers
}

}  // namespace rabbitears
