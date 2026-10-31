/*
 * SeekQueue: the one seek-request protocol the media sources share
 * (task 6.7d, review R19).
 *
 * A seek is a request that a DECODE thread applies later, so the protocol is
 * a PAIR — (generation, target) — and the applier may only credit the
 * generation whose target it actually moved the container to. Reading the
 * newest target and then acknowledging the newest count (what both sources
 * used to do) credits a request that arrived mid-apply without ever applying
 * ITS target: the pacing thread, which reads `requested != applied` as "flush
 * in progress", stops draining and presents pixels from the superseded
 * position while the caller believes the newer jump landed.
 *
 * So `pending()` hands over the pair, `apply(gen)`/`refuse(gen)` settle
 * exactly that generation, and a newer request that arrived in between stays
 * pending for the next pass. Requests that arrive before any pass coalesce
 * (newest target wins) — that is the documented shape, and settling the
 * newest generation settles the superseded ones with it.
 *
 * Two terminal outcomes matter to the caller. `apply` means the audience's
 * position is the target; `refuse` means the jump FAILED and the position is
 * whatever it was before. Both settle (nothing stays pending, so a stream's
 * own "a seek is queued, stop sprinting ahead" rule releases either way), but
 * only `apply` moves the applied watermark — which is what a waiter's bool
 * reports. Before that, a failed container seek still advanced the applied
 * generation, so `waitSeekApplied` answered "applied" for a jump that never
 * happened, and the transport went on to flush its queues and re-anchor its
 * clock on a position the media never reached.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace openshow::media {

class SeekQueue {
public:
    // Requests a jump to `seconds`, returning the stamp assigned to THIS
    // request (0 is never a valid stamp). Negative targets clamp to 0.
    uint64_t request(double seconds);

    // The newest request that has not settled, paired with the target that
    // must be applied for it. False when nothing is pending.
    bool pending(uint64_t* generation, double* seconds) const;

    // The newest pending request FAILED to apply (its stamp, from
    // pending()). Settles without moving the applied watermark.
    void refuse(uint64_t generation);

    // Settles `generation` — and every superseded older request — as
    // APPLIED: the media is at its target now.
    void apply(uint64_t generation);

    // Blocks (2 ms polls; no condition variable to lose a wake-up on) until
    // `generation` settles, then reports whether it was APPLIED rather than
    // refused. Returns as soon as it settles either way, so a refusal does
    // not cost the caller the whole timeout. timeoutMs < 0 waits forever;
    // `abort`, when set and true, ends the wait as a failure — the caller's
    // worker thread is gone and nothing will settle this request. `abort2`
    // (task 6.22e4, review A5) is a second such flag owned by whoever waits
    // rather than by the applier: a transport moving its wait to a lane can
    // call the wait off when the transport is retired, while the feeder's
    // own stop flag still ends it independently.
    bool waitApplied(uint64_t generation, int timeoutMs,
                     const std::atomic<bool>* abort = nullptr,
                     const std::atomic<bool>* abort2 = nullptr) const;

    // Same, for the newest request as of this call.
    bool waitAppliedLatest(int timeoutMs, const std::atomic<bool>* abort = nullptr,
                           const std::atomic<bool>* abort2 = nullptr) const;

    // Transport restart: no requests have been made and none is pending.
    void reset();

    // Counters for status and tests: the newest stamp handed out, the newest
    // APPLIED generation, and the newest SETTLED one (applied or refused).
    // A consumer's flush protocol reads requested against applied; "no work
    // pending" is requested against settled.
    uint64_t requested() const;
    uint64_t appliedGeneration() const;
    uint64_t settledGeneration() const;

private:
    // The pair is what the review asked for: the target and the stamp it
    // belongs to are written and read under one lock, so an applier can
    // never combine a new target with an old acknowledgement.
    mutable std::mutex fMutex;
    double fTargetSeconds = 0.0;
    uint64_t fRequested = 0;
    uint64_t fSettled = 0;
    uint64_t fApplied = 0;
};

}  // namespace openshow::media
