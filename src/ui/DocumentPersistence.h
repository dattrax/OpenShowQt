/*
 * DocumentPersistence: the durability half of a document session (task 6.19b1).
 *
 * DocumentController owns the editable documents and their history. This class
 * owns where and when those documents are written, and everything the operator
 * is told about it: the autosave scratch and its bounded, coalescing executor
 * (task 6.14b); the task 2.3 startup rule, that a scratch is newer than the
 * saved service and so wins; the refusal surfacing that keeps the crash net
 * while making the refusal the last word (task 6.14a, review R14); the decision
 * protocol that serialises autosave against an explicit save or load; the
 * survivor report when a committed decision could not remove the scratch it had
 * just made stale; and the quit-edge promise that the durable scratch, not a
 * queued intention, is what the next session finds.
 *
 * It holds no Documents and no History. Every call takes what it writes: a
 * snapshot by const reference for an autosave, and for a save the documents
 * themselves by pointer for exactly one reason — the domain's adopt step
 * retargets each asset at the package's own copy (task 6.12, review R7) — with
 * the rebinds handed back so the caller can tell history about them. Nothing
 * else here changes a document; a load hands back a separate Documents for the
 * caller to move into its own when it is ready to give up what it had.
 *
 * The one thing it needs back from its host is the status line, injected as a
 * callback: durability has an opinion the operator reads, and no properties of
 * its own for a view to bind.
 */

#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "openshow/domain/documents.h"
#include "openshow/domain/storage.h"

#include "AutosaveScheduler.h"

namespace openshow::ui {

class DocumentPersistence : public QObject {
    Q_OBJECT
public:
    // The host's one service: the operator's one-line status. Everything this
    // class reports about durability lands there.
    struct Sink {
        std::function<void(const QString&)> statusLine;
    };

    // baseDirectory is the same directory DocumentController was given: the
    // service lives in <base>/service, the scratch in <base>/autosave.
    DocumentPersistence(Sink sink, const std::filesystem::path& baseDirectory,
                        QObject* parent = nullptr);

    const std::filesystem::path& serviceDirectory() const { return fServiceDirectory; }
    const std::filesystem::path& scratchDirectory() const { return fScheduler->directory(); }

    // The startup rule (task 2.3): autosave data, when present, is newer than
    // the saved service and wins; a missing service simply begins empty. The
    // recovered flag is the caller's dirty marker — there IS unsaved work, and
    // that is precisely why there was anything to recover. No baseline tick: a
    // tick with a virgin Autosave always writes, and a scratch package must
    // mean unsaved work, so the first committed edit is what creates it.
    struct Startup {
        bool recoveredUnsaved = false;
        QString status;
    };
    Startup open(openshow::domain::Documents* documents);

    // The autosave edge of a committed change: inline while the executor is
    // dormant (what the unit tests rely on), coalesced and off-thread once
    // started (task 6.14b). `what` names the event for the status line.
    void autosave(const openshow::domain::Documents& documents, const char* what);

    // The decision protocol (task 6.14b, review R14). Both calls quiesce the
    // executor first — queued autosaves dropped, an in-flight write waited out,
    // and the epoch raised so older jobs and acknowledgements are vetoed — do
    // their one filesystem step, and reopen the autosave path afterwards. So a
    // scratch clear here can never race an older autosave into resurrecting
    // what the decision superseded.
    //
    // A save adopts: on success the model's assets name the package's own
    // copies, *rebinds records each move, and the scratch that decision made
    // stale is removed. A failed removal of that scratch is reported in the
    // status text, never swallowed: a later startup would recover what it holds
    // as if it were unsaved work, which is R14's silent resurrection.
    struct SaveAttempt {
        bool saved = false;
        std::vector<openshow::domain::AssetRebind> rebinds;
        QString status;
    };
    SaveAttempt saveToService(openshow::domain::Documents* documents);

    // A load discards local work, so nothing older than that decision may
    // survive — queued, in flight, or acknowledged — and the trail the discarded
    // edits left is removed too, for the same resurrection reason. The documents
    // arrive separately: the caller moves them into its own when it has given up
    // what it had, and on failure it still has what it had.
    struct LoadAttempt {
        bool loaded = false;
        openshow::domain::Documents documents;
        QString status;
    };
    LoadAttempt loadFromService();

    // Durability is acknowledged on the host's thread wherever the write ran;
    // until start() the scheduler is dormant and ticks inline.
    void start();

    // Blocks until the executor is idle and lets the queued acknowledgements
    // land (fixture seam; harmless elsewhere).
    void awaitIdleAndDeliver();

    // The quit edge: durable, not queued.
    void awaitDurable();

    // Executor counters (tests and diagnostics): package writes run, and
    // requests merged into a later one or vetoed by a decision.
    qulonglong writes() const;
    qulonglong coalesced() const;

private:
    // The autosave edge of a committed decision: quiesce raises the epoch,
    // drops the pending request and waits out an in-flight write, which is the
    // window in which the scratch may be cleared; resume reopens the path for
    // the next edit.
    void quiesce();
    void resume();
    // The outcome of one autosave tick (task 6.14a, review R14). A refusal
    // means the crash net has a hole NOW and must not be discarded — but it
    // lands DEFERRED, because every edit caller sets its own status line
    // synchronously after edit() returns and last write wins. The flag
    // suppresses a queued refusal the moment the scratch is current again
    // (and says so).
    void surfaceResult(const std::string& tickError, const char* what, bool deferStatus = true);
    void onTickCompleted(const QString& what, const QString& error);

    Sink fSink;
    std::filesystem::path fServiceDirectory;
    // Held rather than parented: the executor's thread is stopped and joined in
    // ~DocumentPersistence, while this class is still itself, exactly as when
    // DocumentController owned it directly.
    std::unique_ptr<AutosaveScheduler> fScheduler;
    bool fAutosaveStale = false;  // scratch behind the model: task 6.14a
    // Decision counter (task 6.14b): bumped before every save/discard;
    // autosave work stamped below it is history and must not run or ack.
    std::uint64_t fDecisionEpoch = 0;
};

}  // namespace openshow::ui
