/*
 * DocumentPersistence implementation. The logic is the session's durability
 * contract exactly as tasks 2.3, 6.12, 6.13a, 6.14a and 6.14b earned it —
 * extracted unchanged (task 6.19b1) so that one class decides when bytes are
 * written, what a decision does to the scratch, and what the operator is told
 * about either.
 */

#include "DocumentPersistence.h"

#include <QCoreApplication>
#include <QTimer>

namespace openshow::ui {

DocumentPersistence::DocumentPersistence(Sink sink, const std::filesystem::path& baseDirectory,
                                         QObject* parent)
    : QObject(parent), fSink(std::move(sink)) {
    fServiceDirectory = baseDirectory / "service";
    fScheduler = std::make_unique<AutosaveScheduler>(baseDirectory / "autosave");

    // Task 6.14b (review R14): durability is acknowledged on the host's
    // thread, wherever the write ran.
    connect(fScheduler.get(), &AutosaveScheduler::tickCompleted, this,
            &DocumentPersistence::onTickCompleted);
}

DocumentPersistence::Startup DocumentPersistence::open(openshow::domain::Documents* documents) {
    Startup startup;
    std::string error;
    if (openshow::domain::Autosave::exists(fScheduler->directory())) {
        if (fScheduler->recover(documents, &error)) {
            startup.recoveredUnsaved = true;  // there is unsaved work — that is why we recovered
            startup.status = QStringLiteral("recovered unsaved changes from autosave");
        } else {
            startup.status = QStringLiteral("autosave recovery failed: %1; starting empty")
                                 .arg(QString::fromStdString(error));
        }
    } else if (!fServiceDirectory.empty()) {
        if (openshow::domain::Autosave::exists(fServiceDirectory)) {
            // Not the autosave package, so a real save is waiting.
            if (openshow::domain::loadService(documents, fServiceDirectory, &error)) {
                startup.status = QStringLiteral("loaded %1")
                                     .arg(QString::fromStdWString(fServiceDirectory.wstring()));
            } else {
                startup.status =
                    QStringLiteral("load failed: %1").arg(QString::fromStdString(error));
            }
        }
    }
    return startup;
}

void DocumentPersistence::autosave(const openshow::domain::Documents& documents,
                                   const char* what) {
    if (fScheduler->active()) {
        // Task 6.14b (review R14): the UI thread's whole job is the
        // snapshot copy — cheap, immutable, and the executor keeps only
        // the newest. The result reports back through
        // onTickCompleted when the write is durable (or refused).
        fScheduler->submit(documents, what, fDecisionEpoch);
        return;
    }
    std::string tickError;
    fScheduler->tickInline(documents, &tickError);
    surfaceResult(tickError, what);
}

DocumentPersistence::SaveAttempt DocumentPersistence::saveToService(
    openshow::domain::Documents* documents) {
    SaveAttempt attempt;
    // Decision protocol (task 6.14b, review R14): before this save is even
    // attempted, the executor is quiesced — queued autosaves are dropped, an
    // in-flight write finishes, and the epoch vetoes anything stamped older.
    // So the scratch clear below can never race an older autosave into
    // resurrecting what this save supersedes.
    quiesce();
    std::string error;
    if (!openshow::domain::saveServiceAndAdopt(*documents, fServiceDirectory, &attempt.rebinds,
                                               &error)) {
        resume();
        attempt.status = QStringLiteral("save failed: %1").arg(QString::fromStdString(error));
        return attempt;
    }
    attempt.saved = true;
    // The service now holds everything the autosave was protecting. No
    // re-tick after clear: clear() resets the fingerprint, so a tick would
    // immediately write a package claiming unsaved work that does not exist.
    std::string clearError;
    if (!fScheduler->clearNow(&clearError)) {
        // The save COMMITTED; what failed is deleting the scratch the
        // save made stale. A later startup would recover what it holds as
        // if it were unsaved work — R14's silent resurrection — so the
        // survivor must be named, not swallowed.
        attempt.status =
            QStringLiteral("saved %1, but the stale autosave could NOT be removed (%2); "
                           "a later start may recover work this save already replaced")
                .arg(QString::fromStdWString(fServiceDirectory.wstring()),
                     QString::fromStdString(clearError));
    } else {
        attempt.status = QStringLiteral("saved %1")
                             .arg(QString::fromStdWString(fServiceDirectory.wstring()));
    }
    resume();
    return attempt;
}

DocumentPersistence::LoadAttempt DocumentPersistence::loadFromService() {
    LoadAttempt attempt;
    // Same decision protocol as a save (task 6.14b): this load DISCARDS local
    // work, and no autosave older than that decision may survive — queued, in
    // flight, or acknowledged.
    quiesce();
    std::string error;
    if (!openshow::domain::loadService(&attempt.documents, fServiceDirectory, &error)) {
        resume();
        attempt.status = QStringLiteral("load failed: %1").arg(QString::fromStdString(error));
        return attempt;
    }
    attempt.loaded = true;
    // Discarding local changes must also discard their autosave trail —
    // otherwise the next startup would recover the edits just thrown away.
    std::string clearError;
    if (!fScheduler->clearNow(&clearError)) {
        // Same contract as a save (task 6.14a, R14): the load itself
        // succeeded; the undeleted scratch is a resurrection risk and is
        // reported, never silently kept.
        attempt.status =
            QStringLiteral("loaded %1, but the stale autosave could NOT be removed (%2); "
                           "a later start may recover work this load just discarded")
                .arg(QString::fromStdWString(fServiceDirectory.wstring()),
                     QString::fromStdString(clearError));
    } else {
        attempt.status = QStringLiteral("loaded %1")
                             .arg(QString::fromStdWString(fServiceDirectory.wstring()));
    }
    resume();
    return attempt;
}

void DocumentPersistence::start() {
    fScheduler->start();
}

void DocumentPersistence::awaitIdleAndDeliver() {
    if (fScheduler->active()) {
        fScheduler->awaitIdle();
        QCoreApplication::processEvents();  // land the queued acknowledgements
    }
}

void DocumentPersistence::awaitDurable() {
    // The shutdown promise is durable, not queued: whatever the host flushed
    // must be on disk before the process is gone.
    if (fScheduler->active()) {
        fScheduler->awaitIdle();
    }
}

qulonglong DocumentPersistence::writes() const {
    return static_cast<qulonglong>(fScheduler->writes());
}

qulonglong DocumentPersistence::coalesced() const {
    return static_cast<qulonglong>(fScheduler->coalesced());
}

void DocumentPersistence::quiesce() {
    ++fDecisionEpoch;
    fScheduler->pauseForDecision(fDecisionEpoch);
}

void DocumentPersistence::resume() {
    fScheduler->resume(fDecisionEpoch);
}

void DocumentPersistence::surfaceResult(const std::string& tickError, const char* what,
                                        bool deferStatus) {
    if (!tickError.empty()) {
        fAutosaveStale = true;
        const QString message =
            QStringLiteral("autosave refused after %1: %2 - unsaved work is NOT on disk")
                .arg(QString::fromLatin1(what), QString::fromStdString(tickError));
        if (!deferStatus) {
            // The background ack has no caller that will overwrite the
            // line afterwards — it is already the last word.
            fSink.statusLine(message);
            return;
        }
        // Deferred, so the refusal is the LAST word: edit callers set
        // their own status synchronously after edit() returns. The flag
        // check is the race guard — if the scratch caught up before this
        // timer ran, the stale warning must not land.
        QTimer::singleShot(0, this, [this, message] {
            if (fAutosaveStale) {
                fSink.statusLine(message);
            }
        });
        return;
    }
    if (fAutosaveStale) {
        // A successful tick means the scratch on disk matches the model
        // again: either this call wrote it, or the fingerprint proves the
        // model never drifted past the last good write.
        fAutosaveStale = false;
        fSink.statusLine(QStringLiteral("autosave recovered - the scratch on disk is current "
                                        "again"));
    }
}

void DocumentPersistence::onTickCompleted(const QString& what, const QString& error) {
    // No caller writes the status after this signal, so the refusal lands
    // immediately (the deferral exists only for the inline edit path).
    surfaceResult(error.toStdString(), what.toLatin1().constData(), false);
}

}  // namespace openshow::ui
