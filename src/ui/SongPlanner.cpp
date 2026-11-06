#include "SongPlanner.h"

#include <QTimer>
#include <QVariantMap>

#include "DocumentController.h"
#include "WorkerController.h"

namespace openshow::ui {

namespace {

constexpr const char* kPendingMarker = "\x01awaiting-measurement";
constexpr int kMaxMeasureRounds = 500;
constexpr int kMeasureAnswerBudgetMs = 15000;

}  // namespace

bool songInputFor(DocumentController* documents,
                  const std::vector<openshow::catalog::Stanza>& stanzas,
                  openshow::importer::SongPaginationInput* out) {
    *out = openshow::importer::SongPaginationInput{};
    out->stanzas = stanzas;
    if (documents == nullptr) {
        return false;
    }
    const QVariantMap style = documents->songBoxStyle();
    if (!style.contains(QStringLiteral("boxWidth"))) {
        return false;
    }
    out->style.boxWidth = style.value(QStringLiteral("boxWidth")).toDouble();
    out->style.boxHeight = style.value(QStringLiteral("boxHeight")).toDouble();
    out->style.fontSize = style.value(QStringLiteral("fontSize")).toDouble();
    out->style.lineSpacing = style.contains(QStringLiteral("lineSpacing"))
                                 ? style.value(QStringLiteral("lineSpacing")).toDouble()
                                 : 1.2;
    out->style.fontFamily =
        style.value(QStringLiteral("fontFamily")).toString().toStdString();
    out->style.fontWeight = style.contains(QStringLiteral("fontWeight"))
                                ? style.value(QStringLiteral("fontWeight")).toInt()
                                : openshow::domain::kDefaultFontWeight;
    out->style.fontStyle = style.contains(QStringLiteral("fontStyle"))
                               ? style.value(QStringLiteral("fontStyle")).toString().toStdString()
                               : "normal";
    out->style.fitMode = style.contains(QStringLiteral("fitMode"))
                             ? style.value(QStringLiteral("fitMode")).toString().toStdString()
                             : "fixed";
    out->style.minFontSize = style.contains(QStringLiteral("minFontSize"))
                                 ? style.value(QStringLiteral("minFontSize")).toDouble()
                                 : openshow::domain::kDefaultMinFontSize;
    if (style.contains(QStringLiteral("stroke"))) {
        const QVariantMap stroke = style.value(QStringLiteral("stroke")).toMap();
        out->style.stroke.enabled = stroke.value(QStringLiteral("enabled")).toBool();
        out->style.stroke.color = stroke.value(QStringLiteral("color")).toString().toStdString();
        if (stroke.contains(QStringLiteral("width"))) {
            out->style.stroke.width = stroke.value(QStringLiteral("width")).toDouble();
        }
    }
    if (style.contains(QStringLiteral("shadow"))) {
        const QVariantMap shadow = style.value(QStringLiteral("shadow")).toMap();
        out->style.shadow.enabled = shadow.value(QStringLiteral("enabled")).toBool();
        out->style.shadow.color = shadow.value(QStringLiteral("color")).toString().toStdString();
        if (shadow.contains(QStringLiteral("opacity"))) {
            out->style.shadow.opacity = shadow.value(QStringLiteral("opacity")).toDouble();
        }
        if (shadow.contains(QStringLiteral("offsetX"))) {
            out->style.shadow.offsetX = shadow.value(QStringLiteral("offsetX")).toDouble();
        }
        if (shadow.contains(QStringLiteral("offsetY"))) {
            out->style.shadow.offsetY = shadow.value(QStringLiteral("offsetY")).toDouble();
        }
        if (shadow.contains(QStringLiteral("blurSigma"))) {
            out->style.shadow.blurSigma = shadow.value(QStringLiteral("blurSigma")).toDouble();
        }
    }
    return true;
}

SongPlanner::SongPlanner(DocumentController* documents, WorkerController* worker,
                         QObject* parent)
    : QObject(parent), fDocuments(documents), fWorker(worker) {
    connect(fWorker, &WorkerController::textMeasured, this,
            [this](qint64 id, const QVariantList& rows) {
                if (id != fPendingMeasureId) {
                    return;  // an ask this run already retired
                }
                fPendingMeasureId = 0;
                for (const QVariant& row : rows) {
                    const QVariantMap map = row.toMap();
                    const auto found = fPendingSends.find(
                        map.value(QStringLiteral("key")).toString().toStdString());
                    if (found == fPendingSends.end()) {
                        continue;
                    }
                    Answer answer;
                    answer.height = map.value(QStringLiteral("height")).toDouble();
                    const QVariant fits = map.value(QStringLiteral("fits"));
                    if (fits.isValid()) {
                        answer.fits = fits.toBool() ? 1 : 0;
                    }
                    if (fMeasured.size() >= openshow::importer::kMaxMeasureCacheEntries) {
                        fMeasured.clear();  // bounded: re-ask rather than grow
                    }
                    fMeasured[found->second] = answer;
                }
                fPendingSends.clear();
                if (fRestartWanted) {
                    fRestartWanted = false;
                    fMeasured.clear();
                }
                run();
            });
    connect(fWorker, &WorkerController::textMeasureFailed, this,
            [this](qint64 id, const QString& reason) {
                if (id != fPendingMeasureId) {
                    return;
                }
                fPendingMeasureId = 0;
                fPendingSends.clear();
                finishWithFailure(reason.isEmpty()
                                      ? QStringLiteral("the renderer would not answer the "
                                                       "measurement")
                                      : reason);
            });
    connect(fWorker, &WorkerController::stateChanged, this, [this] {
        if (fWorker->isConnected() || fPendingMeasureId == 0) {
            return;
        }
        fPendingMeasureId = 0;
        fPendingSends.clear();
        finishWithFailure(QStringLiteral(
            "the output went away mid-measurement — no slides were generated; "
            "try again once the worker is back"));
    });
}

void SongPlanner::start(const std::vector<openshow::catalog::Stanza>& stanzas,
                        const openshow::importer::SongPaginationInput* captured) {
    ++fGeneration;
    fHasPlan = false;
    fSlides.clear();
    fFingerprint.clear();
    fRestartWanted = false;
    fMeasured.clear();
    fInput = openshow::importer::SongPaginationInput{};
    if (captured != nullptr) {
        fInput = *captured;
        fInput.stanzas = stanzas;
    } else {
        songInputFor(fDocuments, stanzas, &fInput);
    }
    run();
}

void SongPlanner::cancel() {
    ++fGeneration;
    fPendingMeasureId = 0;
    fPendingSends.clear();
    fMeasured.clear();
    fRestartWanted = false;
    fMeasuring = false;
    fHasPlan = false;
    fSlides.clear();
    fFingerprint.clear();
}

void SongPlanner::run() {
    const quint64 generation = fGeneration;
    fMeasuring = true;
    for (int round = 1; round <= kMaxMeasureRounds; ++round) {
        const openshow::importer::SongPaginationOutcome outcome =
            openshow::importer::paginateSong(
                fInput, [this](const std::vector<openshow::importer::MeasureRequest>& requests,
                               std::vector<openshow::importer::MeasureReply>* replies,
                               std::string* error) {
                    return measure(requests, replies, error);
                });
        if (outcome.ok) {
            if (generation != fGeneration) {
                return;  // superseded while the last batch was artificial
            }
            fSlides = outcome.slides;
            // The opening cue is intentionally wordless; it is presentation
            // policy, not an empty stanza stored in the song database.
            openshow::importer::SongSlide opening;
            opening.label = "Blank";
            opening.stanzaIndex = fInput.stanzas.size();
            fSlides.insert(fSlides.begin(), std::move(opening));
            fFingerprint = openshow::importer::songPlanFingerprint(fInput);
            fHasPlan = true;
            fMeasuring = false;
            emit finished(true, QString());
            return;
        }
        if (outcome.error.find(kPendingMarker) != std::string::npos) {
            fMeasuring = false;
            return;  // suspended, not failed
        }
        finishWithFailure(QString::fromStdString(outcome.error));
        return;
    }
    finishWithFailure(
        QStringLiteral("the words did not settle into slides — try a smaller song"));
}

void SongPlanner::finishWithFailure(const QString& reason) {
    fMeasuring = false;
    fHasPlan = false;
    fSlides.clear();
    emit finished(false, reason);
}

bool SongPlanner::measure(
    const std::vector<openshow::importer::MeasureRequest>& requests,
    std::vector<openshow::importer::MeasureReply>* replies, std::string* error) {
    QVariantList blocks;
    for (const openshow::importer::MeasureRequest& request : requests) {
        const std::string cacheKey = openshow::importer::measureCacheKey(request);
        const auto cached = fMeasured.find(cacheKey);
        if (cached != fMeasured.end()) {
            openshow::importer::MeasureReply reply;
            reply.key = request.key;
            reply.height = cached->second.height;
            reply.lineCount = 0;
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
        block.insert(QStringLiteral("fontWeight"), request.fontWeight);
        block.insert(QStringLiteral("fontStyle"),
                     QString::fromStdString(request.fontStyle));
        block.insert(QStringLiteral("fitMode"),
                     QString::fromStdString(request.fitMode));
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
        return true;
    }
    if (fPendingMeasureId != 0) {
        *error = kPendingMarker;
        return false;
    }
    const qint64 id = fWorker->measureText(blocks);
    if (id == 0) {
        fPendingSends.clear();
        *error = "the words could not be put to the renderer: " +
                 fWorker->statusText().toStdString();
        return false;
    }
    fPendingMeasureId = id;
    QTimer::singleShot(kMeasureAnswerBudgetMs, this, [this, id] {
        if (fPendingMeasureId != id) {
            return;
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

}  // namespace openshow::ui
