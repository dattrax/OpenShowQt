/*
 * PrepareExecutor's rules (task 6.9a), pinned headless against the real
 * object: the RPC-side caller is never parked by queued work; accepted
 * jobs complete exactly once and in FIFO order; capacity rejections are
 * explicit and hold no state; a waiting job cancelled as superseded
 * retires without its work ever running; stop() retires everything —
 * cooperative-cancel the runner, inline-cancel the waiters, join, and no
 * completion fires again. The live half of 6.9a (prepare acks and status
 * latency against a real worker) is background_media_tests; this is the
 * lifecycle contract itself, with injectable work and no session.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "session/PrepareExecutor.h"

namespace {

using openshow::worker::PrepareExecutor;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        ++gFailures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
    return condition;
}

using Clock = std::chrono::steady_clock;

constexpr auto kEve = std::chrono::seconds(2);

// Waits until `predicate` holds or the test's patience ends.
template <typename Predicate>
bool waitFor(Predicate predicate) {
    const auto deadline = Clock::now() + kEve;
    while (Clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// One executor under test: every accepted job records its completion here,
// so "exactly once" is a count, not a hope.
struct Recorder {
    std::mutex mutex;
    std::vector<std::uint64_t> completed;  // ids in completion order
    std::vector<std::uint64_t> cancelled;  // ids that completed cancelled
    std::vector<std::uint64_t> ran;        // ids whose work body executed

    void noteDone(std::uint64_t id, bool wasCancelled) {
        std::lock_guard<std::mutex> lock(mutex);
        completed.push_back(id);
        if (wasCancelled) {
            cancelled.push_back(id);
        }
    }
    void noteRan(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        ran.push_back(id);
    }
    size_t countDone(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (const std::uint64_t seen : completed) {
            n += seen == id ? 1 : 0;
        }
        return n;
    }
};

// A job body that waits on `gate` before finishing, checking the
// cancellation flag while it waits — the contract preload work keeps.
PrepareExecutor::Work gated(std::shared_future<void> gate, Recorder& rec,
                            std::uint64_t id) {
    return [gate, &rec, id](const std::atomic<bool>& cancelled) {
        rec.noteRan(id);
        while (!cancelled.load() && gate.wait_for(std::chrono::milliseconds(2)) ==
                                         std::future_status::timeout) {
        }
    };
}

PrepareExecutor::Done doneFor(Recorder& rec, std::uint64_t id) {
    return [&rec, id](bool cancelled) { rec.noteDone(id, cancelled); };
}

void testFastCompletion() {
    Recorder rec;            // declared first: it must outlive ~PrepareExecutor
    PrepareExecutor exec;
    check(exec.start(), "executor starts");
    check(!exec.start(), "there is no second start");

    std::promise<void> open;
    open.set_value();
    check(exec.submit(1, gated(open.get_future(), rec, 1), doneFor(rec, 1)),
          "a free executor accepts a job");
    check(waitFor([&] { return rec.completed.size() == 1; }), "the job completes");
    check(rec.ran.size() == 1 && rec.cancelled.empty(), "it completed uncancelled");
    check(exec.queued() == 0, "queued() settles back to zero");
}

void testCallerNeverParks() {
    Recorder rec;            // declared first: it must outlive ~PrepareExecutor
    PrepareExecutor exec;
    exec.start();
    std::promise<void> gate;
    auto release = gate.get_future().share();  // shared_future: copyable latch

    const auto t0 = Clock::now();
    check(exec.submit(1, gated(release, rec, 1), doneFor(rec, 1)),
          "the long job is accepted");
    check(exec.submit(2, gated(release, rec, 2), doneFor(rec, 2)),
          "the caller keeps submitting while job 1 runs");
    const auto elapsed = Clock::now() - t0;
    check(elapsed < std::chrono::milliseconds(50),
          "submit does not wait for queued work (the RPC thread's guarantee)");
    check(exec.queued() == 2, "queued() counts the running job and the waiter");
    gate.set_value();
    check(waitFor([&] { return rec.completed.size() == 2; }), "both complete");
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        check(rec.completed[0] == 1 && rec.completed[1] == 2,
              "completions are FIFO, not by arrival of the release");
    }
}

void testCapacityRejection() {
    Recorder rec;            // declared first: it must outlive ~PrepareExecutor
    PrepareExecutor exec;
    exec.start();
    std::promise<void> gate;
    auto release = gate.get_future().share();  // shared_future: copyable latch

    size_t accepted = 0;
    for (std::uint64_t id = 0; id < PrepareExecutor::kMaxQueued; ++id) {
        if (exec.submit(id, gated(release, rec, id), doneFor(rec, id))) {
            ++accepted;
        }
    }
    check(accepted == PrepareExecutor::kMaxQueued, "capacity is accepted exactly");
    bool rejectedDoneFired = false;
    check(!exec.submit(99, gated(release, rec, 99),
                       [&rejectedDoneFired](bool) { rejectedDoneFired = true; }),
          "the job past capacity is refused explicitly");
    check(exec.queued() == PrepareExecutor::kMaxQueued, "refusal grows nothing");
    gate.set_value();
    check(waitFor([&] { return rec.completed.size() == accepted; }),
          "every accepted job still completes");
    check(!rejectedDoneFired, "the refused job's completion never fires");
    std::promise<void> ready;
    ready.set_value();
    check(exec.submit(99, gated(ready.get_future().share(), rec, 99), doneFor(rec, 99)),
          "capacity frees after the drain: new work is accepted again");
}

void testCancelSupersededWaitingJob() {
    Recorder rec;            // declared first: it must outlive ~PrepareExecutor
    PrepareExecutor exec;
    exec.start();
    std::promise<void> gate;
    auto release = gate.get_future().share();  // shared_future: copyable latch
    exec.submit(1, gated(release, rec, 1), doneFor(rec, 1));
    // Job 1 must be RUNNING before job 2 is queued, or cancel() would take
    // the queue path for what is actually the runner.
    check(waitFor([&] { return rec.ran.size() == 1; }), "job 1 is running");
    exec.submit(2, gated(release, rec, 2), doneFor(rec, 2));

    check(exec.cancel(2), "a waiting job is cancellable by id");
    check(rec.countDone(2) == 1, "the cancelled job's done fired exactly once");
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        check(rec.cancelled.size() == 1 && rec.cancelled[0] == 2,
              "it reports cancelled");
        check(rec.ran.size() == 1, "a superseded job's work never ran");
    }
    check(!exec.cancel(2), "it cannot be cancelled twice");
    check(!exec.cancel(1), "the running job is only reachable through stop()");
    check(!exec.cancel(77), "an unknown id is honestly refused");
    gate.set_value();
    check(waitFor([&] { return rec.countDone(1) == 1; }), "the runner completes once");
}

void testStopRetiresEverything() {
    Recorder rec;            // declared first: it must outlive ~PrepareExecutor
    PrepareExecutor exec;
    exec.start();
    std::promise<void> gate;
    auto release = gate.get_future().share();  // shared_future: copyable latch
    exec.submit(1, gated(release, rec, 1), doneFor(rec, 1));
    check(waitFor([&] { return rec.ran.size() == 1; }), "job 1 is running");
    exec.submit(2, gated(release, rec, 2), doneFor(rec, 2));
    exec.submit(3, gated(release, rec, 3), doneFor(rec, 3));

    const auto t0 = Clock::now();
    exec.stop();  // must not wait for the gate: the flag IS the release
    const auto elapsed = Clock::now() - t0;
    check(elapsed < std::chrono::milliseconds(500),
          "stop() reaches the running job through its flag, not through the gate");
    check(rec.countDone(1) == 1 && rec.countDone(2) == 1 && rec.countDone(3) == 1,
          "every outstanding job answered exactly once across the stop");
    {
        std::lock_guard<std::mutex> lock(rec.mutex);
        check(rec.cancelled.size() == 3, "all three report cancelled");
    }
    check(exec.queued() == 0, "stop() leaves nothing queued");
    exec.stop();  // idempotent
    check(!exec.submit(4, gated(release, rec, 4), doneFor(rec, 4)),
          "submit after stop is refused");
    check(!exec.start(), "a retired executor does not restart");
}

void testNoGrowthAndExactlyOnceUnderChurn() {
    // Supersede storms: submit and cancel the same ids while the worker
    // races to pop them. The invariants: queued() never exceeds capacity,
    // each accepted id answers exactly once, and nothing outlives stop().
    Recorder rec;            // declared first: it must outlive ~PrepareExecutor
    PrepareExecutor exec;
    exec.start();
    std::promise<void> hold;
    auto holdGate = hold.get_future().share();
    check(exec.submit(0, gated(holdGate, rec, 0), doneFor(rec, 0)),
          "a blocker pins the runner thread");
    check(waitFor([&] { return rec.ran.size() == 1; }), "the blocker runs");

    size_t accepted = 1;
    for (std::uint64_t round = 0; round < 200; ++round) {
        const std::uint64_t id = 100 + round;
        const bool submitted =
            exec.submit(id, gated(holdGate, rec, id), doneFor(rec, id));
        if (submitted) {
            ++accepted;
        }
        check(exec.queued() <= PrepareExecutor::kMaxQueued,
              "queued() is bounded through the churn");
        if (submitted && !exec.cancel(id)) {
            // The worker took it first; its completion arrives after the
            // blocker. Nothing to do — the exactly-once audit covers it.
        }
    }
    hold.set_value();
    check(waitFor([&] { return rec.completed.size() == accepted; }),
          "every accepted job answered across the storm");
    for (std::uint64_t round = 0; round < 200; ++round) {
        if (rec.countDone(100 + round) > 1) {
            check(false, "no id answers twice");
            break;
        }
    }
    exec.stop();
    check(exec.queued() == 0, "and the executor ends empty");
}

}  // namespace

int main() {
    testFastCompletion();
    testCallerNeverParks();
    testCapacityRejection();
    testCancelSupersededWaitingJob();
    testStopRetiresEverything();
    testNoGrowthAndExactlyOnceUnderChurn();

    if (gFailures != 0) {
        std::fprintf(stderr, "%d prepare-executor checks failed\n", gFailures);
        return 1;
    }
    std::printf("all prepare-executor checks passed\n");
    return 0;
}
