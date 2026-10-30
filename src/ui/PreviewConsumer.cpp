/*
 * PreviewConsumer implementation. The rules here are the client's half of the
 * preview protocol; the notes on each say which failure they exist to prevent.
 */

#include "PreviewConsumer.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "openshow/protocol/preview_wire.h"

namespace openshow::ui {

namespace proto = openshow::protocol;

PreviewConsumer::PreviewConsumer(Outbound outbound, QObject* parent)
    : QObject(parent), fOut(std::move(outbound)) {}

PreviewConsumer::~PreviewConsumer() {
    closeStream();
}

void PreviewConsumer::setSessionLive(bool live) {
    fHostOnline = live;
}

void PreviewConsumer::setCapabilities(bool selectionCapable, bool thumbnailCapable) {
    // Both at once, because the hello reply answers both every time: a worker
    // that dropped a capability between handshakes must not leave the editor
    // asking for a target it can no longer draw.
    fSelectionCapable = selectionCapable;
    fThumbnailCapable = thumbnailCapable;
}

// --- the audience mirror's segment ---------------------------------------

void PreviewConsumer::closeStream() {
    if (fMap) {
        ::munmap(fMap, fMapBytes);
        fMap = nullptr;
        fMapBytes = 0;
    }
    if (fMapFd >= 0) {
        ::close(fMapFd);
        fMapFd = -1;
    }
    // The off-air frame lived in this mapping (task 6.16a). Dropping it is
    // not housekeeping: a picture from a retired stream, still on the canvas
    // while the operator edits something else, is exactly the stale imagery
    // the generation rule is designed to make impossible — so it goes now,
    // and the ask goes with it. What the UI WANTS is kept: the next stream
    // re-issues it as its own first ask.
    fSelectionImage = QImage();
    fSelectionImageRequest = 0;
    fSelectionAskedJson.clear();
    // The list's pictures came out of this mapping too, and so does their
    // right to be shown (task 6.16b). The ids go with them: a frame of the
    // retired stream can arrive with a sequence that still looks good, and the
    // only thing that says which slide it belongs to is the batch that named
    // it. What the UI WANTS is kept, as above — the next stream re-issues.
    clearThumbnailPictures();
    fThumbnailIds.clear();
    fThumbnailKeys.clear();
    fThumbnailAskedPayload.clear();
    fThumbnailAwaitAck = false;
}

void PreviewConsumer::openSegment(const std::string& name, std::uint64_t bytesPerSlot) {
    closeStream();
    fPreviewSlotStride = bytesPerSlot;

    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        fPreviewError = QStringLiteral("shm_open %1 failed").arg(
            QString::fromStdString(name));
        emit changed();
        return;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 ||
        info.st_size <
            static_cast<off_t>(proto::previewSegmentBytes(0, 0))) {
        ::close(fd);
        fPreviewError = QStringLiteral("preview segment has a bad size");
        emit changed();
        return;
    }
    const std::uint64_t total = fPreviewSlotStride * fPreviewSlotCount + 64;
    if (info.st_size < static_cast<off_t>(total)) {
        ::close(fd);
        fPreviewError = QStringLiteral("preview segment is smaller than promised");
        emit changed();
        return;
    }
    void* map = ::mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        fPreviewError = QStringLiteral("mmap %1 failed").arg(
            QString::fromStdString(name));
        emit changed();
        return;
    }

    const auto* segment = static_cast<const proto::PreviewSegmentHeader*>(map);
    if (segment->magic != proto::kPreviewMagic ||
        segment->layoutVersion != proto::kPreviewLayoutVersion ||
        segment->slotCount != fPreviewSlotCount ||
        segment->bytesPerSlot != bytesPerSlot) {
        ::munmap(map, total);
        ::close(fd);
        fPreviewError = QStringLiteral("preview segment header mismatch");
        emit changed();
        return;
    }

    fMap = static_cast<std::uint8_t*>(map);
    fMapFd = fd;
    fMapBytes = total;
    fPreviewError.clear();
    emit changed();
}

