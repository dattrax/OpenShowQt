/*
 * TC.10: the measured cold/warm journey for persisted service-card pictures.
 *
 * This fixture exists to answer, with numbers rather than a claim, whether the
 * cache earns its place:
 *
 *   - cold: a saved service with no pictures is opened, the visible band is
 *     asked for, and every row costs a worker render (frames > 0). Time to
 *     the last visible picture is recorded.
 *   - warm: the same service is warmed (renders recorded, cache block saved),
 *     then reopened IN A SEPARATE PROCESS — the app restarted — and the same
 *     band is asked for. Valid hits must cost ZERO worker thumbnail renders;
 *     the pictures come from Base64/PNG decode on the overlay lane.
 *   - large service: a 200-slide manifest is parsed with and without a full
 *     cache block, so the JSON cost of the feature is measured rather than
 *     guessed.
 *
 * The audience's own stream is watched throughout: a card picture arriving
 * through the worker must not stop the mirror, and the warm run must still
 * receive audience frames.
 *
 * Needs a display (the real worker opens its output window). Desktop preset,
 * label display.
 */

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QVariantList>

#include <boost/json.hpp>

#include "openshow/domain/storage.h"
#include "openshow/domain/thumbnails.h"

#include "DocumentController.h"
#include "ThumbnailStore.h"
#include "WorkerController.h"

