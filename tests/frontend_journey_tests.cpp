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
 *   8. the media bin's cue (UI.4b): a real probe accepts a file and the
 *      production track reports it (with its own clock), a file it cannot use
 *      is refused naming that file while the live track carries on, a go moves
 *      none of it, and the removal is the stop.
 *   9. the theme chain (TH.3): a domain-compiled slide, the service's current
 *      theme over it, and an explicit-flat customization all reach the
 *      operator's frame as acknowledgement-backed re-skins.
 *  10. the operator's transition numbers (TR.3): a service default and a
 *      view's override cross the audience's ground for the milliseconds they
 *      name, a Cut crosses nothing, an edit cannot shorten the fade already
 *      crossing, an off-air theme moves nobody, an explicit clear is a Cut
 *      while a restore keeps the record its own snapshot carried, and the
 *      measured numbers are the stored ones.
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
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include <unistd.h>

#include <QCoreApplication>
#include <QImage>
#include <QVariantMap>

#include "openshow/domain/documents.h"
#include "openshow/scene/scene.h"

#include "DocumentController.h"
#include "WorkerController.h"

using openshow::ui::DocumentController;
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

// Yellow (#ffe000) words over dark grounds: a grid count in the middle band,
// where a line of the chain test's 72 px text can land. (Task TH.3's chain
// pixels are the first coloured words this fixture paints.)
int yellowPixels(const QImage& image) {
    if (image.isNull()) {
        return 0;
    }
    int yellow = 0;
    for (int y = image.height() / 5; y < image.height() * 4 / 5; y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) > 190 && qGreen(pixel) > 150 && qBlue(pixel) < 110) {
                ++yellow;
            }
        }
    }
    return yellow;
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

    // --- 8. the media bin's cue, validated by a real worker (task UI.4b) ----
    // The bin's Play button is WorkerController::selectStandaloneTrack and
    // nothing else, so what a row may claim is exactly what this seam answers.
    // In order: a decodable file becomes the production track and the status
    // block says so; a file the probe cannot use is REFUSED, with the worker's
    // own reason recorded against the path that ask named (the only way the
    // bin can mark the right row "cannot play"); the track already on air is
    // untouched by that failure; a slide change moves none of it; and the
    // removal is the stop.
    const std::string bedPath = (scratch / "bed.m4a").string();
    const std::string corruptPath = (scratch / "not-audio.m4a").string();
    {
        const std::string command =
            "ffmpeg -v error -y -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000"
            " -t 2 -c:a aac '" + bedPath + "' >/dev/null 2>&1";
        if (std::system(command.c_str()) != 0) {
            std::fprintf(stderr, "FAIL: the audio bed could not be authored\n");
            controller.shutdownNow();
            return 1;
        }
        std::ofstream corrupt(corruptPath);
        corrupt << "these are not the audio bytes you are looking for";
    }
    const QString bed = QString::fromStdString(bedPath);
    controller.selectStandaloneTrack(bed);
    if (!check(waitUntil([&controller, &bed] {
                   return controller.standaloneOnAir() && controller.standalonePath() == bed;
               }),
               "media cue: the accepted file is confirmed as the production track")) {
        controller.shutdownNow();
        return 1;
    }
    check(controller.standaloneRefusal().isEmpty() && controller.standaloneRefusalPath().isEmpty(),
          "media cue: an accepted selection carries no verdict at all");
    check(waitUntil([&controller] { return controller.standalonePosition() >= 0.0; }),
          "media cue: the track reports the clock the audience is hearing");
    check(controller.liveCueKey() == QStringLiteral("cue-b") &&
              controller.pendingCueKey().isEmpty(),
          "media cue: activating audio presents nothing and moves no cue marker");
    // The probe's bad news, on a file that exists and is not audio: the ask
    // comes back with the worker's reason naming it, and the bed keeps playing
    // exactly as it was — which is what lets the bin mark one row and leave the
    // live one alone.
    const QString corruptFile = QString::fromStdString(corruptPath);
    controller.selectStandaloneTrack(corruptFile);
    const bool refused = waitUntil([&controller, &corruptFile] {
        return controller.standaloneRefusalPath() == corruptFile &&
               !controller.standaloneRefusal().isEmpty();
    });
    check(refused, "media cue: a file the real probe cannot use is refused with a reason");
    check(refused && controller.standaloneRefusal().contains(corruptFile),
          "media cue: the refusal names the file the worker turned down");
    check(controller.standaloneOnAir() && controller.standalonePath() == bed,
          "media cue: the refused selection leaves the track on air untouched");
    // A go, and the bed is still the bed: this track has a life of its own,
    // which is the whole reason a bin cue can be more than a slide's accessory.
    const auto revisionBefore = controller.sceneActive();
    controller.presentSceneJson(sceneAlpha(goldPath, magentaPath));
    check(waitUntil([&controller, revisionBefore] {
              return controller.scenePending() == 0 && controller.sceneActive() != revisionBefore;
          }),
          "media cue: the scene did move, so the claim below is not vacuous");
    check(controller.standaloneOnAir() && controller.standalonePath() == bed,
          "media cue: a go neither restarts nor stops the production track");
    controller.selectStandaloneTrack(QString());
    check(waitUntil([&controller] { return !controller.standaloneOnAir(); }),
          "media cue: Stop takes the track down, and the removal is the acknowledgement");
    check(controller.standaloneRefusal().isEmpty() && controller.standaloneRefusalPath().isEmpty(),
          "media cue: the new selection retires the verdict the last one left");

    // --- 9. the chain re-skins the on-air slide (task TH.3) ------------------
    // A slide COMPILED FROM THE DOMAIN — not a hand-built payload — goes on
    // air, and then the service's current theme and a slide's explicit-flat
    // customization travel the same controller present path an operator's
    // edit takes. The operator's own frame must show each layer's answer,
    // after the acknowledgement that promises it.
    {
        openshow::domain::Documents chain;
        const openshow::domain::Id worn = chain.createShow("ChainJourney");
        const openshow::domain::Id cue = chain.appendSlide(worn);
        chain.setSlideLines(worn, cue, {"CHAINED"});
        const openshow::domain::Id house = chain.createTheme(
            "House", "#ffffff", "#101820", "", "", 64.0, "center", 1.25);
        const openshow::domain::Id bright = chain.createTheme(
            "Bright", "#ffe000", "#200000", "#602020", "", 72.0, "center", 1.25);
        const auto chainJson = [&chain, &worn, &cue]() {
            openshow::scene::Scene compiled;
            if (!openshow::scene::compileSlide(chain, worn, cue, &compiled)) {
                return QString();
            }
            return serialized(compiled);
        };
        check(chain.setSlideTheme(worn, cue, house), "chain: the slide follows its house theme");
        const auto revision = controller.sceneActive();
        controller.presentSceneJson(chainJson());
        check(waitUntil([&controller, revision] {
                  return controller.scenePending() == 0 &&
                         controller.sceneActive() != revision && controller.wordsOnAir() &&
                         controller.backgroundOnAir();
              }),
              "chain: the domain-compiled slide is acknowledged with both layers on air");
        check(pixelsSettled(controller, [](const QImage& frame) {
                  return nearColor(rgbAt(frame, 0.02, 0.03), 0x101820u, 12) &&
                         whitePixels(frame) > 20;
              }),
              "chain: the house theme paints its dark ground and white words");

        check(chain.setCurrentTheme(bright), "chain: the service puts on the current theme");
        const auto revisionWorn = controller.sceneActive();
        controller.presentSceneJson(chainJson());
        check(waitUntil([&controller, revisionWorn] {
                  return controller.scenePending() == 0 &&
                         controller.sceneActive() != revisionWorn;
              }),
              "chain: the current theme's re-skin is acknowledged");
        check(pixelsSettled(controller, [](const QImage& frame) {
                  return nearColor(rgbAt(frame, 0.02, 0.03), 0x200000u, 12) &&
                         nearColor(rgbAt(frame, 0.02, 0.97), 0x602020u, 24) &&
                         yellowPixels(frame) > 10;
              }),
              "chain: the audience sees the current theme's graded ground and yellow words");

        openshow::domain::SlideCustomization flat;
        flat.backgroundGradient = "";  // the slide says flat OVER the graded current theme
        check(chain.setSlideCustomizations(worn, cue, flat),
              "chain: the slide customizes itself flat");
        const auto revisionFlat = controller.sceneActive();
        controller.presentSceneJson(chainJson());
        check(waitUntil([&controller, revisionFlat] {
                  return controller.scenePending() == 0 &&
                         controller.sceneActive() != revisionFlat;
              }),
              "chain: the customized flat answer is acknowledged");
        check(pixelsSettled(controller, [](const QImage& frame) {
                  return nearColor(rgbAt(frame, 0.02, 0.03), 0x200000u, 12) &&
                         nearColor(rgbAt(frame, 0.02, 0.97), 0x200000u, 12) &&
                         yellowPixels(frame) > 10;
              }),
              "chain: the floor holds the ground colour — the ramp is gone, the words stay");
    }

    // --- 10. the operator's numbers are the audience's seconds (task TR.3) ---
    // Every leg above faded on lengths the documents had no way to name. Now
    // they do, and only this fixture can prove the whole line at once: a number
    // written through DocumentController — the class every transition control
    // commits through — reaches the audience's PIXELS through main.cpp's own
    // resolver wiring, at the length that number says. workspace_ui_tests
    // proves a gesture lands on a record and scene_frame_tests proves a record
    // drives a blend; that a gesture drives a blend is a different claim, and
    // it is the one "do not infer success from the widget value alone" asks for.
    {
        DocumentController docs(nullptr, scratch / "transition-night");
        // main.cpp's wiring, one callback for one question: the cue lane asks
        // the DOCUMENTS how this slide's cue moves, at the instant Go is
        // issued, and that answer belongs to the cue for as long as it flies.
        controller.setTransitionResolver(
            [&docs](const boost::json::object& scene) {
                return docs.transitionForSceneJson(scene);
            });
        if (!check(controller.transitionOptionsSupported(),
                   "this worker honours the durations the documents now write")) {
            std::fprintf(stderr, "FAIL: the fade lengths below could not be honoured — the whole "
                                 "leg would measure the worker's own defaults\n");
            controller.shutdownNow();
            return 1;
        }

        // Two grounds no frame can be mistaken for both, authored the way the
        // Edit page authors them: a fade's halfway point is near NEITHER, which
        // is what makes a blend evidence rather than an assumption.
        check(docs.createSong(QStringLiteral("Fade Night")) && docs.addSlide(),
              "the transition journey authors a slide");
        docs.setSlideText(QStringLiteral("MERCY"));
        docs.flushText();
        const QString showId = docs.currentSongId();
        const QString goldSlide =
            docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
        check(docs.customizeCurrentSlide(QStringLiteral("backgroundColor"),
                                         QVariant(QStringLiteral("#f0e030"))),
              "the first slide's ground is gold");
        docs.addSlide();
        docs.setSlideText(QStringLiteral("WRATH"));
        docs.flushText();
        const QString magentaSlide =
            docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();
        check(docs.customizeCurrentSlide(QStringLiteral("backgroundColor"),
                                         QVariant(QStringLiteral("#dc28c8"))),
              "the second slide's ground is magenta");
        const QString gold = docs.sceneJsonForSlide(goldSlide);
        const QString magenta = docs.sceneJsonForSlide(magentaSlide);

        // The watch: every frame the operator's own picture carried between two
        // cues, sorted into the ground it is near and the blend it is not. A
        // blend says the two cues were drawn together; the moment the second
        // ground stands alone says for how long the record said they would be.
        struct FadeWatch {
            bool blended = false;
            int blendMs = 0;
            bool landed = false;
            int landMs = 0;
        };
        const auto elapsedMsFrom = [](const std::chrono::steady_clock::time_point& started) {
            return int(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started).count());
        };
        const auto watch = [&controller, &elapsedMsFrom](uint32_t from, uint32_t to,
                                                         int budgetMs) {
            FadeWatch seen;
            const auto started = std::chrono::steady_clock::now();
            while (elapsedMsFrom(started) < budgetMs) {
                QCoreApplication::processEvents();
                const uint32_t rgb = rgbAt(controller.previewImage(), kBgU, kBgV);
                const bool here = nearColor(rgb, from, 10);
                const bool there = nearColor(rgb, to, 10);
                if (!here && !there && !seen.blended) {
                    seen.blended = true;
                    seen.blendMs = elapsedMsFrom(started);
                }
                if (there) {
                    seen.landed = true;
                    seen.landMs = elapsedMsFrom(started);
                    return seen;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return seen;
        };
        // The same watch for a clear, which has no second ground to arrive: the
        // clock stops the first time the audience is uniformly black. A budget's
        // worth means it never was, which fails the check that reads it.
        const auto msToBlack = [&controller, &elapsedMsFrom](int budgetMs) {
            const auto started = std::chrono::steady_clock::now();
            while (elapsedMsFrom(started) < budgetMs) {
                QCoreApplication::processEvents();
                if (uniformlyBlack(controller.previewImage())) {
                    return elapsedMsFrom(started);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return budgetMs * 2;
        };
        const auto settledOn = [&controller](uint32_t ground) {
            return waitUntil([&controller] { return controller.scenePending() == 0; })
                   && pixelsSettled(controller, [ground](const QImage& frame) {
                          return nearColor(rgbAt(frame, kBgU, kBgV), ground, 12);
                      });
        };

        constexpr int kAskingMs = 2600;     // a length no renderer ever had
        constexpr int kFadeFloorMs = 1700;  // "at least what was asked", with
                                            // the prepare's own round trip
                                            // counted in the same clock
        constexpr int kCutCeilingMs = 1600;  // and what a Cut clearly is not
        constexpr int kRestoreBudgetMs = 9000;  // patience for a re-prepare that
                                                // mints a fresh revision

        // --- (a) the service default, inherited by a slide that says nothing --
        check(docs.setServiceDefaultTransition(QStringLiteral("fade"), kAskingMs, kAskingMs),
              "Settings is set to a crossfade longer than this renderer ever owned");
        controller.presentSceneJson(gold);
        if (!check(settledOn(kGold),
                   "the gold cue is acknowledged and lands before the first fade is measured")) {
            controller.shutdownNow();
            return 1;
        }
        controller.presentSceneJson(magenta);
        const FadeWatch serviceFade = watch(kGold, kMagenta, 9000);
        check(serviceFade.blended && serviceFade.blendMs < 900,
              "the two grounds are drawn together early in the cue — the number in Settings is on "
                  "the wire, not just on the panel");
        check(serviceFade.landed && serviceFade.landMs >= kFadeFloorMs,
              "and they are drawn together for the length the operator typed: a service default "
                  "is a duration the audience obeys, which is the half of TR.3 no widget test "
                  "could ever show");

        // --- (b) the view's own override, saying Cut ------------------------
        check(docs.createBlankTheme(QStringLiteral("Snap")),
              "a theme exists to answer for its own cues");
        QString snapId;
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Snap"))
                snapId = theme.toMap().value(QStringLiteral("id")).toString();
        }
        check(!snapId.isEmpty() && docs.applyThemeToSlide(goldSlide, snapId)
                  && docs.applyThemeToSlide(magentaSlide, snapId),
              "both slides wear it, so their cues read the view's answer over the service's");
        QVariantMap cut;
        cut.insert(QStringLiteral("kind"), QStringLiteral("cut"));
        cut.insert(QStringLiteral("textDurationMs"), kAskingMs);
        cut.insert(QStringLiteral("mediaDurationMs"), kAskingMs);
        check(docs.setThemeTransition(snapId, cut),
              "and the view's answer is a Cut that still carries the numbers it ignores");
        QVariantMap cross;
        cross.insert(QStringLiteral("kind"), QStringLiteral("fade"));
        cross.insert(QStringLiteral("textDurationMs"), kAskingMs);
        cross.insert(QStringLiteral("mediaDurationMs"), kAskingMs);
        controller.presentSceneJson(gold);
        check(settledOn(kGold), "the gold cue is on air again under the view's Cut");
        controller.presentSceneJson(magenta);
        const FadeWatch cutFade = watch(kGold, kMagenta, 6000);
        check(!cutFade.blended,
              "no frame of a Cut holds both grounds: the view's override stopped the crossfade "
                  "the service would otherwise have performed");
        check(cutFade.landed && cutFade.landMs <= kCutCeilingMs,
              "and the cue is simply on — a fraction of the length the same slide's service would "
                  "have taken");

        // --- (c) a fade already crossing keeps its own length ---------------
        check(docs.setThemeTransition(snapId, cross), "the view crosses at that length again");
        controller.presentSceneJson(gold);
        check(settledOn(kGold), "and the crossing cue is on air and settled");
        controller.presentSceneJson(magenta);
        // The record is the CUE's, captured when Go was issued. What the
        // operator writes while the two grounds are crossing therefore belongs
        // to the next cue, and the pixels are the only honest witness.
        check(docs.setThemeTransition(snapId, cut),
              "mid-fade, the operator changes their mind and cuts the view");
        const FadeWatch inFlight = watch(kGold, kMagenta, 9000);
        check(inFlight.blended && inFlight.landed && inFlight.landMs >= kFadeFloorMs,
              "the fade already crossing finishes at the length it was given at Go: an edit "
                  "cannot shorten a cue the audience is inside of");
        controller.presentSceneJson(gold);
        const FadeWatch nextCue = watch(kMagenta, kGold, 6000);
        check(!nextCue.blended && nextCue.landed && nextCue.landMs <= kCutCeilingMs,
              "and the edit is exactly as prompt as it has to be on the NEXT cue: that one is a "
                  "Cut with no blend in it");

        // --- (d) an off-air theme presents nothing -------------------------
        // The number to compare against is the ACKNOWLEDGED one, and the two
        // clocks this fixture reads are not the same clock: `scenePending`
        // sleeps when the present's REPLY lands, while `sceneActive` moves when
        // the next status poll reports it — every half second. A baseline taken
        // at the first quiet moment is therefore the cue BEFORE the one the
        // operator last gave, and the check below would blame the off-air edit
        // for that cue's acknowledgement. So wait for the acknowledged number to
        // hold still for longer than a poll, and take it from there.
        waitUntil([&controller] { return controller.scenePending() == 0; });
        const auto quiescentRevision = [&controller]() {
            qint64 before = controller.sceneActive();
            for (int waited = 0; waited < 6000; waited += 700) {
                // Longer than one poll: the number only proves anything once a
                // status message has had its chance to disagree with it.
                settle(700);
                const qint64 after = controller.sceneActive();
                if (after == before) {
                    return after;
                }
                before = after;
            }
            return before;
        };
        const qint64 activeBeforeOffAir = quiescentRevision();
        check(docs.createBlankTheme(QStringLiteral("Nobody wears this")),
              "a theme exists that is nowhere on air");
        QString offId;
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("name")).toString()
                    == QStringLiteral("Nobody wears this"))
                offId = theme.toMap().value(QStringLiteral("id")).toString();
        }
        QVariantMap lingering;
        lingering.insert(QStringLiteral("kind"), QStringLiteral("fade"));
        lingering.insert(QStringLiteral("textDurationMs"), 9000);
        lingering.insert(QStringLiteral("mediaDurationMs"), 9000);
        check(!offId.isEmpty() && docs.setThemeTransition(offId, lingering),
              "and it is given a nine-second crossfade nobody is looking at");
        settle(600);
        check(controller.sceneActive() == activeBeforeOffAir && controller.scenePending() == 0,
              "editing a theme that is not on air presents nothing: no new revision, nothing "
                  "pending");
        check(nearColor(rgbAt(controller.previewImage(), kBgU, kBgV), kGold, 12),
              "and the picture the operator is looking at did not move either");

        // --- (e) an explicit clear is a Cut; a restore keeps the cue's own --
        // Two rules, both about which record the audience obeys when the
        // documents have moved since the cue. First the clear: the operator
        // said "gone", so the ground crosses to black at nothing, even with the
        // view set to cross — magenta→black passes through dark magenta, so the
        // clock here cannot mistake a 2.6 s cross for the instant the operator
        // asked for. Then the restore, which is measured where a blend is
        // possible: the WORDS clear leaves the ground on air, so the restored
        // words have an outgoing scene to cross in against, and the length of
        // that crossing tells whether the restore brought the cue's own record
        // or whatever the theme says by now. (The theme says Cut.)
        check(docs.setThemeTransition(snapId, cross), "the view crosses at 2.6 s");
        controller.presentSceneJson(magenta);
        if (!check(settledOn(kMagenta), "a crossing magenta cue is on air, words included")) {
            controller.shutdownNow();
            return 1;
        }
        const int wordsFull = whitePixels(controller.previewImage());
        check(wordsFull > 60,
              "the settled cue's words are a real count for the restore to come back to");
        controller.clearSlide();
        check(waitUntil([&controller] {
                  return !controller.wordsOnAir() && controller.backgroundOnAir()
                         && controller.scenePending() == 0;
              }),
              "the words clear is acknowledged with the ground still on air");
        check(waitUntil([&controller] { return whitePixels(controller.previewImage()) < 5; }),
              "and the operator's own frame has no words in it");
        check(controller.canRestore(), "restore is offered against the words the clear took");
        // The record travels WITH the retained content from the moment the clear
        // takes it, so the documents are never consulted again for a restore.
        // Which is exactly what the edit below is: it happens after the clear,
        // and the audience must not hear about it.
        check(docs.setThemeTransition(snapId, cut),
              "the operator cuts the view AFTER the clear took its snapshot");
        const auto restoreStarted = std::chrono::steady_clock::now();
        controller.restoreOutput();
        int wordsBackIn = kRestoreBudgetMs + 1;
        while (elapsedMsFrom(restoreStarted) <= kRestoreBudgetMs) {
            QCoreApplication::processEvents();
            if (whitePixels(controller.previewImage()) >= wordsFull * 9 / 10) {
                wordsBackIn = elapsedMsFrom(restoreStarted);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(wordsBackIn >= kFadeFloorMs,
              "the restored words cross in over the length the snapshot carried, not at the "
                  "instant the theme now says: the record the retained content is owed was "
                  "resolved when the clear took it, and an edit since then changes nothing");
        check(waitUntil([&controller, wordsFull] {
                  return controller.wordsOnAir() && controller.backgroundOnAir()
                         && whitePixels(controller.previewImage()) >= wordsFull * 9 / 10
                         && controller.scenePending() == 0;
              }),
              "and the restore lands whole — acknowledged and on the operator's frame");
        // Now the full clear, with the view set to cross so a Cut is something
        // the fixture had to earn rather than the default it fell into.
        check(docs.setThemeTransition(snapId, cross), "the view crosses again");
        controller.clearAll();
        const int clearedIn = msToBlack(4000);
        check(waitUntil([&controller] {
                  return !controller.wordsOnAir() && !controller.backgroundOnAir()
                         && controller.scenePending() == 0;
              }),
              "the full clear is acknowledged with nothing left on air");
        check(clearedIn <= kCutCeilingMs,
              "and the audience is black without crossing: an explicit clear is a Cut whatever "
                  "the view's numbers say");

        // --- (f) live markers still wait for the worker's own ack ----------
        controller.presentCueJson(QStringLiteral("cue-long"), gold);
        check(controller.pendingCueKey() == QStringLiteral("cue-long"),
              "a cued go carrying duration keys is pending the moment it leaves");
        check(waitUntil([&controller] {
                  return controller.liveCueKey() == QStringLiteral("cue-long")
                         && controller.pendingCueKey().isEmpty() && controller.scenePending() == 0;
              }),
              "and the LIVE claim still lights only when this worker says the revision those "
                  "durations rode on is active");

        // --- (g) the number that drove tonight is tomorrow's number --------
        check(docs.save(), "the service saves with the view's record");
        openshow::domain::Documents reopened;
        std::string loadError;
        if (check(openshow::domain::loadService(&reopened, scratch / "transition-night" / "service",
                                                &loadError),
                  "the next session reads the service back")) {
            const openshow::domain::TransitionOptions carried =
                reopened.resolveTransitionForSlide(showId.toStdString(),
                                                   magentaSlide.toStdString());
            check(carried.kind == openshow::domain::TransitionKind::kFade
                      && carried.textDurationMs == kAskingMs
                      && carried.mediaDurationMs == kAskingMs,
                  "and the cue lane's answer for that slide tomorrow is the record that drove the "
                      "audience tonight — the measured milliseconds are the stored ones");
        }

        // --- (h) a restarted worker is black, and the numbers stay ----------
        // The runbook's last clause. production_output_tests owns the audience's
        // pixels for a brand-new worker (every pixel black, footer included);
        // what belongs HERE is that the frontend's OWN preview of a fresh
        // session shows that black, that the new session still speaks the
        // duration contract it never negotiated before, and that the record the
        // cue lane answers with did not go down with the process: tonight's
        // milliseconds live in the document, not in a worker's memory.
        controller.shutdownNow();
        {
            WorkerController restarted;
            restarted.setTransitionResolver(
                [&docs](const boost::json::object& scene) {
                    return docs.transitionForSceneJson(scene);
                });
            restarted.start();
            if (!check(waitUntil(
                           [&restarted] {
                               return restarted.outputLive() && restarted.previewActive()
                                      && !restarted.previewImage().isNull();
                           },
                           15000),
                       "a restarted worker is up and streaming the operator's frame again")) {
                restarted.shutdownNow();
                std::filesystem::remove_all(scratch);
                return 1;
            }
            check(restarted.transitionOptionsSupported(),
                  "a session that never saw tonight's cues still honours the duration keys, so "
                      "the contract is the build's and not one worker's");
            check(!restarted.wordsOnAir() && !restarted.backgroundOnAir()
                      && uniformlyBlack(restarted.previewImage()),
                  "and it starts on black: no words, no ground, and every corner of the "
                      "operator's preview dark");
            const QString magentaScene = docs.sceneJsonForSlide(magentaSlide);
            const openshow::domain::TransitionOptions afterRestart = docs.transitionForSceneJson(
                boost::json::parse(magentaScene.toStdString()).as_object());
            check(afterRestart.kind == openshow::domain::TransitionKind::kFade
                      && afterRestart.textDurationMs == kAskingMs
                      && afterRestart.mediaDurationMs == kAskingMs,
                  "while the answer for the same slide is the 2.6 s the audience crossed: the next "
                      "Go is asked the documents, and the documents kept their word");
            restarted.shutdownNow();
        }
    }

    std::printf("frontend_journey: go/gradient/composed clears/restore across a 16-deep prune, "
                "adoption of both plates with restore from the new paths, a real missing-"
                "picture fault (%s) repaired, a cue lane whose live marker only lights for "
                "the cue the worker acknowledged, and a theme chain whose re-skins land as "
                "acknowledged pixels — all in status AND in the copied frame\n",
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
