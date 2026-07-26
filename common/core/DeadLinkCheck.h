// SPDX-License-Identifier: GPL-3.0-or-later
//
// DeadLinkCheck — the decision core for the background dead-link checker.
//
// Pure logic, no network and no DB, so the dangerous part is testable headlessly. The dangerous
// part is NOT the probing; it is deciding what a probe MEANS. Get that wrong and the checker marks
// thousands of working channels dead, and with "Hide unavailable" on the user's library simply
// disappears — recovered only through a menu they have never opened. Every rule here exists to
// make that outcome hard to reach:
//
//   • a probe that failed to CONNECT is Inconclusive, never Dead — an offline laptop, a VPN drop
//     or a captive portal must never be read as "every channel is gone";
//   • a whole sweep is discarded unless enough of it succeeded (see sweepIsTrustworthy) — the
//     single most important guard, because a dead network fails every probe identically;
//   • only an unambiguous rejection from a server that actually answered marks a channel Dead.
//
// Shared with mac (graphics-free, windows.h-free) so both platforms classify identically.
#pragma once

#include <cstddef>

namespace rabbitears {

enum class ProbeOutcome {
    Alive,         // the server answered and served (or would serve) the stream
    Dead,          // the server answered and refused: gone, forbidden, or a hard server error
    Inconclusive,  // we learned nothing — no connection, a timeout, or a status we can't judge
};

// Transport-level result of one probe, before any HTTP status is available.
enum class ProbeTransport {
    Ok,           // connected and got a response
    NoConnect,    // DNS/connect/TLS failure — says more about US than about the channel
    Timeout,      // connected (or not) but nothing arrived in time
};

// Classify one probe. `httpStatus` is only meaningful when transport == Ok.
//
// Deliberately conservative: a stream that answers 200/206/3xx is Alive; 404/410 (gone) and
// 401/403 (refused) are Dead; 5xx is Inconclusive, NOT Dead — an overloaded provider is the most
// common cause and it recovers. Anything unrecognised is Inconclusive.
ProbeOutcome classifyProbe(ProbeTransport transport, int httpStatus);

// Would writing this sweep's results be safe? Called before ANY dead_status is written.
//
// The failure this prevents: the machine goes offline mid-sweep, every remaining probe fails to
// connect, and a naive checker marks the rest of the library dead. A healthy sweep always contains
// a decent share of channels that answered; if almost nothing did, the fault is far more likely to
// be at our end than at every provider's simultaneously.
//
// Rules: at least `minProbes` results, and the share that actually reached a server
// (alive + dead) must be at least `minRespondingPct` percent of them.
bool sweepIsTrustworthy(int alive, int dead, int inconclusive, int minProbes = 5,
                        int minRespondingPct = 40);

// How long a recorded verdict stays fresh. A channel checked inside this window is skipped, so a
// re-run resumes rather than re-probing the whole library — and a provider is not hammered by a
// user who opens the dialog repeatedly.
constexpr long long kDeadLinkTtlSeconds = 24LL * 60 * 60;

// Should this channel be probed now? `lastCheckedUtc` of 0 means never checked.
bool needsProbe(long long lastCheckedUtc, long long nowUtc,
                long long ttlSeconds = kDeadLinkTtlSeconds);

}  // namespace rabbitears
