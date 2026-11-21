/*
 * TC.4: the bounded background lane behind persisted card pictures.
 *
 * A record is trusted only after its checksum, PNG header and declared size
 * all agree; a corrupt one becomes a miss rather than a picture, a stall or an
 * error the operator has to clear. Decoding and encoding happen off the UI
 * thread, a newer ask for a slide supersedes an older one, and a document swap
 * retires whatever was in flight. The store keeps the record list inside the
 * frozen count and byte ceilings and asks for a cache-only flush at a
 * coalesced moment rather than on every frame.
 *
 * Qt Core+Gui only, no display: PNG encode/decode is Qt's own codec.
 */

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include <unistd.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QTimer>

#include "openshow/domain/thumbnails.h"

#include "ThumbnailStore.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::domain::ThumbnailCache;
using openshow::domain::ThumbnailCacheEntry;
using openshow::ui::ThumbnailStore;

bool waitFor(const std::function<bool()>& done, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QImage samplePicture(int width, int height, const char* words) {
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(20, 30, 60));
    QPainter painter(&image);
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("DejaVu Sans"), 18));
    painter.drawText(image.rect(), Qt::AlignCenter, QString::fromLatin1(words));
    painter.end();
    return image;
}

QByteArray encodePng(const QImage& image) {
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

std::string fingerprint(const char* suffix) {
    return openshow::domain::sha256Hex(std::string("fingerprint-") + suffix);
}

ThumbnailCacheEntry validEntry(const std::string& slideId, const std::string& fp,
                               const QImage& picture) {
    ThumbnailCacheEntry entry;
    entry.showId = "show1";
    entry.slideId = slideId;
    entry.fingerprint = fp;
    entry.renderer = openshow::domain::rendererIdentity();
    entry.width = openshow::domain::kThumbnailWidth;
    entry.height = openshow::domain::kThumbnailHeight;
    const QByteArray png = encodePng(picture);
    const std::string bytes(png.constData(), static_cast<std::size_t>(png.size()));
    entry.pngBytes = static_cast<std::uint64_t>(bytes.size());
    entry.checksum = openshow::domain::pngChecksum(bytes);
    entry.pngBase64 = openshow::domain::base64Encode(bytes);
    return entry;
}

void exerciseEncodeDecodeAndFlush() {
    ThumbnailStore store;
    int flushed = 0;
    ThumbnailCache flushedCache;
    store.setFlushHook([&](const ThumbnailCache& snapshot, QString*) {
        ++flushed;
        flushedCache = snapshot;
        return true;
    });
    QList<QImage> ready;
    QList<QPair<QString, QString>> missed;
    QObject::connect(&store, &ThumbnailStore::pictureReady,
                     [&](const QString&, const QString&, const QImage& image) { ready.append(image); });
    QObject::connect(&store, &ThumbnailStore::pictureMissed,
                     [&](const QString& slideId, const QString& fp) { missed.append({slideId, fp}); });

    store.adopt(ThumbnailCache{}, QString::fromStdString(openshow::domain::sha256Hex("manifest")), true);
    store.record(QStringLiteral("show1"), QStringLiteral("slide17"),
                 QString::fromStdString(fingerprint("17")), samplePicture(320, 180, "verse"));
    check(store.waitForIdle(), "TC.4: the encode lane finishes");
    check(store.has(QStringLiteral("slide17"), QString::fromStdString(fingerprint("17"))),
          "TC.4: a finished render becomes a record");
    check(store.encodedPictures() == 1 && store.entryCount() == 1,
          "TC.4: exactly one picture is recorded");
    const ThumbnailCacheEntry& entry = store.cache().entries.front();
    check(entry.width == 320 && entry.height == 180,
          "the persisted record preserves the actual render resolution");
    check(entry.pngBase64.size() <= openshow::domain::kMaxThumbnailPngBytes &&
              entry.pngBytes <= openshow::domain::kMaxThumbnailPngBytes,
          "TC.4: the encoded picture is inside the per-record bound");
    check(entry.checksum.size() == 64 && entry.renderer == openshow::domain::rendererIdentity(),
          "TC.4: the record carries a checksum and the renderer identity");

    store.request(QStringLiteral("slide17"), QString::fromStdString(fingerprint("17")), 200,
                  112);
    check(waitFor([&] { return ready.size() == 1; }), "TC.4: the demand lane answers");
    check(ready.size() == 1 && ready.front().size() == QSize(200, 112),
          "TC.4: the picture arrives at the requested display extent");
    check(store.hits() == 1 && store.misses() == 0 && missed.isEmpty(),
          "TC.4: a valid record is a hit, not a miss");

    store.request(QStringLiteral("slide404"), QString::fromStdString(fingerprint("404")), 200, 112);
    check(missed.size() == 1 && missed.front().first == QStringLiteral("slide404"),
          "TC.4: an unknown slide is a miss");

    // The coalesced cache-only flush: due after the quiet window, once.
    check(store.dirty(), "TC.4: a new record makes the cache dirty");
    check(waitFor([&] { return flushed > 0; }, 8000), "TC.4: the flush becomes due");
    check(flushed == 1 && flushedCache.entries.size() == 1 && !store.dirty(),
          "TC.4: exactly one cache-only flush carries the record");

    // A refusing hook keeps the entries in memory and retries later.
    store.record(QStringLiteral("show1"), QStringLiteral("slide18"),
                 QString::fromStdString(fingerprint("18")), samplePicture(320, 180, "chorus"));
    check(store.waitForIdle(), "TC.4: the second encode finishes");
    store.setFlushHook([&](const ThumbnailCache&, QString* error) {
        ++flushed;
        *error = QStringLiteral("not ours to write");
        return false;
    });
    check(waitFor([&] { return store.flushFailures() > 0; }, 8000),
          "TC.4: a refused flush is counted");
    check(store.dirty() && store.entryCount() == 2,
          "TC.4: a refused flush keeps every record in memory");

    // Read-only: nothing is written at all.
    ThumbnailStore readOnly;
    readOnly.adopt(store.cache(), QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    int writes = 0;
    readOnly.setFlushHook([&](const ThumbnailCache&, QString*) {
        ++writes;
        return true;
    });
    readOnly.record(QStringLiteral("show1"), QStringLiteral("slide19"),
                    QString::fromStdString(fingerprint("19")), samplePicture(320, 180, "bridge"));
    check(readOnly.waitForIdle(), "TC.4: the read-only encode finishes");
    check(waitFor([&] { return writes > 0; }, 2500) == false && writes == 0,
          "TC.4: a read-only package never flushes");
}

void exerciseFailures() {
    ThumbnailStore store;
    QList<QImage> ready;
    QList<QString> missed;
    QObject::connect(&store, &ThumbnailStore::pictureReady,
                     [&](const QString&, const QString&, const QImage& image) { ready.append(image); });
    QObject::connect(&store, &ThumbnailStore::pictureMissed,
                     [&](const QString& slideId, const QString&) { missed.append(slideId); });

    ThumbnailCache cache;
    ThumbnailCacheEntry corruptChecksum = validEntry("slide1", fingerprint("1"),
                                                     samplePicture(512, 288, "checksum"));
    corruptChecksum.checksum = openshow::domain::sha256Hex("not those bytes");
    cache.entries.push_back(corruptChecksum);

    ThumbnailCacheEntry notDecodable = validEntry("slide2", fingerprint("2"),
                                                  samplePicture(512, 288, "png"));
    // Valid PNG bytes replaced by arbitrary Base64: the header refuses it.
    notDecodable.pngBase64 = openshow::domain::base64Encode(std::string(600, 'x'));
    notDecodable.pngBytes = 600;
    notDecodable.checksum = openshow::domain::pngChecksum(std::string(600, 'x'));
    cache.entries.push_back(notDecodable);

    ThumbnailCacheEntry wrongSize = validEntry("slide3", fingerprint("3"),
                                               samplePicture(640, 360, "size"));
    cache.entries.push_back(wrongSize);

    ThumbnailCacheEntry brokenBase64 = validEntry("slide4", fingerprint("4"),
                                                  samplePicture(512, 288, "base64"));
    brokenBase64.pngBase64 = "%%% not base64 %%%";
    cache.entries.push_back(brokenBase64);

    store.adopt(cache, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    // The unreadable-Base64 record is already refused by the block sanitiser;
    // the other three are structurally valid and fail only when decoded.
    check(store.entryCount() == 3, "TC.4: structurally invalid records are dropped on adopt");

    store.request(QStringLiteral("slide1"), QString::fromStdString(fingerprint("1")), 200, 112);
    store.request(QStringLiteral("slide2"), QString::fromStdString(fingerprint("2")), 200, 112);
    store.request(QStringLiteral("slide3"), QString::fromStdString(fingerprint("3")), 200, 112);
    check(waitFor([&] { return missed.size() >= 3; }), "TC.4: each bad picture answers");
    check(ready.isEmpty() && missed.size() == 3,
          "TC.4: a bad checksum, a non-PNG and a mismatched size are misses, never pictures");
    check(store.failures() == 3, "TC.4: each failure is counted");
    check(missed.count(QStringLiteral("slide3")) == 1,
          "TC.4: a picture whose header disagrees with the record is refused");

    // A record whose Base64 is unreadable is a miss before any job starts.
    store.request(QStringLiteral("slide4"), QString::fromStdString(fingerprint("4")), 200, 112);
    check(missed.size() == 4 && ready.isEmpty(),
          "TC.4: unreadable Base64 is a miss without decoding");
}

void exerciseSupersedingAndGeneration() {
    ThumbnailStore store;
    QList<QImage> ready;
    QObject::connect(&store, &ThumbnailStore::pictureReady,
                     [&](const QString&, const QString&, const QImage& image) { ready.append(image); });
    ThumbnailCache cache;
    cache.entries.push_back(validEntry("slide1", fingerprint("1"), samplePicture(512, 288, "one")));
    store.adopt(cache, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);

    // Two asks for one slide: the newest extent owns the row.
    store.request(QStringLiteral("slide1"), QString::fromStdString(fingerprint("1")), 160, 90);
    store.request(QStringLiteral("slide1"), QString::fromStdString(fingerprint("1")), 300, 169);
    check(store.waitForIdle(), "TC.4: superseding decodes finish");
    check(ready.size() == 1 && ready.front().size() == QSize(300, 169),
          "TC.4: only the newest ask paints, at its own extent");

    // A document swap retires a decode that is already in flight.
    ready.clear();
    store.request(QStringLiteral("slide1"), QString::fromStdString(fingerprint("1")), 240, 135);
    store.adopt(ThumbnailCache{}, QString::fromStdString(openshow::domain::sha256Hex("other-manifest")), false);
    check(store.waitForIdle(), "TC.4: the retired decode finishes");
    check(ready.isEmpty(), "TC.4: a decode from the previous document never paints");
}

void exerciseRetiredEncode() {
    ThumbnailStore store;
    store.adopt(ThumbnailCache{}, QString::fromStdString(openshow::domain::sha256Hex("first")),
                false);
    // Record, then swap the document before the encode lane reports: the PNG
    // describes a document that is no longer open and must never be recorded
    // into the new one's cache.
    store.record(QStringLiteral("show1"), QStringLiteral("slide1"),
                 QString::fromStdString(fingerprint("retired")),
                 samplePicture(320, 180, "retired"));
    store.adopt(ThumbnailCache{}, QString::fromStdString(openshow::domain::sha256Hex("second")),
                false);
    check(store.waitForIdle(), "TC.8: the retired encode finishes");
    check(store.entryCount() == 0,
          "TC.8: an encode finishing after the document changed records nothing");
    check(!store.has(QStringLiteral("slide1"), QString::fromStdString(fingerprint("retired"))),
          "TC.8: the retired picture is not in the new document's cache");
}

void exerciseBoundsAndPruning() {
    ThumbnailStore store;
    store.adopt(ThumbnailCache{}, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    ThumbnailCache big;
    const QImage picture = samplePicture(512, 288, "bounded");
    const ThumbnailCacheEntry base = validEntry("slide0", fingerprint("0"), picture);
    for (std::size_t i = 0; i < openshow::domain::kMaxThumbnailCacheEntries + 20; ++i) {
        ThumbnailCacheEntry entry = base;
        entry.slideId = "slide" + std::to_string(1000 + i);
        entry.fingerprint = openshow::domain::sha256Hex("f" + std::to_string(i));
        big.entries.push_back(entry);
    }
    store.adopt(big, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    check(store.entryCount() == openshow::domain::kMaxThumbnailCacheEntries,
          "TC.4: an oversized block is capped at the record bound");

    // Recording more pictures than the count bound evicts the least recently
    // used and never grows past the ceiling.
    ThumbnailCache small;
    small.entries.push_back(validEntry("slideKeep", fingerprint("keep"), picture));
    store.adopt(small, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    for (int i = 0; i < 6; ++i) {
        store.record(QStringLiteral("show1"), QStringLiteral("slideNew%1").arg(i),
                     QString::fromStdString(fingerprint("new")), picture);
    }
    check(store.waitForIdle(), "TC.4: the eviction encodes finish");
    check(store.entryCount() == 7, "TC.4: records accumulate inside the count ceiling");
    store.setEnabled(false);  // stop the flush timer from outliving the fixture

    // The count ceiling evicts the least recently used record.
    ThumbnailStore full;
    ThumbnailCache overfull;
    for (std::size_t i = 0; i < openshow::domain::kMaxThumbnailCacheEntries; ++i) {
        ThumbnailCacheEntry entry = base;
        entry.slideId = "fullSlide" + std::to_string(i);
        entry.fingerprint = openshow::domain::sha256Hex("full" + std::to_string(i));
        overfull.entries.push_back(entry);
    }
    full.adopt(overfull, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    full.record(QStringLiteral("show1"), QStringLiteral("newest"),
                QString::fromStdString(fingerprint("newest")), picture);
    check(full.waitForIdle(), "TC.4: the ceiling encode finishes");
    check(full.entryCount() == openshow::domain::kMaxThumbnailCacheEntries &&
              full.has(QStringLiteral("newest"), QString::fromStdString(fingerprint("newest"))),
          "TC.4: a new picture evicts the least recently used, not itself");
    full.setEnabled(false);

    // The byte ceiling is enforced by the same pruning rule.
    ThumbnailStore byteStore;
    ThumbnailCache bulky;
    const ThumbnailCacheEntry heavy = [] {
        ThumbnailCacheEntry entry;
        entry.showId = "show1";
        entry.slideId = "slideHeavy";
        entry.fingerprint = fingerprint("heavy");
        entry.renderer = openshow::domain::rendererIdentity();
        entry.width = 512;
        entry.height = 288;
        std::string bytes("\x89PNG\r\n\x1a\n", 8);
        bytes.append(30 * 1024, 'h');
        entry.pngBytes = bytes.size();
        entry.checksum = openshow::domain::pngChecksum(bytes);
        entry.pngBase64 = openshow::domain::base64Encode(bytes);
        return entry;
    }();
    for (std::size_t i = 0; i < openshow::domain::kMaxThumbnailEncodedBytes /
                                    (heavy.pngBase64.size()) + 3; ++i) {
        ThumbnailCacheEntry entry = heavy;
        entry.slideId = "slideHeavy" + std::to_string(i);
        bulky.entries.push_back(entry);
    }
    byteStore.adopt(bulky, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    check(byteStore.encodedBytes() <=
              static_cast<qint64>(openshow::domain::kMaxThumbnailEncodedBytes),
          "TC.4: the encoded-byte ceiling survives an oversized block");

    // Deleting a slide's records (or pruning to the live document) drops them.
    ThumbnailStore pruned;
    ThumbnailCache two;
    two.entries.push_back(validEntry("slideA", fingerprint("a"), picture));
    two.entries.push_back(validEntry("slideB", fingerprint("b"), picture));
    pruned.adopt(two, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    pruned.pruneToSlides({QStringLiteral("slideA")});
    check(pruned.entryCount() == 1 && pruned.has(QStringLiteral("slideA"),
                                                QString::fromStdString(fingerprint("a"))),
          "TC.4: pruning keeps only the live slide's picture");
    pruned.forget(QStringLiteral("slideA"));
    check(pruned.entryCount() == 0, "TC.4: forgetting a slide removes its pictures");
}

void exerciseCanceledAndBoundedJobs() {
    ThumbnailStore store;
    const QString fp = QString::fromStdString(fingerprint("queued"));
    QImage red(160, 90, QImage::Format_ARGB32_Premultiplied);
    red.fill(Qt::red);
    QImage blue(512, 288, QImage::Format_ARGB32_Premultiplied);
    blue.fill(Qt::blue);
    store.record("show1", "slide1", fp, red);
    store.forget("slide1");
    check(store.waitForIdle() && !store.has("slide1", fp),
          "forget cancels an encode already running before its completion lands");
    store.record("show1", "slide1", fp, red);
    store.record("show1", "slide1", fp, blue);
    check(store.waitForIdle(), "superseded encodes finish");
    const auto* latest = store.find("slide1", fp);
    check(latest && latest->width == 512, "the newest encode wins even for the same fingerprint");
    store.record("show1", "slide1", fp, red);
    check(store.waitForIdle() && store.find("slide1", fp)->width == 512,
          "a smaller render cannot downgrade a sharp persisted picture");

    store.adopt({}, "digest", false);
    store.record("show1", "slide1", fp, red);
    check(store.waitForIdle(), "the small picture encodes");
    int misses = 0;
    QObject::connect(&store, &ThumbnailStore::pictureMissed,
                     [&](const QString&, const QString&) { ++misses; });
    store.request("slide1", fp, 512, 288);
    check(misses == 1, "growing a card requests a sharp render instead of upscaling a persisted proxy");
    for (int i = 0; i < 500; ++i) {
        store.record("show1", QString("slide%1").arg(i + 10), fp, blue);
        check(store.pendingEncodes() <= 128 && store.queuedEncodeBytes() <= 8 * 1024 * 1024,
              "encode bursts keep both queued jobs and queued image bytes bounded");
    }
    store.pruneToSlides({});
    check(store.waitForIdle() && store.entryCount() == 0,
          "pruning cancels pending and running encodes, including slides without finished records");

    ThumbnailCache cache;
    for (int i = 0; i < 256; ++i) {
        cache.entries.push_back(validEntry("slide" + std::to_string(i), fp.toStdString(), blue));
    }
    store.adopt(cache, "digest", false);
    for (int i = 0; i < 1000; ++i) {
        store.request(QString("slide%1").arg(i % 256), fp, 160 + i % 32, 90);
        check(store.pendingDecodes() <= 128, "decode bursts have a bounded backlog");
    }
    store.adopt({}, "next-document", false);
    check(store.waitForIdle(), "retired bounded decode work drains without painting");
}

void exerciseDisabled() {
    ThumbnailStore store;
    QList<QString> missed;
    QObject::connect(&store, &ThumbnailStore::pictureMissed,
                     [&](const QString& slideId, const QString&) { missed.append(slideId); });
    ThumbnailCache cache;
    cache.entries.push_back(validEntry("slide1", fingerprint("1"),
                                       samplePicture(512, 288, "off")));
    store.adopt(cache, QString::fromStdString(openshow::domain::sha256Hex("manifest")), false);
    store.setEnabled(false);
    store.request(QStringLiteral("slide1"), QString::fromStdString(fingerprint("1")), 160, 90);
    check(missed.size() == 1 && store.entryCount() == 0,
          "TC.4: a disabled cache answers every lookup as a miss");
    store.record(QStringLiteral("show1"), QStringLiteral("slide1"),
                 QString::fromStdString(fingerprint("1")), samplePicture(160, 90, "off"));
    check(store.waitForIdle() && store.entryCount() == 0,
          "TC.4: a disabled cache records nothing");
}

void exerciseOldSizesRetired() {
    ThumbnailCache cache;
    for (const int width : {181, 198, 204, 320}) {
        auto entry = validEntry("slide" + std::to_string(width), fingerprint("fixed"),
                                samplePicture(width, qRound(width * 9.0 / 16), "cached"));
        entry.width = width;
        entry.height = qRound(width * 9.0 / 16);
        cache.entries.push_back(std::move(entry));
    }
    ThumbnailStore store;
    ThumbnailCache flushed;
    int writes = 0;
    store.setFlushHook([&](const ThumbnailCache& snapshot, QString*) {
        flushed = snapshot;
        ++writes;
        return true;
    });
    store.adopt(cache, "durable-digest", true);
    check(store.entryCount() == 1 && store.cache().entries.front().width == 320,
          "old panel-sized pictures are dropped while the fixed picture is retained");
    check(store.dirty(), "retiring unused sizes schedules their removal from disk");
    check(waitFor([&] { return writes == 1; }, 8000) && flushed.entries.size() == 1 &&
              flushed.entries.front().width == 320 && !store.dirty(),
          "unused sizes are flushed out even without rendering any new pictures");
    store.adoptEntry(cache.entries.front());
    check(store.entryCount() == 1 && !store.dirty(),
          "an old Library variant cannot put a discarded smaller picture back");

    store.adopt({1, {cache.entries.front()}}, "durable-digest", true);
    check(store.entryCount() == 0 && store.dirty(),
          "a cache containing only obsolete sizes becomes an empty dirty snapshot");
    check(waitFor([&] { return writes == 2; }, 8000) && flushed.entries.empty(),
          "the last obsolete picture is removed from disk too");
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    exerciseEncodeDecodeAndFlush();
    exerciseFailures();
    exerciseSupersedingAndGeneration();
    exerciseRetiredEncode();
    exerciseBoundsAndPruning();
    exerciseCanceledAndBoundedJobs();
    exerciseDisabled();
    exerciseOldSizesRetired();
    if (gFailed) {
        return 1;
    }
    std::printf("ok: thumbnail store encode/decode, misses, bounds and flush\n");
    return 0;
}
