#include "ReflowPlanner.h"

#include <QTimer>
#include <QVariantList>

#include "DocumentController.h"
#include "SongPlanner.h"
#include "WorkerController.h"

namespace openshow::ui {

namespace {

constexpr const char* kPendingMarker = "\x01awaiting-measurement";
constexpr int kMaxMeasureRounds = 500;
constexpr int kMeasureAnswerBudgetMs = 15000;

const char* kindName(openshow::domain::ReflowSourceKind kind) {
    switch (kind) {
        case openshow::domain::ReflowSourceKind::kCatalogSong:
            return "catalog-song";
        case openshow::domain::ReflowSourceKind::kScripture:
            return "scripture";
        case openshow::domain::ReflowSourceKind::kLocalSong:
            break;
    }
    return "local-song";
}

}  // namespace

ReflowPlanner::ReflowPlanner(DocumentController* documents, WorkerController* worker,
                             QObject* parent)
    : QObject(parent), fDocuments(documents), fWorker(worker) {
    if (fWorker == nullptr) {
        return;
    }
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
                    if (fMeasured.size() >=
                        openshow::importer::kMaxMeasureCacheEntries) {
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
                finishWithFailure(
                    reason.isEmpty()
                        ? QStringLiteral("the renderer would not answer the measurement")
                        : reason);
            });
    connect(fWorker, &WorkerController::stateChanged, this, [this] {
        if (fWorker->isConnected() || fPendingMeasureId == 0) {
            return;
        }
        fPendingMeasureId = 0;
        fPendingSends.clear();
        finishWithFailure(QStringLiteral(
            "the output went away mid-measurement — nothing was changed; try "
            "again once the worker is back"));
    });
}

void ReflowPlanner::setMeasuring(bool measuring) {
    if (fMeasuring == measuring) {
        return;
    }
    fMeasuring = measuring;
    emit stateChanged();
}

QVariantMap ReflowPlanner::eligibility(const QString& showId) const {
    QVariantMap out;
    if (fDocuments == nullptr) {
        out.insert(QStringLiteral("eligible"), false);
        out.insert(QStringLiteral("reason"),
                   QStringLiteral("there is no open service"));
        return out;
    }
    const openshow::domain::ReflowEligibility check =
        openshow::domain::reflowEligibility(fDocuments->documents(),
                                            showId.toStdString());
    out.insert(QStringLiteral("eligible"), check.eligible);
    out.insert(QStringLiteral("reason"), QString::fromStdString(check.reason));
    out.insert(QStringLiteral("usesStoredSource"), check.usesStoredSource);
    out.insert(QStringLiteral("sourceKind"),
               QString::fromLatin1(kindName(check.source.kind)));
    out.insert(QStringLiteral("sectionStartsNewSlide"),
               check.sectionStartsNewSlide);
    out.insert(QStringLiteral("leadingBlank"), check.hasLeadingBlankCue);
    out.insert(QStringLiteral("contentSlideCount"),
               static_cast<int>(check.contentSlideCount));
    out.insert(QStringLiteral("blockCount"),
               static_cast<int>(check.source.blocks.size()));
    return out;
}

