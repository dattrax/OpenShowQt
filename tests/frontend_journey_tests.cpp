/*
 * Task 6.21g (review F7): the operator's night through the REAL frontend.
 *
 * production_journey_tests proved the same night over a hand-written socket:
 * the review's honest objection was that go, clear and restore there bypass
 * the classes that actually make those decisions — and that its restore
 * re-presented a remembered worker revision, where the product re-prepares
 * retained content. That fixture stays exactly as it is, covering the wire.
 * This one drives the production objects — WorkerController supervising a
 * real worker, PresentationModel's latest-go/clear/restore logic, the
 * PreviewConsumer's own copied frame — and asserts the same double proof the
 * review asked for: the acknowledged status flags AND the pixels of the
 * frame the operator's PreviewView would paint.
 *
 * What has to be true for F7 to stay closed, in order:
 *   1. a go through the controller paints the whole authored stack — a gold
 *      cover background, a foreground picture in its authored box, white words;
 *   2. a gradient slide goes on air through the controller with its ramp
 *      sampled at ceiling and floor;
 *   3. per-layer clears compose against the picture — background first (the
 *      gold goes, the picture and words ride through), then the words clear
 *      takes the rest to uniform black;
 *   4. the restore comes back AFTER the worker's 16-deep store has pruned the
 *      revision the clear cut from — 18 plain scenes intervene, so presenting
 *      a remembered revision is impossible and re-preparing retained content
 *      is the only path back to these pixels;
 *   5. a save that MOVES both asset files (background plate and foreground
 *      picture) is adopted: exactly one re-prepare, no present, the audience
 *      unchanged — and the next clear→restore cycle draws both pictures from
 *      the NEW paths, the old names gone from the filesystem, with no
 *      foreground fault reported;
 *   6. a foreground picture whose file is missing names itself in
 *      foregroundErrors (path + reason from the real worker's decode) without
 *      failing the go, and writing the file back and going again repairs the
 *      notice and draws the box, with the failure event still counted.
 *   7. the cue lane (UI.3b): a tagged go is pending synchronously and LIVE
 *      only once the worker names its revision active — the LIVE cue owns
 *      the pixels on the preview stream — and a go that never reaches the
 *      worker marks nothing.
 *
 * Needs a display (the worker opens its output window) and ffmpeg (the plates
 * are real PNGs through the real decode path). Desktop preset, label display.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include <unistd.h>

#include <QCoreApplication>
#include <QImage>
#include <QVariantMap>

#include "openshow/scene/scene.h"

#include "WorkerController.h"

using openshow::ui::WorkerController;

namespace {

namespace scene = openshow::scene;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

// Spin the Qt event loop until the predicate holds. Everything this fixture
// observes — status polls, preview frames, process signals — arrives through
// the event loop, so waiting MEANS processing.
bool waitUntil(const std::function<bool()>& predicate, int budgetMs = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

void settle(int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Real PNGs through ffmpeg, the same discipline as the raw-protocol journey:
// the worker decodes a file any image tool could have made, never something
// only this test knows how to read.
bool writePng(const std::string& path, const char* colour, int width, int height) {
    const std::string command =
        "ffmpeg -v error -y -f lavfi -i color=c=" + std::string(colour) + ":s=" +
        std::to_string(width) + "x" + std::to_string(height) + ":r=1 -frames:v 1 '" + path +
        "' >/dev/null 2>&1";
    return std::system(command.c_str()) == 0 && ::access(path.c_str(), R_OK) == 0;
}

/* --- the audience, as the frontend itself sees it ---------------------------
   previewImage() is the frame PreviewConsumer copied out of shared memory —
   the same QImage PreviewView paints. The stream is 640x360 of the full
   16:9 canvas, so authored coordinates divide out. ------------------------- */

