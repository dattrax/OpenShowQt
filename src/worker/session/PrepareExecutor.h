#pragma once
/*
 * PrepareExecutor (task 6.9a): the bounded, cancellable worker for
 * prepare-time file work — still-image decodes and media probes — so the
 * single RPC thread never parks inside a cold decode. The rule this object
 * owns (review R10): an accepted request is answered — by its completion,
 * once, exactly — and work the worker no longer needs (disconnect, stop,
 * a queue at capacity) retires without growth.
 *
 * Shape: ONE worker thread, a FIFO queue of at most kMaxQueued accepted
 * jobs including the running one, and cooperative cancellation — work
 * checks its flag between units (an image at a time) and returns early.
 * There is no preemption of a decode already in flight, and no thread pool:
 * prepares arrive at operator speed and serialising them bounds the memory
 * of the image cache they feed.
 *
 * Contracts (each pinned by tests/prepare_executor_tests.cpp):
 *   - submit() is nonblocking and answers with true only when the job was
 *     queued. Rejections are explicit: stopped, or the queue is full. A
 *     rejected job's completion never fires — the caller owns the "no".
 *   - every accepted job's done fires EXACTLY ONCE: from the worker thread
 *     when the work finishes or observes its cancelled flag, or from the
 *     caller of cancel()/stop() for a job that was still waiting. done(true)
 *     means the work was cancelled and may not have run at all.
 *   - cancel(id) retires a WAITING job (its done fires inline, true). The
 *     running job is not cancelable by id — only stop() reaches it, through
 *     the flag — so cancel answers "this was superseded before it started",
 *     never "this was interrupted mid-decode".
 *   - stop() flips the running job's flag, retires every waiting job (their
 *     done fires, true), JOINS the worker thread and drains the queue. It
 *     is idempotent, and when it returns no completion can fire again and
 *     queued() is 0 — this is the disconnect/shutdown guarantee that lets
 *     the session tear down everything the work touches afterwards.
 *   - nothing grows: accepted-and-unfinished work never exceeds
 *     kMaxQueued, the queue holds no rejected submissions, and after
 *     stop() holds nothing at all.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace openshow::worker {

class PrepareExecutor {
public:
    // The unit of file work. `cancelled` is set by cancel()/stop() between
    // units; the work MUST check it between its own pieces (one decoded
    // image is a piece) and return when it says stop.
    using Work = std::function<void(const std::atomic<bool>& cancelled)>;
    // Fires exactly once per accepted job; see the header contracts.
    using Done = std::function<void(bool cancelled)>;

    // Operator-speed work with one decode-heavy job running: anything
    // past this on a live socket is the frontend asking faster than the
    // disk can answer, and the honest answer is a refusal it can retry.
    static constexpr std::size_t kMaxQueued = 4;

    PrepareExecutor() = default;
    ~PrepareExecutor();  // stop()
    PrepareExecutor(const PrepareExecutor&) = delete;
    PrepareExecutor& operator=(const PrepareExecutor&) = delete;

    // Starts the worker thread. Returns false if it is already running
    // (there is no restart: after stop() the object is retired).
    bool start();

    // Queues a job under a caller-chosen id (the RPC request id, so a
    // reply can name the request its completion answers). Returns false —
    // without touching the callables — when stopped or full.
    bool submit(std::uint64_t jobId, Work work, Done done);

    // Retires a waiting job by id: its done fires inline with true.
    // Returns false for the running job and for unknown ids.
    bool cancel(std::uint64_t jobId);

    // See the header. Blocks until the worker thread has joined.
    void stop();

    // Accepted jobs not yet completed: the running one plus the waiting
    // queue (0 after stop()).
    std::size_t queued() const;

private:
    struct Job {
        std::uint64_t id = 0;
        std::shared_ptr<std::atomic<bool>> cancelled;
        Work work;
        Done done;
    };

    void run();

    mutable std::mutex fMutex;
    std::condition_variable fCv;
    std::deque<Job> fQueue;             // waiting jobs only; never the running one
    std::shared_ptr<std::atomic<bool>> fRunningFlag;  // set while a job runs
    std::uint64_t fRunningId = 0;
    bool fStarted = false;
    bool fStopping = false;
    std::thread fThread;
};

}  // namespace openshow::worker