bool ReflowPlanner::prepare(const QString& showId, const QString& snapshotJson) {
    // A new ask always retires the previous one, in flight or not.
    ++fGeneration;
    fPendingMeasureId = 0;
    fPendingSends.clear();
    fMeasured.clear();
    fRestartWanted = false;
    fHasPlan = false;
    fSlides.clear();
    fReservedHeight = 0.0;
    setMeasuring(false);

    const auto refuse = [this](const QString& reason) {
        fPlanError = reason;
        emit prepared(false, reason);
        return false;
    };
    if (fDocuments == nullptr) {
        return refuse(QStringLiteral("there is no open service"));
    }
    const openshow::domain::ReflowEligibility check =
        openshow::domain::reflowEligibility(fDocuments->documents(),
                                            showId.toStdString());
    if (!check.eligible) {
        return refuse(QString::fromStdString(check.reason));
    }
    if (fWorker == nullptr || !fWorker->isConnected()) {
        return refuse(QStringLiteral(
            "Reflow needs a connected renderer. You can still keep the existing slide breaks."));
    }
    if (!fWorker->textMeasureSupported()) {
        return refuse(QStringLiteral(
            "The connected renderer cannot reflow text. You can still keep the existing slide breaks."));
    }

    // The future appearance comes from the portable look's own numbers, so
    // the box the plan is measured against is the box the new slides wear.
    const QVariantMap styleMap = fDocuments->boxStyleForSnapshot(
        snapshotJson, QString::fromStdString(check.textContext));
    if (styleMap.isEmpty()) {
        return refuse(QStringLiteral("the chosen look has no usable box to measure"));
    }
    openshow::importer::SongPaginationInput captured;
    if (!songInputForStyle({}, styleMap, &captured)) {
        return refuse(QStringLiteral("the chosen look has no usable box to measure"));
    }

    fShowId = showId;
    fEligibility = check;
    fInput = openshow::importer::ReflowPaginationInput{};
    fInput.style = captured.style;
    fInput.blocks = check.source.blocks;
    fInput.policy = check.sectionStartsNewSlide
                        ? openshow::importer::ReflowPolicy::kSectionStartsNewSlide
                        : openshow::importer::ReflowPolicy::kGreedyBlocks;
    fInput.reference = check.source.reference;
    fInput.attribution = check.source.attribution;
    fInput.fallbackLabel = check.source.reference;
    fExpectedFingerprint = openshow::domain::reflowFingerprint(
        openshow::domain::showTextProjection(fDocuments->documents(),
                                             showId.toStdString()));
    fDocumentGeneration = fDocuments->documentGeneration();
    fPlanError.clear();
    run();
    return true;
}

void ReflowPlanner::cancel() {
    ++fGeneration;
    fPendingMeasureId = 0;
    fPendingSends.clear();
    fMeasured.clear();
    fRestartWanted = false;
    setMeasuring(false);
    fHasPlan = false;
    fSlides.clear();
    fReservedHeight = 0.0;
    fShowId.clear();
    fPlanError = QStringLiteral("reflow preparation was cancelled");
    emit stateChanged();
}

QVariantMap ReflowPlanner::plan() const {
    QVariantMap out;
    if (!fHasPlan) {
        return out;
    }
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("showId"), fShowId);
    out.insert(QStringLiteral("sourceKind"),
               QString::fromLatin1(kindName(fEligibility.source.kind)));
    out.insert(QStringLiteral("usedFallback"), !fEligibility.usesStoredSource);
    out.insert(QStringLiteral("leadingBlank"), fEligibility.hasLeadingBlankCue);
    out.insert(QStringLiteral("backgroundAssetId"),
               QString::fromStdString(fEligibility.backgroundAssetId));
    out.insert(QStringLiteral("backgroundFit"),
               static_cast<int>(fEligibility.backgroundFit));
    out.insert(QStringLiteral("textContext"),
               QString::fromStdString(fEligibility.textContext));
    out.insert(QStringLiteral("sourceFingerprint"),
               QString::fromStdString(fExpectedFingerprint));
    out.insert(QStringLiteral("contentSlideCount"),
               static_cast<int>(fEligibility.contentSlideCount));
    out.insert(QStringLiteral("sectionStartsNewSlide"),
               fEligibility.sectionStartsNewSlide);
    out.insert(QStringLiteral("generation"),
               static_cast<qulonglong>(fGeneration));
    out.insert(QStringLiteral("documentGeneration"),
               static_cast<qulonglong>(fDocumentGeneration));
    out.insert(QStringLiteral("reservedHeight"), fReservedHeight);
    out.insert(QStringLiteral("slideCount"), static_cast<int>(fSlides.size()));
    const openshow::domain::Show* show =
        fDocuments != nullptr ? fDocuments->documents().findShow(
                                    fShowId.toStdString())
                              : nullptr;
    out.insert(QStringLiteral("showAudioAssetId"),
               show != nullptr ? QString::fromStdString(show->audioAssetId)
                               : QString());
    QVariantList slides;
    for (const openshow::importer::ReflowSlide& slide : fSlides) {
        QVariantMap row;
        QVariantList lines;
        for (const std::string& line : slide.lines) {
            lines.push_back(QString::fromStdString(line));
        }
        row.insert(QStringLiteral("lines"), lines);
        row.insert(QStringLiteral("label"),
                   QString::fromStdString(slide.label));
        // Songs keep their section identity on the slide's GROUP; a passage
        // has no song sections, so its verse labels stay in notes only.
        std::string group;
        if (fEligibility.sectionStartsNewSlide) {
            group = slide.sectionLabel;
        }
        row.insert(QStringLiteral("group"), QString::fromStdString(group));
        slides.push_back(row);
    }
    out.insert(QStringLiteral("slides"), slides);
    return out;
}