uint32_t rgbAt(const QImage& image, double u, double v) {
    if (image.isNull()) {
        return 0xFFFFFFFFu;  // a missing frame matches nothing real; white fails checks
    }
    const int x = std::clamp(int(u * image.width()), 0, image.width() - 1);
    const int y = std::clamp(int(v * image.height()), 0, image.height() - 1);
    const QRgb pixel = image.pixel(x, y);
    return (uint32_t(qRed(pixel)) << 16) | (uint32_t(qGreen(pixel)) << 8) | uint32_t(qBlue(pixel));
}

bool nearColor(uint32_t rgb, uint32_t want, int tolerance = 8) {
    const int dr = int((rgb >> 16) & 0xFF) - int((want >> 16) & 0xFF);
    const int dg = int((rgb >> 8) & 0xFF) - int((want >> 8) & 0xFF);
    const int db = int(rgb & 0xFF) - int(want & 0xFF);
    return dr * dr + dg * dg + db * db <= tolerance * tolerance * 3;
}

// Words are white items on colours here that are not: gold, magenta, dark
// green and black all fail at least one channel of "all three > 200".
int whitePixels(const QImage& image) {
    if (image.isNull()) {
        return 0;
    }
    int white = 0;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) > 200 && qGreen(pixel) > 200 && qBlue(pixel) > 200) {
                ++white;
            }
        }
    }
    return white;
}

