#include "session/PrepareExecutor.h"

#include <utility>

namespace openshow::worker {

PrepareExecutor::~PrepareExecutor() {
    stop();
}

bool PrepareExecutor::start() {
    // start()/stop() are serialised by the owning session's lifecycle
    // (open() starts, close() stops; nothing else touches the thread).
    std::lock_guard<std::mutex> lock(fMutex);
    if (fStarted || fStopping) {
        return false;  // there is no restart, and no start after stop
    }
    fStarted = true;
    fThread = std::thread([this] { run(); });
    return true;
}

bool PrepareExecutor::submit(std::uint64_t jobId, Work work, Done done) {
    if (!work || !done) {
        return false;  // a job that could never answer its caller is not accepted
    }
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (!fStarted || fStopping) {
            return false;
        }
        // The capacity includes the running job: kMaxQueued is everything
        // this executor can owe a completion for at one instant.
        if (fQueue.size() + (fRunningFlag ? 1u : 0u) >= kMaxQueued) {
            return false;
        }
        fQueue.push_back(Job{jobId, std::make_shared<std::atomic<bool>>(false),
                             std::move(work), std::move(done)});
    }
    fCv.notify_one();
    return true;
}

bool PrepareExecutor::cancel(std::uint64_t jobId) {
    Job retired;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        if (fRunningFlag && jobId == fRunningId) {
            return false;  // a decode in flight is only reachable through stop()
        }
        for (auto it = fQueue.begin(); it != fQueue.end(); ++it) {
            if (it->id != jobId) {
                continue;
            }
            // Pop under the lock: the worker's own pop races this, and
            // whoever holds the lock takes the job whole.
            retired = std::move(*it);
            fQueue.erase(it);
            found = true;
            break;
        }
        if (!found) {
            return false;  // unknown id (or already taken by the worker)
        }
    }
    retired.done(true);
    return true;
}

void PrepareExecutor::stop() {
    std::deque<Job> retired;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fStopping = true;
        if (fRunningFlag) {
            fRunningFlag->store(true);  // the running job leaves at its next check
        }
        retired.swap(fQueue);
        fCv.notify_all();
    }
    // Retired waiting jobs answer from THIS thread, before the join, so a
    // caller whose object owns both ends (the server's deferred replies)
    // sees every completion settle before teardown continues.
    for (Job& job : retired) {
        job.done(true);
    }
    if (fThread.joinable()) {
        fThread.join();
    }
}

std::size_t PrepareExecutor::queued() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fQueue.size() + (fRunningFlag ? 1u : 0u);
}

void PrepareExecutor::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(fMutex);
            fCv.wait(lock, [this] { return fStopping || !fQueue.empty(); });
            if (fQueue.empty()) {
                return;  // fStopping: stop() drains the queue under the lock
            }
            job = std::move(fQueue.front());
            fQueue.pop_front();
            fRunningFlag = job.cancelled;
            fRunningId = job.id;
        }
        job.work(*job.cancelled);
        const bool wasCancelled = job.cancelled->load();
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fRunningFlag.reset();
            fRunningId = 0;
        }
        job.done(wasCancelled);
    }
}

}  // namespace openshow::worker
