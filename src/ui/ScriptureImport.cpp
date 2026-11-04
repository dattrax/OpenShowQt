// The Bible importer's page session (milestone BI, task BI.5). See the
// header for the shape; this file is that shape carried out: selection in,
// worker-measured slides out, one honest off-air picture, and one door to
// the schedule.
#include "ScriptureImport.h"

#include <QTimer>
#include <QStringList>
#include <QVariantMap>

#include "BibleImport.h"
#include "DocumentController.h"
#include "WorkerController.h"
#include "openshow/domain/documents.h"

namespace {

namespace importer = openshow::importer;

// The run-abort that means "not failed, awaiting a batch". Distinctive and
// never a sentence shown to the operator: the reply resumes the run, and
// only a genuine failure reaches the status line.
constexpr const char* kPendingMarker = "\x01awaiting-measurement";

// The renderer answered once per batch round trip; the budget is a whole
// minute of silence, which is a dead or wedged output and not a slow one.
// It exists so the "measuring" state always has an exit the operator can
// act on.
constexpr int kMeasureAnswerBudgetMs = 60000;

// A measure round answers at least one new block or the run is not making
// progress; the bound turns any pathological engine into an honest failure
// rather than a silent spin. The honest runs need far fewer rounds than
// this (each is one batch of up to the wire's cap).
constexpr int kMaxMeasureRounds = 500;

}  // namespace

