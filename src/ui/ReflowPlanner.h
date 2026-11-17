/*
 * Optional reflow against a chosen future appearance (task PT.5).
 *
 * The plan is a PREPARATION, never a mutation: this object reads the item's
 * bounded source (or the conservative current-word source), measures the
 * words in the chosen look's box through the worker, and publishes a list
 * of slides for a later commit. Cancelling, failing, a superseded run or a
 * worker that goes away leaves the document exactly as it was.
 *
 * Measurement discipline is SongPlanner's: a cache miss sends one bounded
 * batch and suspends, the reply fills the cache, and the pure packer runs
 * again from the top. The run is keyed by a generation, so a late answer
 * can only ever complete the newest ask.
 *
 * The style is captured at prepare() from the portable snapshot the caller
 * is about to apply, so a browsing cursor or a later theme selection cannot
 * change what was measured. The words are re-checked when the plan lands:
 * if the item's text projection changed while it was being measured, the
 * plan is refused rather than silently reflowing stale words.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <map>
#include <string>
#include <vector>

#include "openshow/domain/refloweligibility.h"
#include "openshow/domain/reflowsource.h"
#include "openshow/importer/reflowpagination.h"

namespace openshow::ui {

class DocumentController;
class WorkerController;

class ReflowPlanner final : public QObject {
    Q_OBJECT
public:
    ReflowPlanner(DocumentController* documents, WorkerController* worker,
                  QObject* parent = nullptr);

    // Task PT.5: the one eligibility answer, without measuring anything.
    Q_INVOKABLE QVariantMap eligibility(const QString& showId) const;

    // Prepare (or supersede) a plan for `showId` against the portable look
    // in `snapshotJson` (the codec's JSON). Returns false with `prepared`
    // already emitted when the item cannot be reflowed, the look has no
    // usable box, or no measuring worker is connected. Otherwise returns
    // true and emits `prepared` once, later.
    Q_INVOKABLE bool prepare(const QString& showId, const QString& snapshotJson);

    // Drop any plan and retire an in-flight run. Never touches documents.
    Q_INVOKABLE void cancel();

    // The current plan, empty when there is none. See the .cpp for keys.
    Q_INVOKABLE QVariantMap plan() const;

    bool measuring() const { return fMeasuring || fPendingMeasureId != 0; }
    bool hasPlan() const { return fHasPlan; }

signals:
    void prepared(bool ok, const QString& error);
    void stateChanged();

private:
    void run();
    void finishWithFailure(const QString& reason);
    bool measure(const std::vector<openshow::importer::MeasureRequest>& requests,
                 std::vector<openshow::importer::MeasureReply>* replies,
                 std::string* error);
    void setMeasuring(bool measuring);

    struct Answer {
        double height = 0.0;
        int fits = -1;  // -1 unknown, 0 false, 1 true
    };

    DocumentController* fDocuments = nullptr;
    WorkerController* fWorker = nullptr;

    // The captured run.
    QString fShowId;
    std::string fExpectedFingerprint;
    quint64 fDocumentGeneration = 0;
    openshow::domain::ReflowEligibility fEligibility;
    openshow::importer::ReflowPaginationInput fInput;

    // The result.
    std::vector<openshow::importer::ReflowSlide> fSlides;
    double fReservedHeight = 0.0;
    bool fHasPlan = false;
    QString fPlanError;

    // Measurement.
    bool fMeasuring = false;
    bool fRestartWanted = false;
    quint64 fGeneration = 0;
    qint64 fPendingMeasureId = 0;
    std::map<std::string, std::string> fPendingSends;  // wire key -> cache key
    std::map<std::string, Answer> fMeasured;
};

}  // namespace openshow::ui
