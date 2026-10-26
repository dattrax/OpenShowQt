/*
 * AutosaveScheduler — the bounded, coalescing scratch-persistence
 * executor (task 6.14b, review R14).
 *
 * R14 measured the old shape: every edit, undo and redo re-saved the
 * whole scratch package synchronously on the UI thread, so a session
 * holding large media froze navigation after every keystroke burst — and
 * an autosave still in flight when the operator saved or discarded could
 * land afterwards and resurrect the very scratch that decision had
 * removed.
 *
 * The executor is BOUNDED: one job runs at a time (scratch writes must
 * serialise against each other), and at most one request waits behind
 * it. A submit() while a request is already pending REPLACES it — the
 * crash net only ever needs the freshest state, and the snapshots are
 * immutable copies taken on the UI thread, so a discarded request loses
 * nothing.
 *
 * The DECISION protocol serialises autosave against save()/discard:
 * pauseForDecision() drops the pending request and blocks until an
 * in-flight write has completed, and the caller may touch the scratch
 * (clear it) only while paused; the epoch counter additionally vetoes
 * any job or acknowledgement whose snapshot predates the decision, so
 * an old write can neither land after the clear nor report itself as
 * though it still mattered.
 *
 * Until start() the scheduler is DORMANT: tickInline() runs on the
 * calling thread, which is the pre-6.14b behaviour — production main()
 * starts the thread, and unit tests stay deterministic unless they ask
 * for the threaded contract by name.
 */

#pragma once

#include <QObject>
#include <QString>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "openshow/domain/documents.h"
#include "openshow/domain/storage.h"

namespace openshow::ui {

class AutosaveScheduler : public QObject {
    Q_OBJECT
public:
    explicit AutosaveScheduler(std::filesystem::path directory, QObject* parent = nullptr);
    ~AutosaveScheduler() override;  // stops the worker and joins

    const std::filesystem::path& directory() const { return fAutosave.directory(); }

    // Synchronous operations, valid only while the worker is neither
    // running nor about to run: before start(), or between
    // pauseForDecision() and resume().
    bool recover(openshow::domain::Documents* documents, std::string* error);
    bool clearNow(std::string* error);
    bool tickInline(const openshow::domain::Documents& documents, std::string* error);

    void start();
    bool active() const { return fThread.joinable(); }

    // Queue a snapshot as "the latest". `epoch` is the caller's decision
    // counter: a request stamped below the current decision epoch (or
    // arriving while paused) is dropped unrun, counted, never acked.
    void submit(const openshow::domain::Documents& snapshot, const std::string& what,
                std::uint64_t epoch);

    // Raise the decision epoch, drop the pending request, and block until
    // no write is in flight. Between this and resume(), the caller owns
    // the scratch alone.
    void pauseForDecision(std::uint64_t decisionEpoch);
    void resume(std::uint64_t decisionEpoch);

    // Block until nothing is running or pending. On quit this is the
    // promise that the durable scratch, not a queued intention, is what
    // the next session finds.
    void awaitIdle();

    // Counters for tests and telemetry: package writes actually run, and
    // requests that were merged into a later one or vetoed by a decision.
    std::uint64_t writes() const;
    std::uint64_t coalesced() const;

signals:
    // Delivery lands on the receiver's thread (queued from the worker):
    // durability is acknowledged back where the operator reads it.
    void tickCompleted(const QString& what, const QString& error);

private:
    void run();

    openshow::domain::Autosave fAutosave;
    std::thread fThread;
    mutable std::mutex fMutex;
    std::condition_variable fCv;

    struct Request {
        openshow::domain::Documents documents;
        std::string what;
        std::uint64_t epoch = 0;
    };
    std::optional<Request> fPending;
    bool fRunning = false;  // a job holds the Autosave object
    bool fPaused = false;
    bool fStop = false;
    std::uint64_t fDecisionEpoch = 0;
    std::uint64_t fWrites = 0;
    std::uint64_t fCoalesced = 0;
};

}  // namespace openshow::ui