void PreviewConsumer::handleStartReply(const boost::json::object& payload) {
    if (!payload.contains("bytesPerSlot")) {
        return;
    }
    // Do not map yet: the segment exists once the render thread has
    // acted, and the first preview.ready proves it did.
    closeStream();
    fPreviewSlotCount = payload.at("slotCount").to_number<std::uint64_t>();
    fPreviewSlotStride = payload.at("bytesPerSlot").to_number<std::uint64_t>();
    fPendingPreviewName = std::string(payload.at("name").as_string());
    fPreviewFrames = 0;
    fPreviewError.clear();
    // A new stream is a new generation domain (task 6.16a): the worker
    // has no selection registered on it, so whatever the editor was
    // looking at has to be asked for again — and the frame that answers
    // the old stream's ask is gone with it.
    fSelectionAskedJson.clear();
    fSelectionImage = QImage();
    fSelectionImageRequest = 0;
    fSelectionFrames = 0;
    if (!fSelectionWantedJson.isEmpty()) {
        const QString wanted = fSelectionWantedJson;
        requestSelectionPreview(wanted);
    }
    // The same reset for the list's target (task 6.16b): a new segment is
    // empty, the worker has no batch registered on it, and every id the
    // old ack named describes a frame that can never arrive. What the list
    // wants is re-asked as this stream's own first batch.
    fThumbnailAskedPayload.clear();
    fThumbnailAcked = 0;
    fThumbnailAwaitAck = false;
    fThumbnailIds.clear();
    fThumbnailKeys.clear();
    fThumbnailFrames = 0;
    clearThumbnailPictures();
    if (!fThumbnailWantedPayload.isEmpty()) {
        sendThumbnailBatch(fThumbnailWantedPayload, fThumbnailWantedWidth,
                           fThumbnailWantedHeight);
    }
    emit changed();
}

// --- the editor's off-air picture (task 6.16a, review R16) ---------------
//
// The edit canvas used to render its own text with QML, which meant the
// operator composed one picture and the audience saw another: Qt's wrapping,
// Qt's fit, Qt's fallbacks against Skia's. The fix is not a better QML
// approximation — it is the worker drawing the slide the operator picked,
// into the preview stream it already owns, without the audience being shown
// it. So this is a three-step conversation over the transport that exists:
// prepare (the store hands out a revision), preview.select (the render thread
// draws THAT revision off the air), preview.ready tagged with the generation
// (the picture arrives). Nothing here presents, restores or clears.
void PreviewConsumer::requestSelectionPreview(const QString& sceneJson) {
    if (sceneJson.isEmpty()) {
        return;  // no slide: the caller stands the target down instead
    }
    // What the UI wants to see, remembered FIRST and even when this call
    // cannot ask for it: a worker that is still starting, a stream
    // mid-restart, and a capability that only appears at the handshake all
    // re-issue from here, which is why opening the edit page on a cold start
    // still ends with a picture rather than a placeholder forever.
    fSelectionWantedJson = sceneJson;
    if (!fHostOnline || !fSelectionCapable) {
        return;  // the canvas keeps the frame it last had, awaiting its host
    }
    if (sceneJson == fSelectionAskedJson) {
        return;  // the newest ask already carries this exact picture
    }
    boost::system::error_code parseError;
    const boost::json::value parsed =
        boost::json::parse(sceneJson.toStdString(), parseError);
    if (parseError || !parsed.is_object()) {
        emit statusMessage(QStringLiteral("preview.select: the scene JSON is not readable"));
        return;
    }
    if (!fOut.sendable(parsed, "preview.select")) {
        return;  // the reason is already in the status line
    }
    // Prepare first: an off-air scene is named by a store revision, and the
    // only way to have one is to hand the scene over. No armPrepare — that
    // mirrors the payload as a presentable scene, and this one is not being
    // presented; the marker below is what makes the ack answer with a
    // preview.select instead. The ask travels WITH the request rather than being
    // noted after it: the generation only exists once the socket carries it, and
    // the reply has to find the one it belongs to.
    auto ask = std::make_shared<ScenePrepareAsk>();
    const std::int64_t id = fOut.request("scene.prepare", parsed.as_object(), ask);
    if (id == 0) {
        return;  // refused locally (backlog or in-flight bound); a later change re-asks
    }
    ++fSelectionRequest;
    ask->selectionRequest = fSelectionRequest;
    fSelectionAskedJson = sceneJson;
}

