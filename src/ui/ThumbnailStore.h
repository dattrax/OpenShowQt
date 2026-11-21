/*
 * ThumbnailStore: the runtime overlay of persisted service-card pictures
 * (TC.4–TC.6).
 *
 * The service codec (openshow/domain/thumbnails.h) owns the record contract and
 * the file; this class owns the pictures in memory. It decodes a demanded
 * record on a bounded background lane, encodes a finished worker render the
 * same way, and keeps the authoritative record list within the agreed count and
 * byte ceilings. `PreviewConsumer` asks it first and records into it last; the
 * document session adopts it at load/save and provides the cache-only flush.
 *
 * The rules that make it safe to sit on the UI thread:
 *
 *   - no job touches this object's state: decode/encode are pure functions of
 *     their captured bytes, and their results return through a queued call;
 *   - every adopt/clear raises a generation, so a late decode from a document
 *     that is no longer open can never paint or record anything;
 *   - a picture is delivered only if it is still the newest request for that
 *     slide, and only at the extent the caller asked for;
 *   - the record list is bounded by count and by encoded bytes, evicting least
 *     recently used first, so a large service cannot grow the manifest forever;
 *   - a failure (checksum, header, corrupt Base64, encode over budget) is a
 *     MISS with a counter, never an error the operator has to clear.
 */

#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>

#include "openshow/domain/thumbnails.h"

class QThreadPool;

namespace openshow::ui {

class ThumbnailStore : public QObject {
    Q_OBJECT
public:
    // Called when a coalesced cache-only flush is due. `snapshot` is immutable
    // for the duration of the call; it returns false with a reason when the
    // manifest must not be rewritten (an external change, a save in flight, a
    // read-only package). With no hook the store keeps records in memory only.
    using FlushHook = std::function<bool(const openshow::domain::ThumbnailCache& snapshot,
                                         QString* error)>;

    explicit ThumbnailStore(QObject* parent = nullptr);
    ~ThumbnailStore() override;

    // --- document binding ------------------------------------------------
    //
    // The loaded block, the authored digest of the manifest it came from and
    // whether a cache-only flush is authorised. Every adopt retires in-flight
    // work: a decode for the previous document's owner must never land under
    // this one. Smaller legacy operator pictures are retired here and their
    // removal is carried by the next cache flush/save.
    void adopt(const openshow::domain::ThumbnailCache& cache, const QString& authoredDigest,
               bool writable);
    // The digest a save/load just installed; the next flush must match it.
    void setAuthoredDigest(const QString& digest) { fAuthoredDigest = digest; }
    QString authoredDigest() const { return fAuthoredDigest; }
    void setWritable(bool writable);
    // The manifest now holds these records (a save or flush installed them):
    // the dirty flag clears without another write. Leaves the cache-only
    // timer stopped until the next record arrives.
    void markPersisted();
    void setFlushHook(FlushHook hook) { fFlushHook = std::move(hook); }
    // Drops records whose slide the document no longer holds (deleted shows
    // and slides), keeping the rest untouched.
    void pruneToSlides(const QSet<QString>& slideIds);

    // A running session with no writable package may still use a cache it
    // loaded; with cache disabled the store answers every lookup as a miss and
    // records nothing (TC.10's comparison runs). Also settable before first use
    // through OPENSHOW_THUMBNAIL_CACHE=0.
    void setEnabled(bool enabled);

    // --- demand lane -----------------------------------------------------

    // Ask for one slide's record at the display extent the row will paint.
    // A hit arrives asynchronously as pictureReady(); a missing, unreadable or
    // stale record answers pictureMissed() immediately, which is the caller's
    // signal to include the row in a worker batch. Repeated requests for the
    // same slide supersede each other: only the newest extent is delivered.
    void request(const QString& slideId, const QString& fingerprint, qint64 width,
                 qint64 height);

    // Keep the actual render resolution. A larger display request misses until
    // the worker supplies a sharper picture; persisted pixels are never enlarged.
    void record(const QString& showId, const QString& slideId, const QString& fingerprint,
                const QImage& rendered);
    // Drops one slide's records (an edit made its fingerprints stale).
    void forget(const QString& slideId);
    // TC.9: a Library variant copied into this service under its new slide id.
    // It becomes an ordinary service record (dirty, saved with the service).
    void adoptEntry(const openshow::domain::ThumbnailCacheEntry& entry);
    const openshow::domain::ThumbnailCacheEntry* find(const QString& slideId,
                                                     const QString& fingerprint) const;

