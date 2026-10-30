/*
 * PreviewConsumer: the frontend's picture half of the worker conversation —
 * the shared-memory stream the worker writes, the frames copied out of it, and
 * the three targets those frames name: the audience mirror, the editor's
 * off-air slide, and the slide list's thumbnails.
 *
 * It is deliberately the client's half of a preview and nothing else. It never
 * owns a socket: its asks go out through the callbacks its host injects, and
 * the only replies it interprets are the ones for the methods it sent — the
 * start answer that names a segment, the thumbnails ack that hands out per-slide
 * ids, and the prepare ack that tells it the scene an off-air ask names exists.
 * Frames arrive on one notification, and the status poll's preview block is the
 * only part of any other answer it reads.
 *
 * What the transport knows about pictures is zero; what this class knows about
 * transports is zero. That is the seam task 6.19a2 exists to make, so the rules
 * below — the generation gate, the byte-bounded cache, the stand-down that
 * re-issues rather than argues — can be read in one file.
 */

#pragma once

#include <boost/json.hpp>

#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "SceneAsk.h"

namespace openshow::ui {

class PreviewConsumer : public QObject {
    Q_OBJECT
public:
    // What the consumer needs from whoever owns the transport, injected rather
    // than reached for: sending is the session's job, and so is the answer to
    // "would the worker refuse this scene?" — a rule with one owner (task 6.8a)
    // is a rule the editor and the list cannot disagree with the presenter about.
    struct Outbound {
        // Send a request the worker answers with a task id. `intent` travels
        // with it and comes back on the reply, unsniffed by the transport (the
        // one this class sends is ScenePrepareAsk, in SceneAsk.h); a preview ask
        // always passes one, since it is a scene.prepare or a batch whose ack
        // has to find the ask it belongs to.
        std::function<std::int64_t(const char* method, boost::json::object params,
                                   std::shared_ptr<void> intent)>
            request;
        // Send a message that owes no answer (the slot-release echo).
        std::function<void(const char* method, boost::json::object params)> notification;
        // The scene budgets this build shares with the worker: false means the
        // payload must not go on the wire, with the reason already told to the
        // operator.
        std::function<bool(const boost::json::value& candidate, const char* verb)> sendable;
    };

    PreviewConsumer(Outbound outbound, QObject* parent = nullptr);
    ~PreviewConsumer() override;

    // The host's session state, as the preview rules read it: whether the
    // worker is identified well enough to be asked for anything (a want
    // remembered while there was nobody to ask is the whole reason a cold start
    // ends with a picture rather than a placeholder), and which of the two
    // askable targets its handshake named.
    void setSessionLive(bool live);
    void setCapabilities(bool selectionCapable, bool thumbnailCapable);

    // The editor's off-air picture (task 6.16a) and the slide list's band
    // (task 6.16b): what to show, and the stand-down that stops it.
    void requestSelectionPreview(const QString& sceneJson);
    void clearSelectionPreview();
    void requestThumbnails(const QVariantList& slides, qint64 width, qint64 height);
    void clearThumbnails();

    // The transport's preview-typed events, handed over by the session that
    // routes replies by the method they went out under.
    void handleStartReply(const boost::json::object& payload);
    void handleReady(const boost::json::object& params);
    void handleThumbnailsAck(const ScenePrepareAsk& intent, const boost::json::object& ack);
    void handlePrepareAck(const ScenePrepareAsk& intent, qint64 revision);
    // A refused request whose intent carried a preview ask: the ask was never
    // MADE, so "asked" goes back to empty and the status poll re-issues it.
    void handleRefusal(const ScenePrepareAsk& intent);
    // The preview block of output.status, which is where the worker's own
    // counters live and where a target the worker has stood down is noticed.
    // A worker too old to send one leaves every counter as the last poll said.
    void updateFromStatus(const boost::json::object& payload);
    // The stream is over (a new one is starting, the connection went away, or
    // this side is): unmap, and drop everything that lived in the mapping.
    void closeStream();

    // The truth QML reads. Every one of these is a fact about this consumer's
    // own copies and gates, never a guess about what the worker meant.
    const QImage& previewImage() const { return fPreviewImage; }
    bool previewActive() const { return fMap != nullptr; }
    qint64 previewFrames() const { return fPreviewFrames; }
    qint64 previewDropped() const { return fPreviewDropped; }
    QString previewError() const { return fPreviewError; }
    // The off-air edit frame: empty until the worker has drawn the slide under
    // edit — callers paint the document's own placeholder then.
    const QImage& selectionImage() const { return fSelectionImage; }
    bool selectionReady() const { return !fSelectionImage.isNull(); }
    qint64 selectionFrames() const { return fSelectionFrames; }
    qint64 selectionStale() const { return fSelectionStale; }
    bool selectionSupported() const { return fSelectionCapable; }
    // Keyed lookup, no assembly: the bytes are the worker's, and a key the
    // cache has not received yet simply has no image — the caller shows its
    // text line until the worker's version arrives.
    bool thumbnailSupported() const { return fThumbnailCapable; }
    qint64 thumbnailFrames() const { return fThumbnailFrames; }
    qint64 thumbnailStale() const { return fThumbnailStale; }
    qint64 thumbnailCachedBytes() const { return fThumbnailBytes; }
    QImage thumbnail(const QString& key) const;

signals:
    // Any change to a picture, a counter, or a capability. The host forwards
    // this as its own preview signal, which is what the views bind to.
    void changed();
    // One slide's picture arrived, keyed so a list delegate repaints its own
    // row and not the thirty around it.
    void thumbnailReady(const QString& key);
    // A preview-side refusal the operator has to see: an ask that cannot fit
    // the stream this client mapped, a batch that cannot be read.
    void statusMessage(const QString& text);

private:
    // The batch's own send: serialize what the UI wants, advance this client's
    // generation, and record what the socket carried. Split from
    // requestThumbnails() because three places need it — a row entering the
    // band, a stream that came up without the ask it was owed, and the
    // stand-down, which is the same message with an empty scenes array.
    void sendThumbnailBatch(const QString& scenesPayload, qint64 width, qint64 height);
    // One picture in the byte-bounded LRU (arrival is the recency), and one
    // out of it. A slide the worker has never rendered has no entry at all,
    // which is what its row's text line is for.
    void storeThumbnail(const QString& key, QImage image);
    void dropThumbnailPicture(const QString& key);
    void clearThumbnailPictures();