void PreviewConsumer::clearSelectionPreview() {
    const bool hadAsk = !fSelectionAskedJson.isEmpty() || !fSelectionWantedJson.isEmpty();
    fSelectionWantedJson.clear();
    fSelectionAskedJson.clear();
    fSelectionImage = QImage();
    fSelectionImageRequest = 0;
    if (hadAsk) {
        emit changed();
    }
    if (!fHostOnline || !fSelectionCapable || fPendingPreviewName.empty()) {
        return;  // no stream: there is no registered target to stand down
    }
    ++fSelectionRequest;  // a stand-down is a change, and changes advance
    fOut.request(proto::kMethodPreviewSelect, boost::json::object{{"revision", 0},
                                                                  {"request", fSelectionRequest}},
                 nullptr);
}

void PreviewConsumer::handlePrepareAck(const ScenePrepareAsk& intent, qint64 revision) {
    // The off-air ask (task 6.16a): this revision IS the picture the
    // editor asked for, so name it back with the generation the ask
    // carried. The worker refuses a generation that does not follow the
    // one it accepted — which is the rule that keeps a retried or
    // reordered prepare from putting an edit the operator abandoned back
    // on the canvas.
    if (intent.selectionRequest == 0) {
        return;
    }
    fOut.request(proto::kMethodPreviewSelect,
                 boost::json::object{{"revision", static_cast<std::uint64_t>(revision)},
                                     {"request", intent.selectionRequest}},
                 nullptr);
}

void PreviewConsumer::handleRefusal(const ScenePrepareAsk& intent) {
    if (intent.thumbnailRequest != 0) {
        // A refused batch — no stream yet, an extent that could not fit a
        // slot, a generation the worker will not take — means the ask was
        // never MADE, so "asked" goes back to empty. The status poll's
        // stand-down rule then re-issues what the UI still wants once the
        // reason is gone, instead of the list believing it had asked for
        // pictures forever.
        fThumbnailAwaitAck = false;
        fThumbnailAskedPayload.clear();
    }
    if (intent.selectionRequest != 0) {
        // Task 6.9a's refusal is the same shape: an off-air prepare the
        // worker would not schedule was never MADE, so the dedupe that
        // remembers the asked payload must forget it — otherwise
        // re-selecting this very slide would silently do nothing, and
        // the canvas would wait forever on a frame nobody is coming.
        fSelectionAskedJson.clear();
    }
}

// --- the slide list's pictures (task 6.16b, review R16) ------------------
//
// Twenty slides used to be twenty lines of text, which is the same compromise
// the edit canvas used to make: the operator composes one picture in Skia and
// the list shows somebody else's approximation of it. The worker now renders
// the visible band itself — its compositor, its fonts, its backgrounds — at
// the size the rows are drawn, into the stream it already owns, as a third
// target, off the air and out of the scene store. One step rather than three:
// preview.thumbnails with the band's compiled scene text, an ack carrying one
// id per slide, and preview.ready frames tagged target=2 whose `revision` is
// that id. Pixels never ride the socket in either direction.
//
// The client's half of "this is a preview, not a second show" is the shape of
// its memory: the newest band is the only thing it wants, the ids live exactly
// as long as the batch that named them, and the pictures it has copied are
// bounded by count AND by bytes, so the cache is a number rather than a habit
// no one has measured.
QImage PreviewConsumer::thumbnail(const QString& key) const {
    const auto entry = fThumbnailCache.constFind(key);
    return entry == fThumbnailCache.constEnd() ? QImage() : entry->image;
}

