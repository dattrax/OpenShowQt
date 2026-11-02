// The Bible importer's page session (milestone BI, task BI.5).
//
// The lookup (BibleImport, BI.2) hands over an answered passage; this class
// owns everything the page does with it: measured pagination through the
// resolved theme's box (BI.4), the OFF-AIR picture of each generated slide
// (the same selection-preview transport the edit canvas draws with — this
// never presents), and the single destination action, Add, which commits the
// passage to today's schedule through DocumentController (BI.3) and nothing
// else. There is deliberately no verb here for the Library, for a present,
// or for a second destination: the page cannot offer what it does not own,
// and the plan forbids it.
//
// The one interesting shape is the measure port. paginateScripture is a
// synchronous pure function; WorkerController::measureText answers when the
// worker replies. The adapter therefore never blocks: a cache miss SENDS the
// batch and aborts the run with a pending marker; the reply fills a
// content-keyed cache and the run restarts from the top, where every earlier
// ask is now a cache hit. Each round sends at most one batch and gets
// strictly further, so the convergence is the pagination's own determinism —
// no nested event loops, no blocking the operator out, and every measurement
// still answered by the worker's SkParagraph alone.
#ifndef OPENSHOW_UI_SCRIPTUREIMPORT_H
#define OPENSHOW_UI_SCRIPTUREIMPORT_H

#include <QObject>
#include <QVariantList>
#include <map>
#include <string>
#include <vector>

#include "openshow/importer/scripturepagination.h"

class BibleImport;

namespace openshow::ui {

class DocumentController;
class WorkerController;

class ScriptureImport final : public QObject {
    Q_OBJECT
    // True while the slides are being measured (a batch is in flight). The
    // page's Add sleeps for exactly as long as this says.
    Q_PROPERTY(bool measuring READ measuring NOTIFY stateChanged)
    // The importer's own sentence: why a run failed (actionable, in the
    // lookup's words where the lookup is at fault), or what it produced.
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(bool canAdd READ canAdd NOTIFY stateChanged)
    // The generated slides: {lines: [QString], label: QString}. `label` is
    // the verse range for the slide's NOTES — the slide's words carry only
    // the passage, the plan's rule, all the way through the page.
    Q_PROPERTY(QVariantList slides READ slides NOTIFY slidesChanged)

public:
    ScriptureImport(BibleImport* bible, DocumentController* documents,
                    WorkerController* worker, QObject* parent = nullptr);

    bool measuring() const { return fMeasuring || fPendingMeasureId != 0; }
    QString status() const { return fStatus; }
    bool canAdd() const { return !fSlides.isEmpty() && !measuring(); }
    QVariantList slides() const { return fSlides; }

    // Rebuild the generated slides from the current selection and the
    // CURRENT theme: a pure function of those inputs, so selection changes,
    // theme changes and lookups all come through this one entry and never
    // touch the network or the schedule.
    Q_INVOKABLE void regenerate();
    // The off-air picture of one generated slide, drawn by the worker into
    // the preview stream it already owns. Nothing is prepared for air and
    // nothing is presented.
    Q_INVOKABLE void previewSlide(int index);
    // The plan's one destination action: today's schedule only, one
    // undoable item, labels in notes, and a successful Add consumes the
    // generated batch so the next click cannot insert a copy of it.
    Q_INVOKABLE bool addToSchedule();
    // Discard the transient work — fetched words and generated slides. The
    // schedule is not touched by this (or by anything else before Add).
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void slidesChanged();

private:
    // The pagination run, restartable: each entry walks paginateScripture
    // against the cache; a miss sends its batch and suspends the loop.
    void runPagination();
    bool measure(const std::vector<openshow::importer::MeasureRequest>& requests,
                 std::vector<openshow::importer::MeasureReply>* replies, std::string* error);
    void publish(const std::vector<openshow::importer::GeneratedSlide>& generated,
                 const openshow::importer::PaginationOutcome& outcome);
    void finishWithFailure(const QString& sentence);
    void setStatus(const QString& sentence);

    BibleImport* fBible = nullptr;
    DocumentController* fDocuments = nullptr;
    WorkerController* fWorker = nullptr;

    // Content-AND-size keyed across the rounds of one run: an identical
    // block is measured once at a given size (the words and the caption
    // are the same words set twice, so they are two measured answers), and
    // the run's determinism is what makes the restart converge instead of
    // churn. Cleared whenever the inputs change.
    std::map<std::pair<std::vector<std::string>, double>, double> fMeasured;
    // The batch in flight: the worker's reply keys mapped back to the
    // content and size they measured, so the cache is keyed by words, not
    // by ids.
    qint64 fPendingMeasureId = 0;
    std::map<std::string, std::pair<std::vector<std::string>, double>> fPendingSends;
    bool fMeasuring = false;
    bool fRerunWanted = false;  // asked again while suspended: the newest wins
    QVariantList fSlides;
    QString fStatus;
};

}  // namespace openshow::ui

#endif  // OPENSHOW_UI_SCRIPTUREIMPORT_H
