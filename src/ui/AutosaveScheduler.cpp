#include "AutosaveScheduler.h"

#include <utility>

namespace openshow::ui {

AutosaveScheduler::AutosaveScheduler(std::filesystem::path directory, QObject* parent)
    : QObject(parent), fAutosave(std::move(directory)) {}

AutosaveScheduler::~AutosaveScheduler() {
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fStop = true;
        fPending.reset();
    }
    fCv.notify_all();
    if (fThread.joinable()) {
        fThread.join();
    }
}

bool AutosaveScheduler::recover(openshow::domain::Documents* documents, std::string* error) {
    return fAutosave.recover(documents, error);
}

bool AutosaveScheduler::clearNow(std::string* error) {
    return fAutosave.clear(error);
}

bool AutosaveScheduler::tickInline(const openshow::domain::Documents& documents,
                                   std::string* error) {
    return fAutosave.tick(documents, error);
}

void AutosaveScheduler::start() {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fThread.joinable()) {
        fThread = std::thread(&AutosaveScheduler::run, this);
    }
}

void AutosaveScheduler::submit(const openshow::domain::Documents& snapshot,
                               const std::string& what, std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (fStop || fPaused || epoch < fDecisionEpoch) {
        // A decision is in progress (or the whole session is ending):
        // this snapshot died at the checkpoint, not on disk.
        ++fCoalesced;
        return;
    }
    if (fPending) {
        ++fCoalesced;  // the fresher snapshot replaces the queued one
    }
    fPending = Request{snapshot, what, epoch};
    fCv.notify_one();
}

void AutosaveScheduler::pauseForDecision(std::uint64_t decisionEpoch) {
    std::unique_lock<std::mutex> lock(fMutex);
    fDecisionEpoch = decisionEpoch;
    fPaused = true;
    fPending.reset();
    // The in-flight write finishes (bounded: one package), and it is
    // done BEFORE the decision-maker touches the scratch — that ordering
    // is what makes "an older write cannot overwrite a newer decision"
    // structural rather than lucky.
    fCv.wait(lock, [this] { return !fRunning; });
}

void AutosaveScheduler::resume(std::uint64_t decisionEpoch) {
    std::lock_guard<std::mutex> lock(fMutex);
    fDecisionEpoch = decisionEpoch;
    fPaused = false;
    fCv.notify_all();
}

void AutosaveScheduler::awaitIdle() {
    std::unique_lock<std::mutex> lock(fMutex);
    fCv.wait(lock, [this] { return !fRunning && !fPending.has_value(); });
}

std::uint64_t AutosaveScheduler::writes() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fWrites;
}

std::uint64_t AutosaveScheduler::coalesced() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fCoalesced;
}

void AutosaveScheduler::run() {
    for (;;) {
        Request job;
        {
            std::unique_lock<std::mutex> lock(fMutex);
            fCv.wait(lock, [this] { return fStop || (fPending.has_value() && !fPaused); });
            if (fStop) {
                return;
            }
            job = std::move(*fPending);
            fPending.reset();
            fRunning = true;
        }
        std::string error;
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(fMutex);
            stale = job.epoch < fDecisionEpoch;
        }
        if (!stale) {
            fAutosave.tick(job.documents, &error);  // the slow part, off the UI thread
            std::lock_guard<std::mutex> lock(fMutex);
            ++fWrites;
            // A decision may have been made while this write ran; if so
            // its ack is history — the scratch it reports on is gone or
            // about to be, and the operator has since heard about the
            // save instead.
            stale = job.epoch < fDecisionEpoch;
        }
        {
            std::lock_guard<std::mutex> lock(fMutex);
            fRunning = false;
        }
        // The ack is queued BEFORE the job stops counting as running:
        // awaitIdle() may only return once the acknowledgement is
        // already in the UI thread's queue.
        if (!stale) {
            emit tickCompleted(QString::fromStdString(job.what), QString::fromStdString(error));
        }
        fCv.notify_all();
    }
}

}  // namespace openshow::ui