void PreviewConsumer::storeThumbnail(const QString& key, QImage image) {
    // Arrival is the recency: a band scrolls one way, so the least recently
    // received picture is the one whose row is furthest off screen.
    dropThumbnailPicture(key);
    ThumbnailPicture picture;
    picture.image = std::move(image);
    picture.sourceJson = fThumbnailAskedScenes.value(key);
    fThumbnailBytes += picture.image.sizeInBytes();
    fThumbnailCache.insert(key, std::move(picture));
    fThumbnailOrder.push_back(key);
    ++fThumbnailFrames;
    // Bounded twice, because either bound alone is a leak with a good excuse:
    // a document of two thousand slides would grow a per-slide cache forever,
    // and a cache of "just" sixty-four pictures is unbounded if a slide is
    // ever asked for at 512x512. Whichever ceiling is hit first evicts.
    while (fThumbnailOrder.size() > static_cast<int>(kThumbnailCacheEntries) ||
           fThumbnailBytes > kThumbnailCacheBytes) {
        if (fThumbnailOrder.isEmpty()) {
            break;
        }
        dropThumbnailPicture(fThumbnailOrder.front());
    }
    emit thumbnailReady(key);
}

void PreviewConsumer::dropThumbnailPicture(const QString& key) {
    const auto entry = fThumbnailCache.find(key);
    if (entry != fThumbnailCache.end()) {
        fThumbnailBytes -= entry->image.sizeInBytes();
        fThumbnailCache.erase(entry);
    }
    fThumbnailOrder.removeAll(key);
    if (fThumbnailBytes < 0) {
        fThumbnailBytes = 0;  // the counter cannot go under; a stale bookkeeping
                              // bug must not make it able to never evict again
    }
}

void PreviewConsumer::clearThumbnailPictures() {
    fThumbnailCache.clear();
    fThumbnailOrder.clear();
    fThumbnailBytes = 0;
}

void PreviewConsumer::requestThumbnails(const QVariantList& slides, qint64 width,
                                        qint64 height) {
    // Build the batch the worker will see, and record what the UI wants FIRST
    // and even when this call cannot send it: a worker still starting, a stream
    // mid-restart and a capability that only appears at the handshake all
    // re-issue from here, which is why opening the slide list on a cold start
    // ends with pictures rather than text lines forever.
    boost::json::array scenes;
    QHash<QString, QString> askedScenes;
    for (const QVariant& entry : slides) {
        const QVariantMap slide = entry.toMap();
        const QString key = slide.value(QStringLiteral("key")).toString();
        const QString sceneJson = slide.value(QStringLiteral("scene")).toString();
        if (key.isEmpty() || sceneJson.isEmpty()) {
            continue;
        }
        boost::system::error_code parseError;
        const boost::json::value parsed =
            boost::json::parse(sceneJson.toStdString(), parseError);
        if (parseError || !parsed.is_object()) {
            continue;  // a slide the compiler has not answered for
        }
        // R11's rule, applied to a BATCH: a slide this build already knows the
        // worker will refuse (authored text over the limit, a data: URI as an
        // asset reference) is dropped here with the reason in the status line,
        // and the rest of the band is asked for anyway. Sending it would buy
        // nothing — the worker refuses that entry too — and a band whose thirty
        // scenes happen to exceed one message would take its twenty-nine good
        // pictures down with it.
        if (!fOut.sendable(parsed, "preview.thumbnails")) {
            continue;
        }
        // An edited slide's old picture is a lie about the content it is
        // listed under, so it goes now and the row falls back to its text line
        // until the worker's new render lands. An unchanged slide keeps its
        // picture: the ask below is deduplicated, so re-listing a band nobody
        // edited costs nobody anything.
        const auto cached = fThumbnailCache.constFind(key);
        if (cached != fThumbnailCache.constEnd() && cached->sourceJson != sceneJson) {
            dropThumbnailPicture(key);
        }
        boost::json::object item;
        item.emplace("key", key.toStdString());
        item.emplace("scene", parsed);
        scenes.push_back(std::move(item));
        askedScenes.insert(key, sceneJson);
    }
    if (scenes.size() > proto::kMaxThumbnailBatch) {
        // The cap is the bound on what one ask can cost the worker, so it is
        // judged here as well as there. Refusing with the reason beats drawing
        // the first thirty of sixty and letting the list believe it had
        // pictures for all sixty.
        emit statusMessage(QStringLiteral("preview.thumbnails takes %1 slides, asked for %2"
                                          " - ask for the visible band")
                               .arg(static_cast<qint64>(proto::kMaxThumbnailBatch))
                               .arg(static_cast<qint64>(scenes.size())));
        return;
    }
    const QString payload = QString::fromStdString(boost::json::serialize(scenes));
    fThumbnailWantedPayload = payload;
    fThumbnailWantedWidth = width;
    fThumbnailWantedHeight = height;
    if (!fHostOnline || !fThumbnailCapable) {
        return;  // the rows keep their text lines, awaiting the capability
    }
    if (payload == fThumbnailAskedPayload && width == fThumbnailAskedWidth &&
        height == fThumbnailAskedHeight && fThumbnailAcked == fThumbnailRequest &&
        !fThumbnailAwaitAck) {
        return;  // the newest ask already carries exactly this band
    }
    fThumbnailAskedScenes = askedScenes;
    sendThumbnailBatch(payload, width, height);
}

