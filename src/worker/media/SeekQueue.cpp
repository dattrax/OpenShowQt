#include "media/SeekQueue.h"

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace openshow::media {

uint64_t SeekQueue::request(double seconds) {
    std::lock_guard<std::mutex> lock(fMutex);
    fTargetSeconds = seconds > 0.0 ? seconds : 0.0;
    return ++fRequested;
}

bool SeekQueue::pending(uint64_t* generation, double* seconds) const {
    std::lock_guard<std::mutex> lock(fMutex);
    if (fRequested <= fSettled) {
        return false;
    }
    if (generation) {
        *generation = fRequested;
    }
    if (seconds) {
        *seconds = fTargetSeconds;
    }
    return true;
}

void SeekQueue::refuse(uint64_t generation) {
    std::lock_guard<std::mutex> lock(fMutex);
    // Settles the request without crediting it: the media did not move, and
    // nothing may wait on this generation again. A newer request (a bigger
    // stamp) is untouched and stays pending.
    if (generation > fSettled && generation <= fRequested) {
        fSettled = generation;
    }
}

void SeekQueue::apply(uint64_t generation) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (generation > fRequested) {
        return;  // a stamp this queue never handed out credits nothing
    }
    if (generation > fSettled) {
        fSettled = generation;
    }
    if (generation > fApplied) {
        fApplied = generation;
    }
}

bool SeekQueue::waitApplied(uint64_t generation, int timeoutMs,
                            const std::atomic<bool>* abort,
                            const std::atomic<bool>* abort2) const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(fMutex);
            if (fSettled >= generation) {
                // Terminal either way — report WHICH outcome, which is the
                // whole point: a refused jump must not answer "applied".
                return fApplied >= generation;
            }
        }
        if ((abort && abort->load(std::memory_order_acquire)) ||
            (abort2 && abort2->load(std::memory_order_acquire))) {
            return false;  // the applier is gone, or the waiter retired; never settles
        }
        if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
}

bool SeekQueue::waitAppliedLatest(int timeoutMs, const std::atomic<bool>* abort,
                                  const std::atomic<bool>* abort2) const {
    uint64_t target = 0;
    {
        std::lock_guard<std::mutex> lock(fMutex);
        target = fRequested;
    }
    if (target == 0) {
        return true;  // nothing was ever requested: nothing is outstanding
    }
    return waitApplied(target, timeoutMs, abort, abort2);
}

void SeekQueue::reset() {
    std::lock_guard<std::mutex> lock(fMutex);
    fTargetSeconds = 0.0;
    fRequested = 0;
    fSettled = 0;
    fApplied = 0;
}

uint64_t SeekQueue::requested() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fRequested;
}

uint64_t SeekQueue::appliedGeneration() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fApplied;
}

uint64_t SeekQueue::settledGeneration() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fSettled;
}

}  // namespace openshow::media