namespace openshow::ui {

ScriptureImport::ScriptureImport(BibleImport* bible, DocumentController* documents,
                                 WorkerController* worker, QObject* parent)
    : QObject(parent), fBible(bible), fDocuments(documents), fWorker(worker) {
    // A new answer or a changed selection IS new input to the pure
    // pagination: it re-runs, off the network and off the schedule.
    connect(fBible, &BibleImport::passageChanged, this, &ScriptureImport::regenerate);
    connect(fWorker, &WorkerController::textMeasured, this,
            [this](qint64 id, const QVariantList& rows) {
                if (id != fPendingMeasureId) {
                    return;  // an ask this page already retired
                }
                fPendingMeasureId = 0;
                for (const QVariant& row : rows) {
                    const QVariantMap map = row.toMap();
                    const auto found =
                        fPendingSends.find(map.value(QStringLiteral("key")).toString().toStdString());
                    if (found == fPendingSends.end()) {
                        continue;  // a row for something else measures nothing
                    }
                    MeasuredAnswer answer;
                    answer.height = map.value(QStringLiteral("height")).toDouble();
                    // TS.3's verdict rides the reply when the worker sends
                    // one; a v1 answer stays UNKNOWN (-1), which the flow
                    // judges by height alone — never a fabricated yes.
                    const QVariant fits = map.value(QStringLiteral("fits"));
                    if (fits.isValid()) {
                        answer.fits = fits.toBool() ? 1 : 0;
                    }
                    if (fMeasured.size() >= importer::kMaxMeasureCacheEntries) {
                        fMeasured.clear();  // bounded: re-ask rather than grow
                    }
                    fMeasured[found->second] = answer;
                }
                fPendingSends.clear();
                if (fRerunWanted) {
                    // The inputs moved while a batch was in flight: the
                    // newest ask wins outright, and the arriving numbers
                    // (measured for the older question) are discarded with
                    // the cache.
                    fRerunWanted = false;
                    fMeasured.clear();
                }
                runPagination();
            });
    connect(fWorker, &WorkerController::textMeasureFailed, this, [this](qint64 id,
                                                                        const QString& reason) {
        if (id != fPendingMeasureId) {
            return;
        }
        fPendingMeasureId = 0;
        fPendingSends.clear();
        finishWithFailure(reason.isEmpty()
                              ? QStringLiteral("the renderer would not answer the measurement")
                              : reason);
    });
    connect(fWorker, &WorkerController::stateChanged, this, [this] {
        if (fWorker->isConnected() || fPendingMeasureId == 0) {
            return;
        }
        // A batch can come back only from the connection it left on. With
        // no worker there is no measurer, and leaving the page "measuring"
        // forever would be a failure the operator cannot act on.
        fPendingMeasureId = 0;
        fPendingSends.clear();
        finishWithFailure(QStringLiteral(
            "the output went away mid-measurement — no slides were generated; "
            "try again once the worker is back"));
    });
}

void ScriptureImport::regenerate() {
    if (measuring()) {
        fRerunWanted = true;  // the newest ask wins, once this batch answers
        return;
    }
    fMeasured.clear();
    fRerunWanted = false;
    if (fBible->selectedVerses().empty()) {
        // Not a failure: the lookup line is already saying where the
        // operator stands (the notice, a refusal, or a passage awaiting a
        // selection). The slides are simply none.
        fContext.clear();
        if (!fSlides.isEmpty()) {
            fSlides.clear();
            emit slidesChanged();
        }
        setStatus(QString());
        return;
    }
    runPagination();
}

void ScriptureImport::runPagination() {
    fMeasuring = true;
    emit stateChanged();

    importer::PaginationInput input;
    for (const importer::PassageVerse& verse : fBible->selectedVerses()) {
        input.verses.push_back(importer::PaginationVerse{verse.reference, verse.text});
    }
    input.reference = fBible->reference().trimmed().toStdString();
    input.attribution = fBible->attribution().trimmed().toStdString();
    // The box and typography the CHAIN resolves for a slide that wears
    // nothing of its own — the exact state the added passage will be in.
    // fontSize 0 stays 0: the renderer's default is the worker's number,
    // and the measurement will answer in it.
    const QVariantMap style = fDocuments->scriptureBoxStyle();
    input.style.boxWidth = style.value(QStringLiteral("boxWidth")).toDouble();
    input.style.boxHeight = style.value(QStringLiteral("boxHeight")).toDouble();
    input.style.fontSize = style.value(QStringLiteral("fontSize")).toDouble();
    input.style.lineSpacing = style.contains(QStringLiteral("lineSpacing"))
                                  ? style.value(QStringLiteral("lineSpacing")).toDouble()
                                  : 1.25;
    input.style.fontFamily = style.value(QStringLiteral("fontFamily")).toString().toStdString();
    // Task TS.3: the style block the same chain resolved for the item
    // these words will fill — scriptureBoxStyle reads it off the compiled
    // fixture's FIRST TEXT ITEM, the very item scriptureSlideSceneJson
    // draws, so the packer's question and the audience's slide answer to
    // one typography by construction. Absent keys stay contract defaults,
    // which is what a pre-TS.3 fixture answers with.
    input.style.fontWeight = style.contains(QStringLiteral("fontWeight"))
                                 ? style.value(QStringLiteral("fontWeight")).toInt()
                                 : openshow::domain::kDefaultFontWeight;
    input.style.fontStyle = style.contains(QStringLiteral("fontStyle"))
                                ? style.value(QStringLiteral("fontStyle")).toString().toStdString()
                                : "normal";
    input.style.fitMode = style.contains(QStringLiteral("fitMode"))
                              ? style.value(QStringLiteral("fitMode")).toString().toStdString()
                              : "fixed";
    input.style.minFontSize = style.contains(QStringLiteral("minFontSize"))
                                  ? style.value(QStringLiteral("minFontSize")).toDouble()
                                  : openshow::domain::kDefaultMinFontSize;
    if (style.contains(QStringLiteral("stroke"))) {
        const QVariantMap stroke = style.value(QStringLiteral("stroke")).toMap();
        input.style.stroke.enabled = stroke.value(QStringLiteral("enabled")).toBool();
        input.style.stroke.color = stroke.value(QStringLiteral("color")).toString().toStdString();
        if (stroke.contains(QStringLiteral("width"))) {
            input.style.stroke.width = stroke.value(QStringLiteral("width")).toDouble();
        }
    }
    if (style.contains(QStringLiteral("shadow"))) {
        const QVariantMap shadow = style.value(QStringLiteral("shadow")).toMap();
        input.style.shadow.enabled = shadow.value(QStringLiteral("enabled")).toBool();
        input.style.shadow.color = shadow.value(QStringLiteral("color")).toString().toStdString();
        if (shadow.contains(QStringLiteral("opacity"))) {
            input.style.shadow.opacity = shadow.value(QStringLiteral("opacity")).toDouble();
        }
        if (shadow.contains(QStringLiteral("offsetX"))) {
            input.style.shadow.offsetX = shadow.value(QStringLiteral("offsetX")).toDouble();
        }
        if (shadow.contains(QStringLiteral("offsetY"))) {
            input.style.shadow.offsetY = shadow.value(QStringLiteral("offsetY")).toDouble();
        }
        if (shadow.contains(QStringLiteral("blurSigma"))) {
            input.style.shadow.blurSigma = shadow.value(QStringLiteral("blurSigma")).toDouble();
        }
    }
    // Which context answered the chain (task TF.4). "" means none did and
    // the words wear the theme's own look; the plan sentence says exactly
    // which of the two the operator is about to add.
    fContext = style.value(QStringLiteral("context")).toString();

    for (int round = 1; round <= kMaxMeasureRounds; ++round) {
        const importer::PaginationOutcome outcome = importer::paginateScripture(
            input, [this](const std::vector<importer::MeasureRequest>& requests,
                          std::vector<importer::MeasureReply>* replies, std::string* error) {
                return measure(requests, replies, error);
            });
        if (outcome.ok) {
            publish(outcome.slides, outcome);
            return;
        }
        if (outcome.error.find(kPendingMarker) != std::string::npos) {
            // Suspended, not failed. measuring() still says true (a batch
            // is in flight), the Add stays asleep, and the reply resumes
            // this loop from the top of a fresh, cache-fed run.
            fMeasuring = false;
            emit stateChanged();
            return;
        }
        finishWithFailure(QString::fromStdString(outcome.error));
        return;
    }
    finishWithFailure(QStringLiteral(
        "the words did not settle into slides — try a smaller selection"));
}

bool ScriptureImport::measure(const std::vector<importer::MeasureRequest>& requests,
                              std::vector<importer::MeasureReply>* replies,
                              std::string* error) {
    QVariantList blocks;
    for (const importer::MeasureRequest& request : requests) {
        const std::string cacheKey = importer::measureCacheKey(request);
        const auto cached = fMeasured.find(cacheKey);
        if (cached != fMeasured.end()) {
            importer::MeasureReply reply;
            reply.key = request.key;
            reply.height = cached->second.height;
            reply.lineCount = 0;  // the fitting reads the height; nothing else
            if (cached->second.fits >= 0) {
                reply.fits = cached->second.fits == 1;
            }
            replies->push_back(std::move(reply));
            continue;
        }
        QVariantMap block;
        block.insert(QStringLiteral("key"), QString::fromStdString(request.key));
        QVariantList lines;
        for (const std::string& line : request.lines) {
            lines.push_back(QString::fromStdString(line));
        }
        block.insert(QStringLiteral("lines"), lines);
        block.insert(QStringLiteral("fontSize"), request.fontSize);
        block.insert(QStringLiteral("width"), request.width);
        block.insert(QStringLiteral("lineSpacing"), request.lineSpacing);
        if (!request.fontFamily.empty()) {
            block.insert(QStringLiteral("fontFamily"),
                         QString::fromStdString(request.fontFamily));
        }
        // Task TS.3: the whole resolved style rides every ask, so the
        // numbers come back from the typography the packed slides will
        // wear. The records go out complete (the wire treats a partial
        // effect record as a refusal, and the chain always knows both
        // answers); the controller validates the bands and refuses a
        // style-deviating batch toward a worker without text.measure.v2.
        block.insert(QStringLiteral("fontWeight"), request.fontWeight);
        block.insert(QStringLiteral("fontStyle"), QString::fromStdString(request.fontStyle));
        block.insert(QStringLiteral("fitMode"), QString::fromStdString(request.fitMode));
        block.insert(QStringLiteral("minFontSize"), request.minFontSize);
        QVariantMap stroke;
        stroke.insert(QStringLiteral("enabled"), request.stroke.enabled);
        stroke.insert(QStringLiteral("color"), QString::fromStdString(request.stroke.color));
        stroke.insert(QStringLiteral("width"), request.stroke.width);
        block.insert(QStringLiteral("stroke"), stroke);
        QVariantMap shadow;
        shadow.insert(QStringLiteral("enabled"), request.shadow.enabled);
        shadow.insert(QStringLiteral("color"), QString::fromStdString(request.shadow.color));
        shadow.insert(QStringLiteral("opacity"), request.shadow.opacity);
        shadow.insert(QStringLiteral("offsetX"), request.shadow.offsetX);
        shadow.insert(QStringLiteral("offsetY"), request.shadow.offsetY);
        shadow.insert(QStringLiteral("blurSigma"), request.shadow.blurSigma);
        block.insert(QStringLiteral("shadow"), shadow);
        blocks.push_back(block);
        fPendingSends[request.key] = cacheKey;
    }
    if (blocks.isEmpty()) {
        return true;  // the whole batch was cache
    }
    if (fPendingMeasureId != 0) {
        // Defensive: one run never has two batches out. (runPagination
        // suspends on the first miss, so reaching here means a caller bug,
        // and an honest abort beats a second unanswered ask.)
        *error = kPendingMarker;
        return false;
    }
    const qint64 id = fWorker->measureText(blocks);
    if (id == 0) {
        // measureText refused WITHOUT a round trip and said why in the
        // worker's status line (no connection, no capability, a broken
        // block). That sentence is actionable, and it belongs here too:
        // the importer's page reads this one.
        fPendingSends.clear();
        *error = "the words could not be put to the renderer: " +
                 fWorker->statusText().toStdString();
        return false;
    }
    fPendingMeasureId = id;
    // BIF.1: a batch that is never answered must retire with its own
    // sentence — "measuring forever" is the one state the operator can do
    // nothing about. A reply for a retired id is already ignored by the
    // handlers above, so winning or losing this race is safe either way.
    QTimer::singleShot(kMeasureAnswerBudgetMs, this, [this, id] {
        if (fPendingMeasureId != id) {
            return;  // answered, retired or superseded — this ask is done
        }
        fPendingMeasureId = 0;
        fPendingSends.clear();
        finishWithFailure(QStringLiteral(
            "the renderer did not answer the measurement — no slides were generated; "
            "try again, and if it keeps silence check the output device"));
    });
    *error = kPendingMarker;
    return false;
}

void ScriptureImport::publish(const std::vector<importer::GeneratedSlide>& generated,
                              const importer::PaginationOutcome& outcome) {
    fSlides.clear();
    for (const importer::GeneratedSlide& slide : generated) {
        QVariantMap row;
        QStringList lines;
        for (const std::string& line : slide.lines) {
            lines.push_back(QString::fromStdString(line));
        }
        row.insert(QStringLiteral("lines"), lines);
        row.insert(QStringLiteral("label"), QString::fromStdString(slide.label));
        fSlides.push_back(row);
    }
    fMeasuring = false;
    if (fRerunWanted) {
        fRerunWanted = false;
        emit slidesChanged();
        runPagination();  // the inputs moved mid-run; answer the newest ask
        return;
    }
    emit slidesChanged();
    // The honest arithmetic of the reservation, in the operator's terms,
    // naming the context the plan was measured in when one answered
    // (task TF.4) — "the theme decides" made audible.
    setStatus(QStringLiteral("%1 slide%2 measured to fit%3 (the reference and attribution "
                             "reserve %4 px of the box) — the picture is off air; Add places "
                             "them on today's schedule as one undoable item")
                  .arg(static_cast<qint64>(fSlides.size()))
                  .arg(fSlides.size() == 1 ? QString() : QStringLiteral("s"))
                  .arg(fContext.isEmpty()
                           ? QString()
                           : QStringLiteral(" in the theme's %1 context").arg(fContext))
                  .arg(QString::number(outcome.reservedHeight, 'f', 0)));
    emit stateChanged();
    previewSlide(0);
}

void ScriptureImport::previewSlide(int index) {
    if (index < 0 || index >= fSlides.size()) {
        return;
    }
    const QStringList lines =
        fSlides.at(index).toMap().value(QStringLiteral("lines")).toStringList();
    const QString sceneJson = fDocuments->scriptureSlideSceneJson(lines);
    if (sceneJson.isEmpty()) {
        setStatus(QStringLiteral("preview: this slide could not be compiled — nothing was "
                                 "written and the schedule is untouched"));
        return;
    }
    // Off air, by construction: this is the editor's selection-preview ask
    // (task 6.16a's transport), not a present. The audience's picture is
    // whatever it was; the preview stream is the operator's.
    fWorker->requestSelectionPreview(sceneJson);
}

bool ScriptureImport::addToSchedule() {
    if (measuring()) {
        setStatus(QStringLiteral("add: the words are still being measured — Add wakes when "
                                 "the slides do"));
        return false;
    }
    if (fSlides.isEmpty()) {
        setStatus(QStringLiteral("add: there is nothing to add yet — look up a passage and "
                                 "keep at least one verse"));
        return false;
    }
    QVariantList payload;
    for (const QVariant& slide : fSlides) {
        const QVariantMap row = slide.toMap();
        QVariantMap entry;
        entry.insert(QStringLiteral("lines"), row.value(QStringLiteral("lines")));
        // The verse label belongs in the slide's NOTES: the slide's words
        // stay exactly the passage (task BI.4's rule, carried to its home).
        entry.insert(QStringLiteral("notes"), row.value(QStringLiteral("label")));
        payload.push_back(entry);
    }
    // Today's schedule, the one the workspace is showing — never the
    // Library, never a present: these two objects own no such verb.
    const QString playlist = fDocuments->currentPlaylistId();
    const bool added = fDocuments->addScripturePassage(playlist, fBible->reference(),
                                                       fBible->translation(),
                                                       fBible->attribution(), payload);
    if (!added) {
        // The controller refused and said why (its sentence already names
        // the case); echo it so the page's own line stays the truth.
        setStatus(fDocuments->status());
        return false;
    }
    // The schedule owns the words now, as one item. The transient batch
    // dies HERE, so a second click has nothing left to duplicate — the
    // plan's "repeated Add cannot duplicate an insertion" made structural
    // rather than advisory.
    fSlides.clear();
    fMeasured.clear();
    emit slidesChanged();
    setStatus(QStringLiteral("added to today's schedule as one item — one undo removes the "
                             "whole passage. Look up again to import more."));
    emit stateChanged();
    return true;
}

void ScriptureImport::cancel() {
    fBible->cancel();  // the in-flight fetch, if any, retires unanswered (BI.2)
    fPendingMeasureId = 0;
    fPendingSends.clear();
    fMeasured.clear();
    fMeasuring = false;
    fRerunWanted = false;
    fContext.clear();  // the cancelled plan named a context; there is no plan now
    const bool hadSlides = !fSlides.isEmpty();
    if (hadSlides) {
        fSlides.clear();
        emit slidesChanged();
    }
    setStatus(QStringLiteral("cancelled — the fetched words and the generated slides are gone. "
                             "Nothing was written%1")
                  .arg(hadSlides ? QStringLiteral(" to the schedule") : QString()));
}

void ScriptureImport::finishWithFailure(const QString& sentence) {
    fMeasuring = false;
    fSlides.clear();
    fContext.clear();  // there is no plan left to name a context of
    emit slidesChanged();
    setStatus(sentence);
}

void ScriptureImport::setStatus(const QString& sentence) {
    fStatus = sentence;
    emit stateChanged();
}

}  // namespace openshow::ui