void PreviewConsumer::sendThumbnailBatch(const QString& scenesPayload, qint64 width,
                                         qint64 height) {
    if (!fHostOnline || !fThumbnailCapable) {
        return;
    }
    if (fPendingPreviewName.empty()) {
        return;  // no stream: there is nowhere for these pixels to go
    }
    // The extent is judged against the segment this client mapped, not against
    // a box: a frame's pixels live in a slot reserved for the stream's own
    // picture, so a size legal on a 1024x576 stream is impossible on a
    // 320x180 one. The worker refuses the same way (the same helper decides
    // both), and asking first puts the reason on the operator's status line
    // rather than in a socket error.
    if (!proto::thumbnailExtentFits(static_cast<std::uint32_t>(std::max<qint64>(0, width)),
                                    static_cast<std::uint32_t>(std::max<qint64>(0, height)),
                                    static_cast<std::uint32_t>(fPreviewSlotStride))) {
        // One predicate, and one sentence that names both of its halves: the
        // agreed box AND this stream's slot. A 16x16 ask and a 512x100 ask on a
        // small stream fail for different reasons, and a status line that
        // blamed the slot for the first would send an operator to change the
        // wrong number.
        emit statusMessage(
            QStringLiteral("preview.thumbnails: %1x%2 is outside %3..%4 pixels, or does "
                           "not fit the %5-byte preview slot this stream was mapped with")
                .arg(width)
                .arg(height)
                .arg(static_cast<qint64>(proto::kMinThumbnailDimension))
                .arg(static_cast<qint64>(proto::kMaxThumbnailDimension))
                .arg(static_cast<qulonglong>(fPreviewSlotStride)));
        return;
    }
    boost::system::error_code parseError;
    const boost::json::value scenes =
        boost::json::parse(scenesPayload.toStdString(), parseError);
    if (parseError || !scenes.is_array()) {
        emit statusMessage(QStringLiteral("preview.thumbnails: the band is not readable"));
        return;
    }
    // THE GENERATION, the batch's own counter: every ask the socket carries
    // advances it, and the worker takes only one strictly later than what it
    // accepted. That is what makes a duplicated scroll handler, a retried
    // request and a replayed frame all the same non-event.
    ++fThumbnailRequest;
    auto ask = std::make_shared<ScenePrepareAsk>();
    boost::json::object params{{"scenes", scenes},
                               {"width", static_cast<std::int64_t>(width)},
                               {"height", static_cast<std::int64_t>(height)},
                               {"request", fThumbnailRequest}};
    const std::int64_t id =
        fOut.request(proto::kMethodPreviewThumbnails, std::move(params), ask);
    if (id == 0) {
        return;  // refused locally; the next change re-asks
    }
    ask->thumbnailRequest = fThumbnailRequest;
    fThumbnailAskedPayload = scenesPayload;
    fThumbnailAskedWidth = width;
    fThumbnailAskedHeight = height;
    fThumbnailAwaitAck = true;
}