bool uniformlyBlack(const QImage& image, int tolerance = 10) {
    if (image.isNull()) {
        return false;
    }
    for (double u : {0.02, 0.25, 0.5, 0.75, 0.98}) {
        for (double v : {0.02, 0.25, 0.5, 0.75, 0.98}) {
            const uint32_t rgb = rgbAt(image, u, v);
            if (((rgb >> 16) & 0xFF) > tolerance || ((rgb >> 8) & 0xFF) > tolerance ||
                (rgb & 0xFF) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

// A go through the controller lands on the acknowledged revision first and
// rides an 800 ms fade to its final picture second. Wait for the predicate,
// then let the fade finish and demand the predicate of the FINAL frame — a
// blend that merely passed through the colour is not evidence.
bool pixelsSettled(const WorkerController& controller,
                   const std::function<bool(const QImage&)>& predicate, int budgetMs = 6000) {
    if (!waitUntil([&] { return predicate(controller.previewImage()); }, budgetMs)) {
        return false;
    }
    settle(900);
    return predicate(controller.previewImage());
}

/* --- the payloads ---------------------------------------------------------- */

QString serialized(const scene::Scene& subject) {
    return QString::fromStdString(boost::json::serialize(scene::toJson(subject)));
}

scene::SceneItem pictureItem(const std::string& path) {
    scene::SceneItem item;
    item.isImage = true;
    item.assetId = "plate-fg";
    item.assetPath = path;
    item.x = 140.0;
    item.y = 760.0;
    item.width = 460.0;
    item.height = 250.0;
    return item;
}

scene::SceneItem textItem(const char* word) {
    scene::SceneItem item;
    item.lines = {word};
    item.color = 0xFFFFFFu;
    item.fontSize = 160.0;
    return item;
}

// The full stack: a gold cover plate, a magenta picture low-left, white words
// centred. Every pixel assertion in this file has exactly one of those as
// its subject.
QString sceneAlpha(const std::string& backgroundPath, const std::string& foregroundPath) {
    scene::Scene subject;
    subject.showId = "fe-journey";
    subject.slideId = "alpha";
    subject.background.color = 0x101030u;
    subject.background.hasImage = true;
    subject.background.assetId = "plate-bg";
    subject.background.assetPath = backgroundPath;
    subject.items = {pictureItem(foregroundPath), textItem("ALPHA")};
    return serialized(subject);
}

// Black ceiling, amber floor: the payload whose TOP colour reads as "no
// background" — the F4 shape, seen through this controller for the first time.
QString sceneGraded() {
    scene::Scene subject;
    subject.showId = "fe-journey";
    subject.slideId = "graded";
    subject.background.color = scene::kBlack;
    subject.background.gradient = 0xFF8000u;
    subject.background.hasGradient = true;
    subject.items = {textItem("GRADED")};
    return serialized(subject);
}

// Filler for the store-pruning stretch: plain words over near-black, a colour
// no assertion here mistakes for anything else.
QString scenePlain(int index) {
    scene::Scene subject;
    subject.showId = "fe-journey";
    subject.slideId = "filler" + std::to_string(index);
    subject.background.color = 0x050505u;
    subject.items = {textItem("FILLER")};
    return serialized(subject);
}

// The stack with a picture whose file does not exist (yet): words and a dark
// green field are the honest scene, the box is the promise that fails.
QString sceneFaulty(const std::string& missingPath) {
    scene::Scene subject;
    subject.showId = "fe-journey";
    subject.slideId = "faulty";
    subject.background.color = 0x0A260Au;
    subject.items = {pictureItem(missingPath), textItem("OMEGA")};
    return serialized(subject);
}

// Sampling points, authored canvas -> stream fractions.
constexpr double kBgU = 0.965, kBgV = 0.045;      // top-right: the cover plate alone
constexpr double kPicU = 0.193, kPicV = 0.815;    // centre of the magenta box
constexpr uint32_t kGold = 0xF0E030;
constexpr uint32_t kMagenta = 0xDC28C8;
constexpr uint32_t kAmber = 0xFF8000;

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const std::string workerBinary = argc > 1 ? argv[1] : "./openshow-worker";
    if (!std::filesystem::exists(workerBinary)) {
        std::fprintf(stderr, "frontend_journey_tests: no worker at '%s'\n", workerBinary.c_str());
        return 1;
    }

    const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                          ("openshow-fe-journey-" + std::to_string(getpid()));
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    const std::string goldPath = (scratch / "plate-gold.png").string();
    const std::string magentaPath = (scratch / "plate-magenta.png").string();
    if (!writePng(goldPath, "0xF0E030", 800, 450) ||
        !writePng(magentaPath, "0xDC28C8", 460, 250)) {
        std::printf("frontend_journey_tests: ffmpeg unavailable — the plates cannot be authored, "
                    "skipping (77)\n");
        std::filesystem::remove_all(scratch);
        return 77;
    }

    // The controller supervises the REAL worker: same QProcess path the shell
    // uses, same socket handshake, same status polls.
    ::setenv("OPENSHOW_WORKER_PROGRAM", workerBinary.c_str(), 1);
    WorkerController controller;
    controller.start();
    if (!waitUntil([&controller] {
            return controller.statusText().contains(QStringLiteral("connected to worker"));
        },
                   20000)) {
        std::fprintf(stderr, "frontend_journey_tests: no worker connection (%s) — a display is "
                             "required\n",
                     controller.statusText().toUtf8().constData());
        return 1;
    }
    if (!waitUntil([&controller] {
            return controller.previewActive() && !controller.previewImage().isNull();
        })) {
        std::fprintf(stderr, "FAIL: the preview stream never produced a frame\n");
        controller.shutdownNow();
        return 1;
    }

    // --- 1. the go paints the whole authored stack ---------------------------
    controller.presentSceneJson(sceneAlpha(goldPath, magentaPath));
    if (!check(waitUntil([&controller] {
                   return controller.sceneActive() >= 1 && controller.wordsOnAir() &&
                          controller.backgroundOnAir() && controller.scenePending() == 0;
               }),
               "alpha: the go is acknowledged with both layers on air")) {
        controller.shutdownNow();
        return 1;
    }
    check(pixelsSettled(controller, [](const QImage& frame) {
              return nearColor(rgbAt(frame, kBgU, kBgV), kGold) &&
                     nearColor(rgbAt(frame, kPicU, kPicV), kMagenta) && whitePixels(frame) > 20;
          }),
          "alpha: cover plate, foreground picture and words all reach the operator's frame");

    // --- 2. the gradient through the real controller -------------------------
    controller.presentSceneJson(sceneGraded());
    check(waitUntil([&controller] {
              return controller.wordsOnAir() && controller.backgroundOnAir() &&
                     controller.scenePending() == 0;
          }),
          "graded: the black-to-colour scene is acknowledged (the gradient counts as a "
          "background)");
    check(pixelsSettled(controller, [](const QImage& frame) {
              const uint32_t ceiling = rgbAt(frame, 0.02, 0.03);
              const uint32_t floor = rgbAt(frame, 0.02, 0.97);
              return nearColor(ceiling, 0x000000, 14) && nearColor(floor, kAmber, 28);
          }),
          "graded: the audience samples black at the ceiling and amber at the floor");

    // --- 3. per-layer clears compose against the picture ----------------------
    controller.presentSceneJson(sceneAlpha(goldPath, magentaPath));
    if (!check(pixelsSettled(controller, [](const QImage& frame) {
                   return nearColor(rgbAt(frame, kBgU, kBgV), kGold) &&
                          nearColor(rgbAt(frame, kPicU, kPicV), kMagenta);
               }),
               "alpha returns on air before the clears")) {
        controller.shutdownNow();
        return 1;
    }
    controller.clearBackground();
    check(waitUntil([&controller] {
              return !controller.backgroundOnAir() && controller.wordsOnAir() &&
                     controller.scenePending() == 0;
          }),
          "clears: the background clear is acknowledged with the words still on air");
    check(pixelsSettled(controller, [](const QImage& frame) {
              return !nearColor(rgbAt(frame, kBgU, kBgV), kGold) &&
                     nearColor(rgbAt(frame, kPicU, kPicV), kMagenta) && whitePixels(frame) > 20;
          }),
          "clears: the plate is gone from the audience and the picture and words rode through");
    controller.clearSlide();
    check(waitUntil([&controller] {
              return !controller.wordsOnAir() && !controller.backgroundOnAir() &&
                     controller.scenePending() == 0;
          }),
          "clears: the composed clear leaves nothing acknowledged");
    check(pixelsSettled(controller, [](const QImage& frame) { return uniformlyBlack(frame); }),
          "clears: the composed clear reaches uniform black");
    check(controller.canRestore(), "clears: restore is offered against the black");

    // --- 4. the restore survives the store pruning ---------------------------
    // 18 BARE prepares on top (they store without presenting — a fresh go
    // would honestly retire the restore snapshot): the worker's store is 16
    // deep and exempts only what is desired or landed, so the revision this
    // clear sequence cut from is gone by the time restoreOutput runs. The
    // worker's revision arithmetic then proves the whole stretch landed —
    // restore re-prepare mints exactly one id past the 18th filler, and a
    // remembered revision could only fail here (task 6.3b, against the real
    // store).
    for (int i = 0; i < 18; ++i) {
        controller.prepareScene(scenePlain(i));
    }
    check(waitUntil([&controller] { return controller.scenePrepared() == 16 &&
                                           controller.scenePending() == 0; }),
          "prune: the filler prepares filled the 16-deep store");
    settle(800);  // the last filler acks; the socket is drained
    check(controller.canRestore(), "prune: the retained restore content is still available");
    const auto activeBeforeRestore = controller.sceneActive();
    controller.restoreOutput();
    check(waitUntil([&controller, activeBeforeRestore] {
              return controller.sceneActive() == activeBeforeRestore + 19 &&
                     controller.wordsOnAir() && controller.backgroundOnAir() &&
                     controller.scenePending() == 0;
          },
                    12000),
          "restore: the pruned content is re-prepared (one id past the 18 fillers) and "
          "acknowledged on air");
    check(pixelsSettled(controller, [](const QImage& frame) {
              return nearColor(rgbAt(frame, kBgU, kBgV), kGold) &&
                     nearColor(rgbAt(frame, kPicU, kPicV), kMagenta) && whitePixels(frame) > 20;
          }),
          "restore: the full stack — plate, picture, words — is the audience again");
    waitUntil([&controller] { return !controller.canRestore(); }, 4000);

    // --- 5. a save MOVES the files; the frontend adopts ----------------------
    // Exactly what DocumentController does after a recover-and-save: the two
    // path-bearing files reappear under package-owned names and the originals
    // are gone. Adoption must re-prepare the owed scene once, present nothing,
    // and hand the rewritten payload to the NEXT clear/restore cycle.
    const std::string adoptedGold = (scratch / "package-gold.png").string();
    const std::string adoptedMagenta = (scratch / "package-magenta.png").string();
    std::filesystem::rename(goldPath, adoptedGold);
    std::filesystem::rename(magentaPath, adoptedMagenta);
    const auto activeBeforeAdopt = controller.sceneActive();
    controller.adoptAssetPaths(QVariantMap{{QString::fromStdString(goldPath),
                                            QString::fromStdString(adoptedGold)},
                                           {QString::fromStdString(magentaPath),
                                            QString::fromStdString(adoptedMagenta)}});
    settle(800);  // the adoption's re-prepare round-trip, and a status poll on top
    check(controller.sceneActive() == activeBeforeAdopt && controller.scenePending() == 0,
          "adopt: adoption re-prepares the owed scene WITHOUT presenting — the audience is "
          "not moved");
    settle(600);
    check(nearColor(rgbAt(controller.previewImage(), kBgU, kBgV), kGold) &&
              nearColor(rgbAt(controller.previewImage(), kPicU, kPicV), kMagenta),
          "adopt: the audience picture is unchanged across the adoption");
    controller.clearAll();
    check(waitUntil([&controller] {
              return !controller.wordsOnAir() && !controller.backgroundOnAir() &&
                     controller.scenePending() == 0;
          }),
          "adopt: clear-all over the adopted payload is acknowledged");
    check(pixelsSettled(controller, [](const QImage& frame) { return uniformlyBlack(frame); }),
          "adopt: the audience is black");
    controller.restoreOutput();
    check(waitUntil([&controller] {
              return controller.wordsOnAir() && controller.backgroundOnAir() &&
                     controller.scenePending() == 0 && controller.foregroundErrors().isEmpty();
          }),
          "adopt: the restore re-prepares the ADOPTED payload with nothing faulted");
    check(pixelsSettled(controller, [](const QImage& frame) {
              return nearColor(rgbAt(frame, kBgU, kBgV), kGold) &&
                     nearColor(rgbAt(frame, kPicU, kPicV), kMagenta) && whitePixels(frame) > 20;
          }),
          "adopt: both pictures draw from the new paths — the old names are gone from disk");

    // --- 6. the missing picture reports itself, and the repair repairs it ----
    const std::string missingPath = (scratch / "not-yet.png").string();
    controller.presentSceneJson(sceneFaulty(missingPath));
    const bool faulted = waitUntil([&controller, &missingPath] {
        const QVariantList faults = controller.foregroundErrors();
        return faults.size() == 1 &&
               faults.first().toMap().value(QStringLiteral("path"))
                       .toString()
                       .toStdString() == missingPath &&
               !faults.first().toMap().value(QStringLiteral("error")).toString().isEmpty() &&
               controller.scenePending() == 0;
    });
    check(faulted,
          "fault: the real worker's per-item failure reaches foregroundErrors with path+reason");
    check(controller.wordsOnAir() && controller.backgroundOnAir(),
          "fault: a missing picture is not a failed go — the scene is on air");
    const QString faultReason =
        faulted
            ? controller.foregroundErrors().first().toMap().value(QStringLiteral("error"))
                  .toString()
            : QStringLiteral("(none)");
    check(pixelsSettled(controller, [](const QImage& frame) {
              return !nearColor(rgbAt(frame, kPicU, kPicV), kMagenta, 60) &&
                     whitePixels(frame) > 20;
          }),
          "fault: the audience shows the field and the words where the box did not draw");
    // The repair: the file comes back and the operator goes again. The notice
    // follows the worker's next decode, and the event count — monotonic — does
    // not un-happen.
    const auto eventsAfterFault = controller.foregroundFailureEvents();
    if (!check(writePng(missingPath, "0xDC28C8", 460, 250),
               "fault: the missing file is written back")) {
        controller.shutdownNow();
        return 1;
    }
    controller.presentSceneJson(sceneFaulty(missingPath));
    check(waitUntil([&controller] {
              return controller.foregroundErrors().isEmpty() && controller.scenePending() == 0;
          }),
          "repair: the notice clears when the acknowledged prepare decodes the path");
    check(controller.foregroundFailureEvents() >= eventsAfterFault && eventsAfterFault >= 1,
          "repair: the failure event stays counted — the count never un-happens");
    check(pixelsSettled(controller, [](const QImage& frame) {
              return nearColor(rgbAt(frame, kPicU, kPicV), kMagenta) && whitePixels(frame) > 20;
          }),
          "repair: the picture draws where the fault line used to be");

    // --- 7. cue markers are acknowledgement-backed (task UI.3b) --------------
    // The Show page lights a cue LIVE only because THIS worker said the
    // revision that cue's go went out for is active. Ask for a cue: it is
    // PENDING the moment the go leaves, and nothing is LIVE yet.
    controller.presentCueJson(QStringLiteral("cue-a"), sceneAlpha(goldPath, magentaPath));
    check(controller.pendingCueKey() == QStringLiteral("cue-a") &&
              controller.liveCueKey().isEmpty(),
          "cue: the go is pending before any acknowledgement");
    if (!check(waitUntil([&controller] {
                   return controller.liveCueKey() == QStringLiteral("cue-a") &&
                          controller.pendingCueKey().isEmpty() && controller.scenePending() == 0;
               }),
               "cue: the acknowledgement turns cue-a live")) {
        controller.shutdownNow();
        return 1;
    }
    // The LIVE claim means the audience sees THAT cue's picture — the same
    // gold-and-magenta stack cue-a carried, read off the preview stream.
    check(pixelsSettled(controller, [](const QImage& frame) {
              return nearColor(rgbAt(frame, kBgU, kBgV), kGold) &&
                     nearColor(rgbAt(frame, kPicU, kPicV), kMagenta) && whitePixels(frame) > 20;
          }),
          "cue: the live marker names the cue whose pixels are actually on air");
    // Going to a second cue: mid-go the old cue keeps the air claim and the
    // new one waits; the ack moves the claim, in order.
    controller.presentCueJson(QStringLiteral("cue-b"), sceneGraded());
    check(controller.pendingCueKey() == QStringLiteral("cue-b") &&
              controller.liveCueKey() == QStringLiteral("cue-a"),
          "cue: mid-go the previous cue still claims the air and the new one is pending");
    check(waitUntil([&controller] {
              return controller.liveCueKey() == QStringLiteral("cue-b") &&
                     controller.pendingCueKey().isEmpty();
          }),
          "cue: the second acknowledgement moves the live marker cue-a -> cue-b");
    // A scene the controller cannot even send asks nothing of the worker, so
    // no cue may claim pending for it — and the settled markers survive the
    // polls that follow.
    controller.presentCueJson(QStringLiteral("cue-c"), QStringLiteral("{not a scene"));
    check(controller.pendingCueKey().isEmpty() &&
              controller.liveCueKey() == QStringLiteral("cue-b"),
          "cue: an unreadable scene marks no cue pending");
    settle(300);
    check(controller.pendingCueKey().isEmpty() &&
              controller.liveCueKey() == QStringLiteral("cue-b"),
          "cue: the refused go leaves the settled markers unchanged");

    std::printf("frontend_journey: go/gradient/composed clears/restore across a 16-deep prune, "
                "adoption of both plates with restore from the new paths, a real missing-"
                "picture fault (%s) repaired, and a cue lane whose live marker only lights for "
                "the cue the worker acknowledged — all in status AND in the copied frame\n",
                faultReason.toUtf8().constData());

    controller.shutdownNow();
    std::filesystem::remove_all(scratch);
    if (gFailures != 0) {
        std::fprintf(stderr, "frontend_journey_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("frontend_journey_tests: the real controller's night is proven end to end\n");
    return 0;
}
