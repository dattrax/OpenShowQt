/*
 * The shared song planning runner (tasks SI.2/SI.3/SL.6).
 *
 * paginateSong is a synchronous pure function; WorkerController::measureText
 * answers when the worker replies. This adapter therefore never blocks: a
 * cache miss SENDS one bounded batch and suspends with a pending marker; the
 * reply fills the content-keyed cache and the run restarts from the top, where
 * every earlier ask is now a hit. Each round sends at most one batch and gets
 * strictly further. The same pattern the Bible importer uses, kept in one
 * place now that two song surfaces need it (the import page's preview and the
 * service bridge's preparation).
 *
 * The style is captured at start() from the current service's resolved Songs
 * view; callers restart when the theme changes. A later start() while a batch
 * is in flight supersedes it: the reply is retired, the cache cleared, and the
 * newest ask runs.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <map>
#include <string>
#include <vector>

#include "openshow/catalog/catalog.h"
#include "openshow/importer/songpagination.h"

namespace openshow::ui {

class DocumentController;
class WorkerController;

// Build the planner input for `stanzas` from the service's resolved Songs
// view (box, size, family, spacing, weight/style/fit, effects). Returns false
// when the chain compiles to no usable text box; `out->style` then carries
// zeros and paginateSong refuses with its own sentence.
bool songInputFor(DocumentController* documents,
                  const std::vector<openshow::catalog::Stanza>& stanzas,
                  openshow::importer::SongPaginationInput* out);

class SongPlanner final : public QObject {
    Q_OBJECT
public:
    SongPlanner(DocumentController* documents, WorkerController* worker,
                QObject* parent = nullptr);

    // Captures the current Songs style and starts planning. Any plan in
    // flight is superseded. finished() follows exactly once per start.
    void start(const std::vector<openshow::catalog::Stanza>& stanzas,
               const openshow::importer::SongPaginationInput* captured = nullptr);
    void cancel();

    bool measuring() const { return fMeasuring || fPendingMeasureId != 0; }
    bool hasPlan() const { return fHasPlan; }
    const std::vector<openshow::importer::SongSlide>& slides() const {
        return fSlides;
    }
    const std::string& fingerprint() const { return fFingerprint; }

signals:
    // ok=false carries the operator's sentence; a superseded run emits
    // nothing at all (its caller has already started a newer one).
    void finished(bool ok, const QString& error);

private:
    void run();
    void finishWithFailure(const QString& reason);
    bool measure(const std::vector<openshow::importer::MeasureRequest>& requests,
                 std::vector<openshow::importer::MeasureReply>* replies,
                 std::string* error);

    struct Answer {
        double height = 0.0;
        int fits = -1;  // -1 unknown, 0 false, 1 true
    };

    DocumentController* fDocuments = nullptr;
    WorkerController* fWorker = nullptr;
    openshow::importer::SongPaginationInput fInput;
    std::vector<openshow::importer::SongSlide> fSlides;
    std::string fFingerprint;
    bool fHasPlan = false;
    bool fMeasuring = false;
    bool fRestartWanted = false;
    quint64 fGeneration = 0;
    qint64 fPendingMeasureId = 0;
    std::map<std::string, std::string> fPendingSends;  // wire key -> cache key
    std::map<std::string, Answer> fMeasured;
};

}  // namespace openshow::ui