void PreviewConsumer::handleThumbnailsAck(const ScenePrepareAsk& intent,
                                          const boost::json::object& ack) {
    // The ack for a band (task 6.16b, review R16): the per-slide ids its
    // frames will carry. They are installed from THIS request's reply and
    // only while it is still the newest ask this client made — an ack for
    // a band the operator has since scrolled past would otherwise map a
    // frame of nothing onto a row that is on screen.
    if (intent.thumbnailRequest != fThumbnailRequest) {
        return;  // superseded: the ids it names are already nobody's
    }
    fThumbnailAwaitAck = false;
    const auto generation = ack.find("request");
    if (generation != ack.end()) {
        fThumbnailAcked = static_cast<std::uint32_t>(
            generation->value().to_number<std::uint64_t>());
    }
    fThumbnailIds.clear();
    fThumbnailKeys.clear();
    const auto frames = ack.find("frames");
    if (frames != ack.end() && frames->value().is_array()) {
        for (const auto& item : frames->value().as_array()) {
            if (!item.is_object()) {
                continue;
            }
            const boost::json::object& slide = item.as_object();
            const auto key = slide.find("key");
            const auto id = slide.find("id");
            if (key == slide.end() || !key->value().is_string() || id == slide.end()) {
                continue;
            }
            const QString slideKey =
                QString::fromStdString(std::string(key->value().as_string()));
            const auto numericId =
                static_cast<std::uint32_t>(id->value().to_number<std::uint64_t>());
            if (slideKey.isEmpty() || numericId == 0) {
                continue;  // 0 is "no id", on every identity in this protocol
            }
            fThumbnailIds.insert(slideKey, numericId);
            fThumbnailKeys.insert(numericId, slideKey);
        }
    }
    // A refused entry is reported by the caller-facing status line rather
    // than silently: a slide whose scene does not compile is a document
    // problem the operator can act on, and the band's other pictures
    // arrive anyway.
    const auto refused = ack.find("refused");
    if (refused != ack.end() && refused->value().is_array() &&
        !refused->value().as_array().empty()) {
        emit statusMessage(QStringLiteral("preview.thumbnails: %1 of %2 slides refused")
                               .arg(static_cast<qint64>(refused->value().as_array().size()))
                               .arg(static_cast<qint64>(fThumbnailIds.size() +
                                                        refused->value().as_array().size())));
    }
    emit changed();
}

void PreviewConsumer::clearThumbnails() {
    const bool hadAsk = !fThumbnailAskedPayload.isEmpty() || !fThumbnailWantedPayload.isEmpty();
    const qint64 width = fThumbnailWantedWidth;
    const qint64 height = fThumbnailWantedHeight;
    // Nothing is wanted, nothing is owed, and nothing is kept: a stand-down
    // that left thirty pictures in the cache would be a list that never stops
    // costing memory the moment it stops being looked at.
    fThumbnailWantedPayload.clear();
    fThumbnailAskedPayload.clear();
    fThumbnailAskedScenes.clear();
    fThumbnailIds.clear();
    fThumbnailKeys.clear();
    fThumbnailAwaitAck = false;
    clearThumbnailPictures();
    if (hadAsk) {
        emit changed();
    }
    if (!hadAsk || !fHostOnline || !fThumbnailCapable || fPendingPreviewName.empty()) {
        return;  // never asked: there is no registered batch to stand down
    }
    // A stand-down is a change, so it advances the generation like any other
    // ask, and it travels with the extent the batch had: the worker judges the
    // extent before the batch, and an empty list with an extent it cannot
    // describe would be refused rather than obeyed.
    sendThumbnailBatch(QStringLiteral("[]"), width == 0 ? 160 : width,
                       height == 0 ? 90 : height);
}

// --- what arrives --------------------------------------------------------