    // --- truth for tests and diagnostics --------------------------------
    bool has(const QString& slideId, const QString& fingerprint) const;
    std::size_t entryCount() const { return fCache.entries.size(); }
    qint64 encodedBytes() const;
    qint64 hits() const { return fHits; }
    qint64 misses() const { return fMisses; }
    qint64 decodedPictures() const { return fDecoded; }
    qint64 encodedPictures() const { return fEncoded; }
    qint64 failures() const { return fFailures; }
    qint64 flushWrites() const { return fFlushWrites; }
    qint64 flushFailures() const { return fFlushFailures; }
    bool idle() const;   // no decode/encode work outstanding
    bool dirty() const { return fDirty; }
    int pendingDecodes() const { return fDecoding.size(); }
    int pendingEncodes() const { return fEncoding.size(); }
    qint64 queuedEncodeBytes() const;
    // Waits (briefly, processing queued calls) until the lanes are idle.
    bool waitForIdle(int timeoutMs = 5000);

    const openshow::domain::ThumbnailCache& cache() const { return fCache; }

    // Lane completions, posted by the job objects to this object's thread.
    // Public only because the runnables live in the implementation file; no
    // caller outside this class should invoke them.
    void finishDecode(std::uint64_t generation, std::uint64_t serial, const QString& slideId,
                      const QString& fingerprint, const QImage& image, const QString& error);
    void finishEncode(std::uint64_t generation, std::uint64_t serial, const QString& slideId,
                      const QString& fingerprint,
                      const openshow::domain::ThumbnailCacheEntry& entry, bool ok);

signals:
    void pictureReady(const QString& slideId, const QString& fingerprint, const QImage& image);
    void pictureMissed(const QString& slideId, const QString& fingerprint);
    // A worker render became a durable record. The Library bridge uses this to
    // remember the look against the catalog item the show came from.
    void recordStored(const QString& showId, const QString& slideId, const QString& fingerprint);
    void countersChanged();

private:
    struct PendingDecode {
        std::uint64_t generation = 0;
        std::uint64_t serial = 0;
        qint64 width = 0;
        qint64 height = 0;
        QString fingerprint;
    };
    struct PendingEncode {
        std::uint64_t generation = 0;
        std::uint64_t serial = 0;
        QString showId;
        QString slideId;
        QString fingerprint;
        qint64 width = 0;
        qint64 height = 0;
        QImage picture;
    };

    void startDecode(const QString& slideId, const QString& fingerprint,
                     const PendingDecode& pending);
    void startEncode(PendingEncode pending);
    void pumpJobs();
    void insertEntry(openshow::domain::ThumbnailCacheEntry entry);
    void touch(const QString& slideId, const QString& fingerprint);
    void pruneToBounds();
    void scheduleFlush();
    void flushNow();
    void raiseGeneration();

    openshow::domain::ThumbnailCache fCache;
    // Least recently used first; touches move an entry to the back.
    QStringList fUsage;
    QString fAuthoredDigest;
    bool fWritable = false;
    bool fEnabled = true;

    // One counter for document lifetimes, one per slide for demand order: a
    // decode that answers an older ask (or an older document) is discarded.
    std::uint64_t fGeneration = 1;
    std::uint64_t fSerial = 0;
    QHash<QString, PendingDecode> fDecoding;
    QHash<QString, PendingEncode> fEncoding;
    QStringList fDecodeQueue;
    QStringList fEncodeQueue;
    int fActiveDecodes = 0;
    int fActiveEncodes = 0;
    qint64 fHits = 0;
    qint64 fMisses = 0;
    qint64 fDecoded = 0;
    qint64 fEncoded = 0;
    qint64 fFailures = 0;

    FlushHook fFlushHook;
    QTimer fFlushTimer;
    qint64 fFlushWrites = 0;
    qint64 fFlushFailures = 0;
    bool fDirty = false;
    bool fFlushInFlight = false;

    std::unique_ptr<QThreadPool> fDecodePool;
    std::unique_ptr<QThreadPool> fEncodePool;

    // The agreed ceilings, from the frozen contract.
    static constexpr int kDecodeThreads = 2;
    static constexpr int kEncodeThreads = 2;
    static constexpr int kMaxPendingJobs = 128;
    static constexpr qint64 kMaxQueuedEncodeBytes = 8 * 1024 * 1024;
    static constexpr qint64 kFlushDelayMs = 2000;
};

}  // namespace openshow::ui