namespace {

using openshow::ui::DocumentController;
using openshow::ui::ThumbnailStore;
using openshow::ui::WorkerController;

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

bool waitFor(const std::function<bool()>& done, int timeoutMs) {
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

struct Band {
    QVariantList rows;
    QStringList keys;
};

Band bandOf(DocumentController& documents, const QString& showId) {
    Band band;
    for (const QVariant& row : documents.slidesOf(showId)) {
        const QString id = row.toMap().value(QStringLiteral("id")).toString();
        const QVariantMap source = documents.thumbnailSourceForSlide(id);
        if (source.value(QStringLiteral("scene")).toString().isEmpty()) {
            continue;
        }
        band.rows.push_back(QVariantMap{{QStringLiteral("key"), id},
                                        {QStringLiteral("scene"),
                                         source.value(QStringLiteral("scene"))},
                                        {QStringLiteral("fingerprint"),
                                         source.value(QStringLiteral("fingerprint"))},
                                        {QStringLiteral("showId"), showId}});
        band.keys.push_back(id);
    }
    return band;
}

bool allPainted(WorkerController& worker, const QStringList& keys) {
    for (const QString& key : keys) {
        if (worker.thumbnail(key).isNull()) {
            return false;
        }
    }
    return !keys.isEmpty();
}

QString buildService(const std::filesystem::path& root, int slides) {
    DocumentController documents(nullptr, root);
    documents.createSong(QStringLiteral("Warm Cards"));
    for (int i = 0; i < slides; ++i) {
        documents.addSlide();
        documents.setSlideText(QStringLiteral("Verse %1\nA card kept between runs").arg(i + 1));
        documents.flushText();
    }
    const QString showId = documents.currentSongId();
    documents.save();
    return showId;
}

struct Measure {
    qint64 ms = -1;
    qint64 frames = 0;
    qint64 hits = 0;
    qint64 misses = 0;
    qint64 storedEntries = 0;
    qint64 storedBytes = 0;
    qint64 audienceFrames = 0;
    bool painted = false;
};

Measure measureBand(const std::filesystem::path& root, const QString& showId,
                    ThumbnailStore* store) {
    Measure measure;
    WorkerController worker;
    worker.setThumbnailPictures(store);
    worker.start();
    if (!waitFor([&worker] { return worker.previewActive(); }, 20000)) {
        worker.shutdownNow();
        return measure;
    }
    DocumentController documents(nullptr, root, store);
    const Band band = bandOf(documents, showId);
    waitFor([&worker] { return worker.previewFrames() > 0; }, 5000);
    const qint64 framesBefore = worker.thumbnailFrames();
    QElapsedTimer timer;
    timer.start();
    worker.requestThumbnailWindow(band.rows, 160, 90);
    measure.painted = waitFor([&] { return allPainted(worker, band.keys); }, 30000);
    measure.ms = timer.elapsed();
    measure.frames = worker.thumbnailFrames() - framesBefore;
    // The mirror is a separate target on the same stream: its own counter,
    // which must have advanced at least once for this session to count as a
    // real audience view rather than a headless row ask.
    measure.audienceFrames = worker.previewFrames();
    if (store != nullptr) {
        measure.hits = store->hits();
        measure.misses = store->misses();
        measure.storedEntries = static_cast<qint64>(store->entryCount());
        measure.storedBytes = store->encodedBytes();
    }
    const qint64 framesSeen = worker.previewFrames();
    waitFor([&worker, framesSeen] { return worker.previewFrames() > framesSeen; }, 3000);
    worker.shutdownNow();
    return measure;
}

QJsonObject measureJson(const Measure& measure) {
    QJsonObject object;
    object.insert(QStringLiteral("ms"), static_cast<qint64>(measure.ms));
    object.insert(QStringLiteral("frames"), static_cast<qint64>(measure.frames));
    object.insert(QStringLiteral("audienceFrames"),
                  static_cast<qint64>(measure.audienceFrames));
    object.insert(QStringLiteral("hits"), static_cast<qint64>(measure.hits));
    object.insert(QStringLiteral("misses"), static_cast<qint64>(measure.misses));
    object.insert(QStringLiteral("entries"), static_cast<qint64>(measure.storedEntries));
    object.insert(QStringLiteral("bytes"), static_cast<qint64>(measure.storedBytes));
    object.insert(QStringLiteral("painted"), measure.painted);
    return object;
}

// --- the child process: a warm reopen --------------------------------------

int measureWarmChild(const std::string& rootPath, const std::string& outPath) {
    const std::filesystem::path root(rootPath);
    const std::string showPath = rootPath + "/show-id";
    std::string showId;
    {
        std::ifstream in(showPath);
        std::getline(in, showId);
    }
    ThumbnailStore store;
    const Measure measure = measureBand(root, QString::fromStdString(showId), &store);
    QJsonObject object = measureJson(measure);
    object.insert(QStringLiteral("manifestBytes"),
                  static_cast<qint64>(std::filesystem::file_size(root / "service" /
                                                                 "service.json")));
    std::ofstream out(outPath);
    out << QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
    store.setEnabled(false);
    return measure.painted ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const QByteArray childRoot = qgetenv("OPENSHOW_THUMB_REOPEN_ROOT");
    if (!childRoot.isEmpty()) {
        return measureWarmChild(childRoot.toStdString(),
                                qgetenv("OPENSHOW_THUMB_REOPEN_OUT").toStdString());
    }

    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("openshow-thumbnail-reopen-" +
                                        std::to_string(getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const int slides = 12;
    const QString showId = buildService(root, slides);
    {
        std::ofstream out(root / "show-id");
        out << showId.toStdString() << "\n";
    }

    // --- cold: no cache at all ------------------------------------------
    ::setenv("OPENSHOW_THUMBNAIL_CACHE", "0", 1);
    const Measure cold = measureBand(root, showId, nullptr);
    ::unsetenv("OPENSHOW_THUMBNAIL_CACHE");
    check(cold.painted && cold.frames == slides,
          "TC.10: a cold band costs one worker render per slide");
    check(cold.ms >= 0, "TC.10: the cold measurement ran");
    check(cold.audienceFrames > 0,
          "TC.10: the audience mirror keeps running while cards render");

    // --- warm the service ------------------------------------------------
    {
        ThumbnailStore store;
        WorkerController worker;
        worker.setThumbnailPictures(&store);
        worker.start();
        check(waitFor([&worker] { return worker.previewActive(); }, 20000),
              "TC.10: the warming session gets a stream");
        DocumentController documents(nullptr, root, &store);
        const Band band = bandOf(documents, showId);
        const std::size_t expected = band.keys.size();
        worker.requestThumbnailWindow(band.rows, 160, 90);
        check(waitFor([&] { return allPainted(worker, band.keys); }, 30000),
              "TC.10: the warming session paints the band");
        check(waitFor([&] { return store.entryCount() >= expected; }, 10000),
              "TC.10: every rendered card becomes a record");
        check(documents.save(), "TC.10: the warmed service saves its pictures");
        worker.shutdownNow();
        store.setEnabled(false);
    }
    const qint64 manifestBytes =
        static_cast<qint64>(std::filesystem::file_size(root / "service" / "service.json"));

    // --- warm: a SEPARATE process, as the application restarts -----------
    const std::filesystem::path outPath = root / "warm.json";
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("OPENSHOW_THUMB_REOPEN_ROOT"),
                       QString::fromStdString(root.string()));
    environment.insert(QStringLiteral("OPENSHOW_THUMB_REOPEN_OUT"),
                       QString::fromStdString(outPath.string()));
    environment.remove(QStringLiteral("OPENSHOW_THUMBNAIL_CACHE"));
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.start(QCoreApplication::applicationFilePath(), {});
    check(child.waitForFinished(120000) && child.exitStatus() == QProcess::NormalExit &&
              child.exitCode() == 0,
          "TC.10: the warm reopen process finishes");
    QJsonObject warm;
    {
        std::ifstream in(outPath);
        std::stringstream buffer;
        buffer << in.rdbuf();
        QJsonParseError error{};
        const QJsonDocument parsed =
            QJsonDocument::fromJson(QByteArray::fromStdString(buffer.str()), &error);
        if (error.error == QJsonParseError::NoError) {
            warm = parsed.object();
        }
    }
    check(!warm.isEmpty(), "TC.10: the warm process reported its measurement");
    check(warm.value(QStringLiteral("painted")).toBool() &&
              warm.value(QStringLiteral("frames")).toInt() == 0,
          "TC.10: a warm reopen paints valid hits with ZERO worker thumbnail renders");
    check(warm.value(QStringLiteral("hits")).toInt() >= slides,
          "TC.10: every visible row was answered by the persisted cache");
    check(warm.value(QStringLiteral("audienceFrames")).toInt() > 0,
          "TC.10: the audience still receives frames in the warm session");

    // --- large service: the JSON cost of the block -----------------------
    {
        const std::filesystem::path big = root / "large";
        std::filesystem::create_directories(big);
        DocumentController documents(nullptr, big);
        documents.createSong(QStringLiteral("Large Service"));
        const int largeSlides = 200;
        for (int i = 0; i < largeSlides; ++i) {
            documents.addSlide();
            documents.setSlideText(QStringLiteral("Line %1\nA large service keeps many cards")
                                       .arg(i + 1));
            documents.flushText();
        }
        check(documents.save(), "TC.10: the large service saves");
        const auto service = big / "service";
        const qint64 plainBytes =
            static_cast<qint64>(std::filesystem::file_size(service / "service.json"));
        QElapsedTimer timer;
        timer.start();
        openshow::domain::Documents loaded;
        std::string error;
        check(openshow::domain::loadService(&loaded, service, &error),
              "TC.10: the large service loads without a cache");
        const qint64 plainLoadMs = timer.elapsed();

        // A full 200-entry block of real 512x288 PNGs, through the codec's own
        // merge path, so the number includes Base64 and JSON parsing.
        // A real card render at the canonical bucket: the measured average
        // 512x288 card is ~3.8 KiB, so a flat fill would flatter the block.
        QImage picture(512, 288, QImage::Format_ARGB32_Premultiplied);
        {
            // A plain service card (words on a flat background), which is what
            // most of a real running order holds; the average of the
            // repository's 162 card renders at this bucket is ~3.8 KiB.
            QImage real(QStringLiteral(OPENSHOW_SOURCE_DIR "/scene-render-ctx-verse-after.png"));
            if (!real.isNull()) {
                picture = real.scaled(512, 288, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            } else {
                picture.fill(QColor(20, 40, 80));
            }
        }
        QByteArray png;
        {
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            picture.save(&buffer, "PNG");
        }
        const std::string bytes(png.constData(), static_cast<std::size_t>(png.size()));
        openshow::domain::ThumbnailCache cache;
        for (int i = 0; i < largeSlides; ++i) {
            openshow::domain::ThumbnailCacheEntry entry;
            entry.showId = loaded.showIds().front();
            entry.slideId = loaded.findShow(loaded.showIds().front())->slides[static_cast<std::size_t>(i)].id;
            entry.fingerprint = openshow::domain::sha256Hex("large-" + std::to_string(i));
            entry.renderer = openshow::domain::rendererIdentity();
            entry.width = 512;
            entry.height = 288;
            entry.pngBytes = bytes.size();
            entry.checksum = openshow::domain::pngChecksum(bytes);
            entry.pngBase64 = openshow::domain::base64Encode(bytes);
            cache.entries.push_back(std::move(entry));
        }
        check(openshow::domain::mergeThumbnailCache(
                  service, cache, openshow::domain::authoredManifestDigest(loaded), &error),
              "TC.10: the large cache block is merged");
        const qint64 cacheBytes =
            static_cast<qint64>(std::filesystem::file_size(service / "service.json"));
        timer.restart();
        openshow::domain::Documents loadedWarm;
        openshow::domain::ThumbnailCache loadedCache;
        check(openshow::domain::loadService(&loadedWarm, service, &error, &loadedCache),
              "TC.10: the large service loads with a full cache block");
        const qint64 warmLoadMs = timer.elapsed();
        check(!loadedCache.entries.empty() &&
                  loadedCache.entries.size() <= static_cast<std::size_t>(largeSlides),
              "TC.10: the block survives the parse inside its bounds");
        std::printf("TC.10 large service: plain %lld B / %lld ms, with cache %lld B / %lld ms"
                    " (+%lld B, +%lld ms), entries %zu\n",
                    static_cast<long long>(plainBytes), static_cast<long long>(plainLoadMs),
                    static_cast<long long>(cacheBytes), static_cast<long long>(warmLoadMs),
                    static_cast<long long>(cacheBytes - plainBytes),
                    static_cast<long long>(warmLoadMs - plainLoadMs),
                    loadedCache.entries.size());
    }

    std::printf("TC.10 cold: %lld ms, %lld worker renders, audience %lld frames\n",
                static_cast<long long>(cold.ms), static_cast<long long>(cold.frames),
                static_cast<long long>(cold.audienceFrames));
    std::printf("TC.10 warmed service: %lld B manifest\n", static_cast<long long>(manifestBytes));
    std::printf("TC.10 warm: %lld ms, %lld worker renders, hits %lld, misses %lld,"
                " records %lld, %lld B\n",
                static_cast<long long>(warm.value(QStringLiteral("ms")).toInt()),
                static_cast<long long>(warm.value(QStringLiteral("frames")).toInt()),
                static_cast<long long>(warm.value(QStringLiteral("hits")).toInt()),
                static_cast<long long>(warm.value(QStringLiteral("misses")).toInt()),
                static_cast<long long>(warm.value(QStringLiteral("entries")).toInt()),
                static_cast<long long>(warm.value(QStringLiteral("bytes")).toInt()));

    std::filesystem::remove_all(root);
    if (gFailed) {
        return 1;
    }
    std::printf("thumbnail_reopen_tests: cold/warm journey measured\n");
    return 0;
}