void PreviewConsumer::updateFromStatus(const boost::json::object& payload) {
    const auto entry = payload.find("preview");
    if (entry == payload.end() || !entry->value().is_object()) {
        return;  // a worker too old to report a preview
    }
    const boost::json::object& preview = entry->value().as_object();
    fPreviewDropped = static_cast<qint64>(preview.at("dropped").to_number<std::uint64_t>());
    fPreviewError = QString::fromStdString(std::string(preview.at("error").as_string()));
    // The off-air target's counters (task 6.16a). Optional, like
    // every newer field: a worker that predates the block keeps
    // whatever the last poll said rather than reading as zero.
    const auto selection = preview.find("selection");
    if (selection != preview.end() && selection->value().is_object()) {
        const boost::json::object& offAir = selection->value().as_object();
        const auto stale = offAir.find("stale");
        if (stale != offAir.end()) {
            fSelectionStale = static_cast<qint64>(stale->value().to_number<std::uint64_t>());
        }
        const auto active = offAir.find("active");
        if (active != offAir.end() && active->value().is_bool() && !active->value().as_bool() &&
            !fSelectionAskedJson.isEmpty() && !fPendingPreviewName.empty()) {
            // The worker has no target registered while this client
            // believes it asked for one: a stand-down it made, or a
            // stream that came up without the ask it was owed. The
            // ask is re-issued rather than argued about, because the
            // canvas showing yesterday's slide is the failure this
            // whole task exists to prevent.
            const QString wanted = fSelectionWantedJson;
            fSelectionAskedJson.clear();
            if (!wanted.isEmpty()) {
                requestSelectionPreview(wanted);
            }
        }
    }
    if (!preview.at("running").as_bool() && fMap) {
        closeStream();  // the worker retired the stream
        fPendingPreviewName.clear();
    }
    // The list's counters (task 6.16b), optional like every newer
    // field: a worker that predates the block keeps whatever the last
    // poll said rather than reading as zero.
    const auto thumbnail = preview.find("thumbnail");
    if (thumbnail != preview.end() && thumbnail->value().is_object()) {
        const boost::json::object& list = thumbnail->value().as_object();
        const auto stale = list.find("stale");
        if (stale != list.end()) {
            fThumbnailStale = static_cast<qint64>(stale->value().to_number<std::uint64_t>());
        }
        const auto active = list.find("active");
        // The re-issue rule for a batch, which is the selection's with
        // one difference: a list is a SET, so "the worker is not
        // showing what I last asked for" is the whole test. It waits
        // for no ack that is already on its way, and it never fires for
        // a list the operator is not looking at — an empty wanted
        // payload means exactly that. A worker that dropped the batch
        // (a stream that came up without it, a stand-down it made
        // itself, a refusal whose reply went missing) is answered by
        // asking again rather than argued about, because a slide list of
        // text lines is the thing this task exists to replace.
        const bool workerActive =
            active != list.end() && active->value().is_bool() && active->value().as_bool();
        if (!fThumbnailWantedPayload.isEmpty() && !fThumbnailAwaitAck && !workerActive &&
            !fPendingPreviewName.empty()) {
            sendThumbnailBatch(fThumbnailWantedPayload, fThumbnailWantedWidth,
                               fThumbnailWantedHeight);
        }
    }
    emit changed();
}

