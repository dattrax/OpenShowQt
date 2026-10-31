// Async scene/media contracts through the production frontend and worker.
#include <boost/json.hpp>
#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include "WorkerController.h"
#include "openshow/scene/scene.h"

namespace {
using openshow::ui::WorkerController;
namespace scene = openshow::scene;
int failures = 0;
bool check(bool ok, const char* message) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    return ok;
}
bool waitUntil(const std::function<bool()>& predicate, int ms = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}
void settle(int ms) { waitUntil([] { return false; }, ms); }
bool colour(const QImage& image, int channel) {
    if (image.isNull()) return false;
    for (int y = 8; y < image.height(); y += 12)
        for (int x = 8; x < image.width(); x += 12) {
            const auto pixel = image.pixel(x, y);
            const int values[] = {qRed(pixel), qGreen(pixel), qBlue(pixel)};
            for (int c = 0; c < 3; ++c)
                if (c == channel ? values[c] < 85 : values[c] > 35) return false;
        }
    return true;
}
bool black(const QImage& image) {
    if (image.isNull()) return false;
    for (int y = 8; y < image.height(); y += 12)
        for (int x = 8; x < image.width(); x += 12) {
            const auto pixel = image.pixel(x, y);
            if (qRed(pixel) > 12 || qGreen(pixel) > 12 || qBlue(pixel) > 12) return false;
        }
    return true;
}
QString videoScene(const std::string& path, bool words = false) {
    scene::Scene subject;
    subject.showId = "async-integration";
    subject.slideId = path;
    subject.background.hasImage = true;
    subject.background.isVideo = true;
    subject.background.assetId = path;
    subject.background.assetPath = path;
    if (words) {
        scene::SceneItem item;
        item.lines = {"NEXT SCENE"};
        item.fontSize = 200;
        subject.items.push_back(item);
    }
    return QString::fromStdString(boost::json::serialize(scene::toJson(subject)));
}

// 6.23b: the scene store, transport, pixels and frontend must agree for
// A -> B -> C, A -> B -> clear, and latest-wins cancellation back to B.
void deferredScene(const std::string& a, const std::string& b, const std::string& c,
                   int ending) {
    qputenv("OPENSHOW_FAULT_PACING_EXIT_MS", "3500");
    qputenv("OPENSHOW_FAULT_PACING_EXIT_MATCH", "transaction-a.mp4");
    WorkerController worker;
    worker.start();
    if (!check(waitUntil([&] { return worker.outputLive() && worker.previewActive(); }),
               "transaction: worker and preview connect")) { worker.shutdownNow(); return; }
    worker.presentSceneJson(videoScene(a));
    if (!check(waitUntil([&] { return worker.sceneActive() > 0 && colour(worker.previewImage(), 0); }),
               "transaction: A is on air")) { worker.shutdownNow(); return; }
    const auto aRevision = worker.sceneActive();
    worker.presentSceneJson(videoScene(b));
    if (!check(waitUntil([&] {
        return worker.sceneActive() != aRevision && worker.scenePending() == 0
            && worker.mediaPath().toStdString() == b && colour(worker.previewImage(), 1);
    }), "transaction: B is on air while A retires")) { worker.shutdownNow(); return; }
    const auto bRevision = worker.sceneActive();
    const auto committed = worker.revision();
    const auto frames = worker.frames();
    if (ending == 1) worker.clearAll();
    else worker.presentSceneJson(videoScene(c, true));
    if (!check(waitUntil([&] { return worker.scenePending() > 0; }, 1500),
               "transaction: deferred scene remains pending")) { worker.shutdownNow(); return; }
    const auto pending = worker.scenePending();
    const auto start = std::chrono::steady_clock::now();
    bool truthful = true;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(350)) {
        settle(10);
        truthful = truthful && worker.sceneActive() == bRevision && worker.scenePending() == pending
            && worker.revision() == committed && worker.mediaPath().toStdString() == b
            && colour(worker.previewImage(), 1);
    }
    check(truthful, "transaction: pending keeps B's scene, media, revision and green pixels together");
    check(worker.frames() > frames, "transaction: B keeps presenting during retirement deferral");
    if (ending == 2) {
        worker.presentSceneJson(videoScene(b));
        check(waitUntil([&] { return worker.scenePending() == 0 && worker.sceneActive() != bRevision; }),
              "transaction: return to B cancels the deferred C without waiting");
        settle(3600);
        check(worker.mediaPath().toStdString() == b && colour(worker.previewImage(), 1),
              "transaction: cancelled C never starts later");
    } else {
        check(waitUntil([&] { return worker.sceneActive() == pending && worker.scenePending() == 0; }),
              "transaction: deferred scene commits after retirement");
        if (ending == 1) {
            check(waitUntil([&] { return !worker.mediaOnAir() && worker.mediaPath().isEmpty()
                                        && black(worker.previewImage()); }),
                  "transaction: clear's scene, media and black preview land together");
        } else {
            check(waitUntil([&] { return worker.mediaPath().toStdString() == c
                                        && worker.mediaOnAir() && !worker.previewImage().isNull()
                                        && qBlue(worker.previewImage().pixel(8, 8)) > 85
                                        && qGreen(worker.previewImage().pixel(8, 8)) < 35; }),
                  "transaction: C's media and content replace B together");
        }
    }
    worker.shutdownNow();
    qunsetenv("OPENSHOW_FAULT_PACING_EXIT_MS");
    qunsetenv("OPENSHOW_FAULT_PACING_EXIT_MATCH");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2 || std::system("ffmpeg -version >/dev/null 2>&1") != 0) return 77;
    qputenv("OPENSHOW_WORKER_PROGRAM", argv[1]);
    qputenv("OPENSHOW_WORKER_EXTRA_ARGS", "--monitor -1");
    QTemporaryDir scratch;
    if (!scratch.isValid()) return 1;
    const std::string a = scratch.path().toStdString() + "/transaction-a.mp4";
    const std::string b = scratch.path().toStdString() + "/transaction-b.mp4";
    const std::string c = scratch.path().toStdString() + "/transaction-c.mp4";
    int n = 0;
    for (const auto& path : {a, b, c}) {
        const char* colours[] = {"red", "green", "blue"};
        const auto command = std::string("ffmpeg -v error -nostdin -y -f lavfi -i color=c=")
            + colours[n++] + ":s=320x180:r=30 -t 6 -c:v libx264 -pix_fmt yuv420p '" + path + "'";
        if (std::system(command.c_str()) != 0) return 1;
    }
    for (int ending = 0; ending < 3; ++ending) deferredScene(a, b, c, ending);
    return failures ? 1 : 0;
}