void ReflowPlanner::run() {
    const quint64 generation = fGeneration;
    setMeasuring(true);
    for (int round = 1; round <= kMaxMeasureRounds; ++round) {
        const openshow::importer::ReflowPaginationOutcome outcome =
            openshow::importer::paginateReflow(
                fInput,
                [this](const std::vector<openshow::importer::MeasureRequest>& requests,
                       std::vector<openshow::importer::MeasureReply>* replies,
                       std::string* error) {
                    return measure(requests, replies, error);
                });
        if (generation != fGeneration) {
            return;  // superseded or cancelled while this round ran
        }
        if (outcome.ok) {
            // The words may have moved while the worker was answering. A
            // plan for the old words is stale, not "close enough".
            const openshow::domain::Show* show =
                fDocuments->documents().findShow(fShowId.toStdString());
            if (show == nullptr) {
                finishWithFailure(QStringLiteral(
                    "the item was removed while its reflow was being prepared — "
                    "nothing was changed"));
                return;
            }
            const std::string current = openshow::domain::reflowFingerprint(
                openshow::domain::showTextProjection(fDocuments->documents(),
                                                     fShowId.toStdString()));
            if (current != fExpectedFingerprint) {
                finishWithFailure(QStringLiteral(
                    "the item's words changed while its reflow was being prepared — "
                    "nothing was changed"));
                return;
            }
            if (!openshow::importer::reflowWordsPreserved(fInput.blocks,
                                                          outcome.slides)) {
                finishWithFailure(QStringLiteral(
                    "the measured plan did not preserve every word — nothing was "
                    "changed"));
                return;
            }
            fSlides = outcome.slides;
            fReservedHeight = outcome.reservedHeight;
            fHasPlan = true;
            fPlanError.clear();
            setMeasuring(false);
            emit prepared(true, QString());
            emit stateChanged();
            return;
        }
        if (outcome.error.find(kPendingMarker) != std::string::npos) {
            setMeasuring(false);
            return;  // suspended, not failed
        }
        finishWithFailure(QString::fromStdString(outcome.error));
        return;
    }
    finishWithFailure(
        QStringLiteral("the words did not settle into slides — try a smaller box or "
                       "a larger size"));
}

void ReflowPlanner::finishWithFailure(const QString& reason) {
    setMeasuring(false);
    fHasPlan = false;
    fSlides.clear();
    fReservedHeight = 0.0;
    fPlanError = reason;
    emit prepared(false, reason);
    emit stateChanged();
}

bool ReflowPlanner::measure(
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
        block.insert(QStringLiteral("height"), request.boxHeight);
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
        block.insert(QStringLiteral("verticalAlign"),
                     QString::fromStdString(request.verticalAlign));
        block.insert(QStringLiteral("allCaps"), request.allCaps);
        QVariantMap stroke;
        stroke.insert(QStringLiteral("enabled"), request.stroke.enabled);
        stroke.insert(QStringLiteral("color"),
                      QString::fromStdString(request.stroke.color));
        stroke.insert(QStringLiteral("width"), request.stroke.width);
        block.insert(QStringLiteral("stroke"), stroke);
        QVariantMap shadow;
        shadow.insert(QStringLiteral("enabled"), request.shadow.enabled);
        shadow.insert(QStringLiteral("color"),
                      QString::fromStdString(request.shadow.color));
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
            "the renderer did not answer the measurement — nothing was changed; "
            "try again, and if it keeps silence check the output device"));
    });
    *error = kPendingMarker;
    return false;
}

}  // namespace openshow::ui