void PreviewConsumer::handleReady(const boost::json::object& params) {
    // Stream identity (task 6.5, review R5): a ready naming anything
    // other than the stream this client currently owns describes slots
    // of a segment it does not have — ignore it entirely, copying nothing
    // and releasing nothing (a delayed release of an old stream must
    // never free a live stream's slot).
    const auto streamIt = params.find("stream");
    if (streamIt == params.end() || !streamIt->value().is_string() ||
        std::string_view(streamIt->value().as_string()) != fPendingPreviewName) {
        return;
    }

    // First ready proves the segment exists: this is where a pending
    // (not-yet-mapped) stream gets opened, retrying if we beat the worker.
    if (fMap == nullptr && !fPendingPreviewName.empty()) {
        openSegment(fPendingPreviewName, fPreviewSlotStride);
    }

    auto number = [&params](const char* key) -> std::uint64_t {
        const auto entry = params.find(key);
        return entry == params.end() ? 0 : entry->value().to_number<std::uint64_t>();
    };
    const std::uint64_t slot = number("slot");
    const std::uint64_t seq = number("seq");
    const std::uint64_t width = number("width");
    const std::uint64_t height = number("height");
    const std::uint64_t rowBytes = number("rowBytes");
    // Whose picture this is, and which ask answers it (task 6.16a). A worker
    // too old to send the fields means the audience, which is what version 1
    // of this stream could only ever carry.
    const std::uint64_t target = number("target");
    const std::uint32_t request = static_cast<std::uint32_t>(number("request"));
    const bool forSelection = target == proto::kPreviewTargetSelection;
    // The list's target (task 6.16b). A thumbnail's `revision` is not a scene
    // revision at all — it is the id the ack gave this slide, because a scene
    // the store has never held has no other name.
    const bool forThumbnail = target == proto::kPreviewTargetThumbnail;
    const std::uint32_t frameId = static_cast<std::uint32_t>(number("revision"));
    // THE CLIENT'S GENERATION GATE, the consumer's half of the rule. A frame
    // answering an ask this client has already superseded is yesterday's edit
    // or last scroll's band: it is released like any other slot and never
    // painted, because putting it under the handles of the slide now being
    // edited — or under a row the operator has since scrolled past — is
    // precisely the failure R16 reported.
    const bool superseded = (forSelection && request != fSelectionRequest) ||
                            (forThumbnail && request != fThumbnailAcked);
    // A thumbnail whose id no accepted batch named describes a slide this
    // client does not have on its list: an id from a band that has since been
    // replaced, or from a batch whose ack has not arrived yet. Either way
    // there is no row to put it in, and guessing from the order frames
    // arrived in is how a picture ends up on the wrong slide.
    const bool unmapped = forThumbnail && !fThumbnailKeys.contains(frameId);

    // Copy the payload under the slot's seqlock: same sequence before and
    // after means the producer never touched it mid-copy. A stale or torn
    // frame is simply skipped; the next notification brings a newer one.
    bool fresh = false;
    if (!superseded && !unmapped && fMap && slot < fPreviewSlotCount &&
        rowBytes == width * 4 && width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
        sizeof(proto::PreviewSlotHeader) + rowBytes * height <= fPreviewSlotStride) {
        const std::uint8_t* slotBase =
            fMap + proto::previewSlotOffset(static_cast<std::uint32_t>(slot), fPreviewSlotStride);
        const auto* header = reinterpret_cast<const proto::PreviewSlotHeader*>(slotBase);
        const std::uint64_t seqBefore =
            std::atomic_ref<const std::uint64_t>(header->seq).load();
        // The slot's own geometry, cross-checked against the notification
        // (task 6.16b): one stream now carries frames of DIFFERENT sizes —
        // the mirror at the stream's extent, a thumbnail at the band's — and
        // the copy below is bounded by the notification's numbers. If the
        // header the same sequence stamps disagrees, the two are not the same
        // frame, and the safe answer is to copy nothing.
        const bool geometryAgrees =
            header->width == width && header->height == height && header->rowBytes == rowBytes;
        if (seqBefore == seq && (seqBefore & 1) == 0 && geometryAgrees) {
            QImage image(static_cast<int>(width), static_cast<int>(height),
                         QImage::Format_RGBA8888);
            if (!image.isNull()) {
                std::memcpy(image.bits(), slotBase + sizeof(proto::PreviewSlotHeader),
                            static_cast<std::size_t>(rowBytes) * height);
                const std::uint64_t seqAfter =
                    std::atomic_ref<const std::uint64_t>(header->seq).load();
                if (seqAfter == seqBefore) {
                    if (forThumbnail) {
                        const QString key = fThumbnailKeys.value(frameId);
                        // The key can only have disappeared between the two
                        // checks above and here if a batch was accepted
                        // mid-copy, on this same thread — which it cannot be.
                        // Belt: a frame with nowhere to go is dropped.
                        if (!key.isEmpty()) {
                            storeThumbnail(key, std::move(image));
                            fresh = true;
                        }
                    } else if (forSelection) {
                        fSelectionImage = std::move(image);
                        fSelectionImageRequest = request;
                        ++fSelectionFrames;
                    } else {
                        fPreviewImage = std::move(image);
                        ++fPreviewFrames;
                    }
                }
            }
        }
    }

    // Ownership of the slot ends whether or not this copy worked out; the
    // producer drops rather than waits, but there is no reason to make it.
    // The echo carries the stream identity the ready came with (task 6.5).
    fOut.notification(proto::kMethodPreviewReleased,
                      boost::json::object{{"slot", static_cast<std::int64_t>(slot)},
                                          {"seq", static_cast<std::int64_t>(seq)},
                                          {"stream", fPendingPreviewName}});
    if (fresh) {
        emit changed();
    }
}

}  // namespace openshow::ui
