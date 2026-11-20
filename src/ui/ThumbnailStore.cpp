#include "ThumbnailStore.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>

namespace openshow::ui {

namespace {

// A job never touches the store's state: it is a pure function of the bytes it
// captured, and its answer returns through the store's thread. Two lanes, each
// bounded, because the UI thread is where the operator's frames are.
class DecodeJob : public QRunnable {
public:
    QPointer<ThumbnailStore> store;
    std::uint64_t generation = 0;
    std::uint64_t serial = 0;
    QString slideId;
    QString fingerprint;
    std::string encoded;
    QByteArray checksum;
    std::uint32_t expectedWidth = 0;
    std::uint32_t expectedHeight = 0;
    qint64 width = 0;
    qint64 height = 0;

    void run() override {
        QImage picture;
        QString error;
        std::string decoded;
        if (!openshow::domain::base64Decode(encoded, &decoded)) {
            error = QStringLiteral("thumbnail record: invalid Base64");
        }
        const QByteArray png(decoded.data(), static_cast<qsizetype>(decoded.size()));
        // The record is trusted only after its bytes prove they are the bytes
        // that were recorded, and only after the PNG header proves the size the
        // record declares. A corrupt blob never reaches the decoder.
        if (!error.isEmpty()) {
            // A malformed record is a miss without invoking the PNG decoder.
        } else if (openshow::domain::pngChecksum(
                std::string_view(png.constData(), static_cast<std::size_t>(png.size()))) !=
            std::string_view(checksum.constData(), static_cast<std::size_t>(checksum.size()))) {
            error = QStringLiteral("thumbnail record: the checksum does not match the picture");
        } else {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (!openshow::domain::pngDimensions(
                    std::string_view(png.constData(), static_cast<std::size_t>(png.size())),
                    &width, &height)) {
                error = QStringLiteral("thumbnail record: the picture is not a PNG");
            } else if (width != expectedWidth || height != expectedHeight) {
                error = QStringLiteral("thumbnail record: the picture's size disagrees");
            } else {
                picture = QImage::fromData(png, "PNG");
                if (picture.isNull()) {
                    error = QStringLiteral("thumbnail record: the PNG could not be decoded");
                } else if (picture.width() != static_cast<int>(expectedWidth) ||
                           picture.height() != static_cast<int>(expectedHeight)) {
                    error = QStringLiteral("thumbnail record: the decoder changed the picture's size");
                }
            }
        }
        // The bucket is the stored size; a row asks for the extent it paints.
        // Scaling here (and not on the UI thread) is the whole point of the
        // demand lane: a 512x288 decode for a 190-pixel card costs this thread
        // 0.4 ms and the operator's thread nothing.
        if (error.isEmpty() && !picture.isNull() && width > 0 && height > 0 &&
            (picture.width() != static_cast<int>(width) ||
             picture.height() != static_cast<int>(height))) {
            picture = picture.scaled(static_cast<int>(width), static_cast<int>(height),
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        const QPointer<ThumbnailStore> target = store;
        const std::uint64_t jobGeneration = generation;
        const std::uint64_t jobSerial = serial;
        const QString jobSlide = slideId;
        const QString jobFingerprint = fingerprint;
        if (target.isNull() || QCoreApplication::instance() == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [target, jobGeneration, jobSerial, jobSlide, jobFingerprint, picture, error] {
                if (!target.isNull()) {
                    target->finishDecode(jobGeneration, jobSerial, jobSlide, jobFingerprint,
                                         picture, error);
                }
            },
            Qt::QueuedConnection);
    }
};

class EncodeJob : public QRunnable {
public:
    QPointer<ThumbnailStore> store;
    std::uint64_t generation = 0;
    std::uint64_t serial = 0;
    QString showId;
    QString slideId;
    QString fingerprint;
    QImage rendered;

    void run() override {
        using openshow::domain::ThumbnailCacheEntry;
        ThumbnailCacheEntry entry;
        bool ok = false;
        QImage bucket = rendered;
        if (!bucket.isNull()) {
            bucket = bucket.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QByteArray png;
            {
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                bucket.save(&buffer, "PNG");
            }
            // Over the per-record bound is a miss on the next run, never a
            // failure here and never an unbounded manifest.
            if (!png.isEmpty() &&
                static_cast<std::uint64_t>(png.size()) <=
                    openshow::domain::kMaxThumbnailPngBytes) {
                const std::string bytes(png.constData(),
                                        static_cast<std::size_t>(png.size()));
                entry.showId = showId.toStdString();
                entry.slideId = slideId.toStdString();
                entry.fingerprint = fingerprint.toStdString();
                entry.renderer = openshow::domain::rendererIdentity();
                entry.width = bucket.width();
                entry.height = bucket.height();
                entry.pngBytes = static_cast<std::uint64_t>(png.size());
                entry.checksum = openshow::domain::pngChecksum(bytes);
                entry.pngBase64 = openshow::domain::base64Encode(bytes);
                ok = openshow::domain::validateThumbnailEntry(entry, nullptr);
            }
        }
        const QPointer<ThumbnailStore> target = store;
        const std::uint64_t jobGeneration = generation;
        const std::uint64_t jobSerial = serial;
        const QString jobSlide = slideId;
        const QString jobFingerprint = fingerprint;
        if (target.isNull() || QCoreApplication::instance() == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [target, jobGeneration, jobSerial, jobSlide, jobFingerprint, entry, ok] {
                if (!target.isNull()) {
                    target->finishEncode(jobGeneration, jobSerial, jobSlide, jobFingerprint, entry, ok);
                }
            },
            Qt::QueuedConnection);
    }
};

QString usageKey(const QString& slideId, const QString& fingerprint) {
    return slideId + QChar(0x1f) + fingerprint;
}

}  // namespace

ThumbnailStore::ThumbnailStore(QObject* parent) : QObject(parent) {
    fDecodePool = std::make_unique<QThreadPool>();
    fDecodePool->setMaxThreadCount(kDecodeThreads);
    fDecodePool->setObjectName(QStringLiteral("openshow-thumbnail-decode"));
    fEncodePool = std::make_unique<QThreadPool>();
    fEncodePool->setMaxThreadCount(kEncodeThreads);
    fEncodePool->setObjectName(QStringLiteral("openshow-thumbnail-encode"));
    fFlushTimer.setSingleShot(true);
    connect(&fFlushTimer, &QTimer::timeout, this, &ThumbnailStore::flushNow);
    const QByteArray off = qgetenv("OPENSHOW_THUMBNAIL_CACHE");
    if (off == "0" || off == "off" || off == "false") {
        fEnabled = false;
    }
}

ThumbnailStore::~ThumbnailStore() {
    // Jobs must be finished before the state they answer into is gone. The
    // store is the object the queued answer checks through a QPointer, so a
    // job that was already running sees a dead target and drops its result.
    fDecodePool->waitForDone();
    fEncodePool->waitForDone();
}

void ThumbnailStore::raiseGeneration() {
    ++fGeneration;
    fDecoding.clear();
    fEncoding.clear();
    fDecodeQueue.clear();
    fEncodeQueue.clear();
}

void ThumbnailStore::adopt(const openshow::domain::ThumbnailCache& cache,
                           const QString& authoredDigest, bool writable) {
    raiseGeneration();
    fCache = cache;
    openshow::domain::sanitizeThumbnailCache(&fCache);
    fUsage.clear();
    for (const openshow::domain::ThumbnailCacheEntry& entry : fCache.entries) {
        fUsage.push_back(usageKey(QString::fromStdString(entry.slideId),
                                  QString::fromStdString(entry.fingerprint)));
    }
    fAuthoredDigest = authoredDigest;
    fWritable = writable;
    fDirty = false;
    fFlushTimer.stop();
    emit countersChanged();
}

void ThumbnailStore::setWritable(bool writable) {
    fWritable = writable;
    if (!writable) {
        fFlushTimer.stop();
    } else if (fDirty) {
        scheduleFlush();
    }
}

void ThumbnailStore::markPersisted() {
    fDirty = false;
    fFlushTimer.stop();
}

void ThumbnailStore::setEnabled(bool enabled) {
    if (enabled == fEnabled) {
        return;
    }
    fEnabled = enabled;
    if (!enabled) {
        fFlushTimer.stop();
        raiseGeneration();
        fCache.entries.clear();
        fUsage.clear();
        fDirty = false;
        emit countersChanged();
    }
}

void ThumbnailStore::pruneToSlides(const QSet<QString>& slideIds) {
    for (const QString& id : fEncoding.keys()) {
        if (!slideIds.contains(id)) {
            fEncoding.remove(id);
            fEncodeQueue.removeAll(id);
        }
    }
    for (const QString& id : fDecoding.keys()) {
        if (!slideIds.contains(id)) {
            fDecoding.remove(id);
            fDecodeQueue.removeAll(id);
        }
    }
    const std::size_t before = fCache.entries.size();
    std::vector<openshow::domain::ThumbnailCacheEntry> kept;
    kept.reserve(fCache.entries.size());
    for (const openshow::domain::ThumbnailCacheEntry& entry : fCache.entries) {
        if (slideIds.contains(QString::fromStdString(entry.slideId))) {
            kept.push_back(entry);
        }
    }
    if (kept.size() == before) {
        return;
    }
    fCache.entries = std::move(kept);
    QStringList usage;
    for (const openshow::domain::ThumbnailCacheEntry& entry : fCache.entries) {
        usage.push_back(usageKey(QString::fromStdString(entry.slideId),
                                 QString::fromStdString(entry.fingerprint)));
    }
    fUsage = usage;
    fDirty = true;
    scheduleFlush();
    emit countersChanged();
}

void ThumbnailStore::request(const QString& slideId, const QString& fingerprint, qint64 width,
                             qint64 height) {
    if (!fEnabled || slideId.isEmpty() || fingerprint.isEmpty()) {
        ++fMisses;
        emit pictureMissed(slideId, fingerprint);
        return;
    }
    const openshow::domain::ThumbnailCacheEntry* entry =
        openshow::domain::findThumbnail(fCache, slideId.toStdString(),
                                        fingerprint.toStdString());
    if (entry == nullptr || width <= 0 || height <= 0 ||
        width > entry->width || height > entry->height ||
        (!fDecoding.contains(slideId) && fDecoding.size() >= kMaxPendingJobs)) {
        ++fMisses;
        emit pictureMissed(slideId, fingerprint);
        return;
    }
    const auto previous = fDecoding.constFind(slideId);
    if (previous != fDecoding.constEnd() && previous->fingerprint == fingerprint &&
        previous->width == width && previous->height == height) {
        return;
    }
    PendingDecode pending;
    pending.generation = fGeneration;
    pending.serial = ++fSerial;
    pending.width = width;
    pending.height = height;
    pending.fingerprint = fingerprint;
    fDecoding.insert(slideId, pending);
    fDecodeQueue.removeAll(slideId);
    fDecodeQueue.push_back(slideId);
    pumpJobs();
}

void ThumbnailStore::startDecode(const QString& slideId, const QString& fingerprint,
                                 const PendingDecode& pending) {
    const openshow::domain::ThumbnailCacheEntry* entry =
        openshow::domain::findThumbnail(fCache, slideId.toStdString(),
                                        fingerprint.toStdString());
    if (entry == nullptr) {
        fDecoding.remove(slideId);
        --fActiveDecodes;
        ++fMisses;
        emit pictureMissed(slideId, fingerprint);
        return;
    }
    auto* job = new DecodeJob;
    job->setAutoDelete(true);
    job->store = this;
    job->generation = pending.generation;
    job->serial = pending.serial;
    job->slideId = slideId;
    job->fingerprint = fingerprint;
    job->encoded = entry->pngBase64;
    job->checksum = QByteArray(entry->checksum.data(), static_cast<qsizetype>(entry->checksum.size()));
    job->expectedWidth = entry->width;
    job->expectedHeight = entry->height;
    job->width = pending.width;
    job->height = pending.height;
    fDecodePool->start(job);
}

void ThumbnailStore::finishDecode(std::uint64_t generation, std::uint64_t serial,
                                  const QString& slideId, const QString& fingerprint,
                                  const QImage& image, const QString& error) {
    --fActiveDecodes;
    const auto pending = fDecoding.constFind(slideId);
    if (generation != fGeneration || pending == fDecoding.constEnd() ||
        pending->serial != serial) {
        pumpJobs();
        return;
    }
    fDecoding.erase(pending);
    if (!error.isEmpty() || image.isNull()) {
        ++fFailures;
        ++fMisses;
        emit pictureMissed(slideId, fingerprint);
        emit countersChanged();
        pumpJobs();
        return;
    }
    touch(slideId, fingerprint);
    ++fDecoded;
    ++fHits;
    emit pictureReady(slideId, fingerprint, image);
    emit countersChanged();
    pumpJobs();
}

void ThumbnailStore::record(const QString& showId, const QString& slideId,
                            const QString& fingerprint, const QImage& rendered) {
    if (!fEnabled || rendered.isNull() || slideId.isEmpty() || fingerprint.isEmpty() ||
        rendered.width() > static_cast<int>(openshow::domain::kThumbnailWidth) ||
        rendered.height() > static_cast<int>(openshow::domain::kThumbnailHeight)) {
        return;
    }
    const auto* existing = find(slideId, fingerprint);
    if (existing != nullptr && existing->width >= static_cast<unsigned>(rendered.width()) &&
        existing->height >= static_cast<unsigned>(rendered.height())) {
        return;  // never replace a sharp record with a smaller proxy
    }
    const auto old = fEncoding.constFind(slideId);
    if (old != fEncoding.constEnd() && old->fingerprint == fingerprint &&
        old->width >= rendered.width() && old->height >= rendered.height()) {
        return;
    }
    const qint64 replacedBytes = old == fEncoding.constEnd() ? 0 : old->picture.sizeInBytes();
    if ((!fEncoding.contains(slideId) && fEncoding.size() >= kMaxPendingJobs) ||
        queuedEncodeBytes() - replacedBytes + rendered.sizeInBytes() > kMaxQueuedEncodeBytes) {
        return;  // optional persistence defers rather than growing a job backlog
    }
    // The newest render for a slide supersedes an encode still in flight: the
    // older pixels describe a scene the operator has already changed.
    PendingEncode pending;
    pending.generation = fGeneration;
    pending.serial = ++fSerial;
    pending.showId = showId;
    pending.slideId = slideId;
    pending.fingerprint = fingerprint;
    pending.width = rendered.width();
    pending.height = rendered.height();
    pending.picture = rendered;
    fEncoding.insert(slideId, pending);
    fEncodeQueue.removeAll(slideId);
    fEncodeQueue.push_back(slideId);
    pumpJobs();
}

void ThumbnailStore::startEncode(PendingEncode pending) {
    auto* job = new EncodeJob;
    job->setAutoDelete(true);
    job->store = this;
    job->generation = pending.generation;
    job->serial = pending.serial;
    job->showId = pending.showId;
    job->slideId = pending.slideId;
    job->fingerprint = pending.fingerprint;
    job->rendered = pending.picture;
    fEncodePool->start(job);
}

void ThumbnailStore::finishEncode(std::uint64_t generation, std::uint64_t serial,
                                  const QString& slideId, const QString& fingerprint,
                                  const openshow::domain::ThumbnailCacheEntry& entry, bool ok) {
    --fActiveEncodes;
    const auto pending = fEncoding.constFind(slideId);
    if (generation != fGeneration || pending == fEncoding.constEnd() ||
        pending->serial != serial || pending->fingerprint != fingerprint) {
        pumpJobs();
        return;
    }
    fEncoding.erase(pending);
    if (ok) {
        insertEntry(entry);
        fDirty = true;
        scheduleFlush();
        emit recordStored(QString::fromStdString(entry.showId), slideId, fingerprint);
    } else {
        ++fFailures;
    }
    emit countersChanged();
    pumpJobs();
}

qint64 ThumbnailStore::queuedEncodeBytes() const {
    qint64 bytes = 0;
    for (const auto& pending : fEncoding) {
        bytes += pending.picture.sizeInBytes();
    }
    return bytes;
}

void ThumbnailStore::pumpJobs() {
    // Only submit a job when its lane has a permit. QThreadPool's internal
    // queue never accumulates images; the bounded maps own pending work.
    while (fActiveDecodes < kDecodeThreads && !fDecodeQueue.isEmpty()) {
        const QString id = fDecodeQueue.takeFirst();
        auto pending = fDecoding.find(id);
        if (pending == fDecoding.end()) continue;
        const auto job = *pending;
        ++fActiveDecodes;
        startDecode(id, job.fingerprint, job);
    }
    while (fActiveEncodes < kEncodeThreads && !fEncodeQueue.isEmpty()) {
        const QString id = fEncodeQueue.takeFirst();
        auto pending = fEncoding.find(id);
        if (pending == fEncoding.end()) continue;
        const auto job = *pending;
        pending->picture = QImage();
        ++fActiveEncodes;
        startEncode(job);
    }
}

void ThumbnailStore::adoptEntry(const openshow::domain::ThumbnailCacheEntry& entry) {
    if (!fEnabled || entry.slideId.empty() || !openshow::domain::validateThumbnailEntry(entry, nullptr)) {
        return;
    }
    insertEntry(entry);
    fDirty = true;
    scheduleFlush();
    emit countersChanged();
}

const openshow::domain::ThumbnailCacheEntry* ThumbnailStore::find(
    const QString& slideId, const QString& fingerprint) const {
    return openshow::domain::findThumbnail(fCache, slideId.toStdString(),
                                           fingerprint.toStdString());
}

void ThumbnailStore::insertEntry(openshow::domain::ThumbnailCacheEntry entry) {
    const QString slideId = QString::fromStdString(entry.slideId);
    const QString fingerprint = QString::fromStdString(entry.fingerprint);
    for (auto& existing : fCache.entries) {
        if (existing.slideId == entry.slideId && existing.fingerprint == entry.fingerprint) {
            existing = std::move(entry);
            touch(slideId, fingerprint);
            pruneToBounds();
            return;
        }
    }
    fCache.entries.push_back(std::move(entry));
    fUsage.push_back(usageKey(slideId, fingerprint));
    ++fEncoded;
    pruneToBounds();
}

void ThumbnailStore::touch(const QString& slideId, const QString& fingerprint) {
    const QString key = usageKey(slideId, fingerprint);
    fUsage.removeAll(key);
    fUsage.push_back(key);
}

void ThumbnailStore::pruneToBounds() {
    const qint64 budget = static_cast<qint64>(openshow::domain::kMaxThumbnailEncodedBytes);
    while (fCache.entries.size() > openshow::domain::kMaxThumbnailCacheEntries ||
           encodedBytes() > budget) {
        if (fUsage.isEmpty()) {
            break;
        }
        const QString key = fUsage.takeFirst();
        const int separator = key.indexOf(QChar(0x1f));
        const std::string slideId = key.left(separator).toStdString();
        const std::string fingerprint = key.mid(separator + 1).toStdString();
        fCache.entries.erase(
            std::remove_if(fCache.entries.begin(), fCache.entries.end(),
                           [&](const openshow::domain::ThumbnailCacheEntry& entry) {
                               return entry.slideId == slideId &&
                                      entry.fingerprint == fingerprint;
                           }),
            fCache.entries.end());
    }
}

qint64 ThumbnailStore::encodedBytes() const {
    qint64 bytes = 0;
    for (const openshow::domain::ThumbnailCacheEntry& entry : fCache.entries) {
        bytes += static_cast<qint64>(entry.pngBase64.size());
    }
    return bytes;
}

bool ThumbnailStore::has(const QString& slideId, const QString& fingerprint) const {
    return openshow::domain::findThumbnail(fCache, slideId.toStdString(),
                                           fingerprint.toStdString()) != nullptr;
}

void ThumbnailStore::forget(const QString& slideId) {
    const std::size_t before = fCache.entries.size();
    fCache.entries.erase(std::remove_if(fCache.entries.begin(), fCache.entries.end(),
                                        [&](const openshow::domain::ThumbnailCacheEntry& entry) {
                                            return entry.slideId == slideId.toStdString();
                                        }),
                         fCache.entries.end());
    for (int i = fUsage.size() - 1; i >= 0; --i) {
        if (fUsage.at(i).startsWith(slideId + QChar(0x1f))) {
            fUsage.removeAt(i);
        }
    }
    fEncoding.remove(slideId);
    fDecoding.remove(slideId);
    fEncodeQueue.removeAll(slideId);
    fDecodeQueue.removeAll(slideId);
    if (fCache.entries.size() != before) {
        fDirty = true;
        scheduleFlush();
        emit countersChanged();
    }
}

bool ThumbnailStore::idle() const {
    return fDecoding.isEmpty() && fEncoding.isEmpty() && fActiveDecodes == 0 &&
           fActiveEncodes == 0;
}

bool ThumbnailStore::waitForIdle(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!idle()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    // One more turn so the queued answers that made the lanes idle are handled.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return true;
}

void ThumbnailStore::scheduleFlush() {
    if (!fDirty || !fWritable || !fFlushHook || fAuthoredDigest.isEmpty() || fFlushInFlight) {
        return;
    }
    if (!fFlushTimer.isActive()) {
        fFlushTimer.start(static_cast<int>(kFlushDelayMs));
    }
}

void ThumbnailStore::flushNow() {
    if (!fEnabled || !fDirty || !fWritable || !fFlushHook || fAuthoredDigest.isEmpty() ||
        fFlushInFlight) {
        return;
    }
    QString error;
    fFlushInFlight = true;
    const bool written = fFlushHook(fCache, &error);
    fFlushInFlight = false;
    if (!written) {
        // The package is not ours to touch right now (an external change, a
        // save in flight, a read-only tree). The entries stay in memory and
        // the next edit or save carries them; a later attempt retries.
        ++fFlushFailures;
        fFlushTimer.start(5000);
        emit countersChanged();
        return;
    }
    fDirty = false;
    ++fFlushWrites;
    // Never more than one cache-only write in this window: the flush is a
    // coalescing courtesy, not a write per render.
    fFlushTimer.start(5000);
    emit countersChanged();
}

}  // namespace openshow::ui
