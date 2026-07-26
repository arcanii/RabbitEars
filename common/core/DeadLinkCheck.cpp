// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/DeadLinkCheck.h"

namespace rabbitears {

ProbeOutcome classifyProbe(ProbeTransport transport, int httpStatus) {
    // Never blame the channel for a failure that is symmetric across every channel. A laptop that
    // just lost wifi produces NoConnect for all 12,000 entries; treating that as evidence would
    // wipe the library. Timeout is likewise ambiguous — a slow provider and a slow link look the
    // same from here.
    if (transport == ProbeTransport::NoConnect || transport == ProbeTransport::Timeout)
        return ProbeOutcome::Inconclusive;

    if (httpStatus >= 200 && httpStatus < 300) return ProbeOutcome::Alive;
    // A redirect is how most CDNs hand off to an edge; the channel is fine.
    if (httpStatus >= 300 && httpStatus < 400) return ProbeOutcome::Alive;

    switch (httpStatus) {
        case 401:  // needs credentials we do not have -> unusable for this user
        case 403:  // refused (often geo-blocking) -> unusable from here
        case 404:  // not found
        case 410:  // gone, explicitly
            return ProbeOutcome::Dead;
        default:
            break;
    }
    // 5xx is the interesting case: an overloaded or briefly broken provider is the usual cause and
    // it recovers, so it must NOT be Dead. Everything else unrecognised is likewise Inconclusive —
    // we only mark Dead on an unambiguous refusal.
    return ProbeOutcome::Inconclusive;
}

bool sweepIsTrustworthy(int alive, int dead, int inconclusive, int minProbes,
                        int minRespondingPct) {
    if (alive < 0 || dead < 0 || inconclusive < 0) return false;
    const int total = alive + dead + inconclusive;
    if (total < minProbes) return false;  // too small a sample to conclude anything
    const int responding = alive + dead;  // probes that actually reached a server
    // Integer compare, no floating point: responding/total >= pct/100.
    return responding * 100 >= total * minRespondingPct;
}

bool needsProbe(long long lastCheckedUtc, long long nowUtc, long long ttlSeconds) {
    if (lastCheckedUtc <= 0) return true;             // never checked
    if (nowUtc < lastCheckedUtc) return true;         // clock went backwards — re-check rather
                                                      // than trust a timestamp from the future
    return (nowUtc - lastCheckedUtc) >= ttlSeconds;
}

}  // namespace rabbitears