    void openSegment(const std::string& name, std::uint64_t bytesPerSlot);

    Outbound fOut;

    // Preview consumer state (UI thread only): the read-only mapping of
    // the worker's shm segment and the frame last copied out of it.
    std::uint8_t* fMap = nullptr;
    int fMapFd = -1;
    std::uint64_t fMapBytes = 0;
    std::uint64_t fPreviewSlotStride = 0;
    std::uint64_t fPreviewSlotCount = 0;
    // The start answer names a segment the render thread has not
    // necessarily created yet; it is mapped on the first ready.
    std::string fPendingPreviewName;
    QImage fPreviewImage;
    qint64 fPreviewFrames = 0;
    qint64 fPreviewDropped = 0;
    QString fPreviewError;
    // Whether the session is live enough to ask, and what the worker's
    // handshake said it can draw. Owned here because every rule below needs
    // them, and they change together at the handshake.
    bool fHostOnline = false;
    bool fSelectionCapable = false;
    bool fThumbnailCapable = false;

    // The editor's off-air target (task 6.16a, review R16), same stream and
    // same copy rules as the mirror.
    //
    // fSelectionRequest is this consumer's own generation counter: one counter,
    // advanced on every ask the socket actually carries. The worker accepts a
    // strictly later one and refuses the rest, so a reply that arrives after
    // the operator moved on, a retried request, or a queued frame from before
    // a stream restart cannot put an old scene back on screen.
    // fSelectionImageRequest records WHICH ask the picture being shown
    // answers: a frame for anything else is released like any other and
    // dropped rather than painted under today's handles.
    std::uint32_t fSelectionRequest = 0;
    std::uint32_t fSelectionImageRequest = 0;
    // The payload the newest ask carried, and the one the UI wants shown.
    // The first dedupes a debounced editor that has not really changed; the
    // second survives a stream restart, which resets the worker's generation
    // domain and needs the ask made again from scratch.
    QString fSelectionAskedJson;
    QString fSelectionWantedJson;
    QImage fSelectionImage;
    qint64 fSelectionFrames = 0;
    qint64 fSelectionStale = 0;  // the worker's counter, mirrored from status

    // The slide list's target (task 6.16b, review R16): one batch of transient
    // scenes, one ask at a time, one byte-bounded picture cache.
    //
    // fThumbnailRequest is this consumer's own generation counter for the
    // batch (independent of the edit target's: the two ask separately and
    // neither may advance the other's), and fThumbnailAcked is the newest one
    // the worker said yes to. A frame is only painted when its `request` is
    // that accepted generation AND its id is one the ack named for a slide
    // this client still lists — so a picture of a band that has been scrolled
    // away cannot land under a visible row.
    std::uint32_t fThumbnailRequest = 0;
    std::uint32_t fThumbnailAcked = 0;
    bool fThumbnailAwaitAck = false;
    // What the UI wants shown (the serialized "scenes" array plus its extent)
    // and what the socket last carried, deduplicated: the list re-asks when a
    // row actually enters or leaves the band, and an unchanged band is not
    // re-rendered because a handler fired twice. The wanted copy survives a
    // stream restart, which is where the ask has to be made again from
    // scratch — the same reason the edit target keeps its own two strings.
    QString fThumbnailWantedPayload;
    qint64 fThumbnailWantedWidth = 0;
    qint64 fThumbnailWantedHeight = 0;
    QString fThumbnailAskedPayload;
    qint64 fThumbnailAskedWidth = 0;
    qint64 fThumbnailAskedHeight = 0;
    // The worker's id for each slide of the accepted batch, and the reverse:
    // the key a copied frame's id belongs to. Both live only as long as the
    // batch that named them.
    QHash<QString, std::uint32_t> fThumbnailIds;
    QHash<std::uint32_t, QString> fThumbnailKeys;
    // The scene text each accepted slide was asked with, kept for exactly as
    // long as the ask: a copied picture records what it was rendered FROM, so
    // the next time that slide is listed with different words its old picture
    // is recognised as stale and dropped instead of shown.
    QHash<QString, QString> fThumbnailAskedScenes;
    // The pictures, LRU by arrival and bounded TWICE: by count and by decoded
    // bytes. A slide list is as long as a document, and "keep every picture we
    // were ever sent" is precisely the growth this task must not have.
    struct ThumbnailPicture {
        QImage image;
        // The scene text this picture was rendered from, so re-asking for a
        // slide whose content changed drops the old picture instead of
        // showing it under a new one.
        QString sourceJson;
    };
    QHash<QString, ThumbnailPicture> fThumbnailCache;
    QStringList fThumbnailOrder;  // front = least recently received
    qint64 fThumbnailBytes = 0;
    qint64 fThumbnailFrames = 0;
    qint64 fThumbnailStale = 0;  // the worker's counter, mirrored from status
    static constexpr std::size_t kThumbnailCacheEntries = 64;
    static constexpr qint64 kThumbnailCacheBytes = 4 * 1024 * 1024;
};

}  // namespace openshow::ui
