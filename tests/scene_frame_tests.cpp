/*
 * Task 6.11a (review R13): one immutable frame, drawn to every target.
 *
 * The worker composes one moment and shows it twice — the audience panel
 * and the operator's preview. Before this task both called the same
 * SceneRenderer::draw(canvas, ..., timeSeconds, ...), and THAT CALL
 * ADVANCED the shared crossfade: the preview's own later timestamp moved
 * the live fade along with it, so a preview could run a fade to a blend the
 * audience had never been shown, and could retire the fade while the panel
 * still held a half-blended picture. "Draw into a target" was not a
 * read-only operation, and the two targets were not showing one frame.
 *
 * The contract is three calls:
 *
 *     frame = renderer->captureFrame(audienceClock, revision);   // once/frame
 *     renderer->draw(frame, panel, ...);
 *     renderer->draw(frame, preview, ...);                        // read-only
 *     renderer->commitFrame(frame);                               // on present
 *
 * and each scenario below is one way that contract can be broken:
 *
 *   - the two targets of one frame agree and sit at neither end of the fade
 *     (agreement alone would be satisfied by two black frames, so the ends
 *     are measured first and required to be far apart);
 *   - drawing is time-less and idempotent — five hundred preview draws move
 *     neither the pixels nor the fade's clock;
 *   - a captured frame does not follow the show, which is exactly what lets
 *     a preview redraw the frame the audience was last shown;
 *   - the layers read the FRAME rather than the renderer's live state — the
 *     mutation R13 found;
 *   - a fade ends when the audience HAS the settled frame, not when a frame
 *     merely computed it: an uncommitted completion keeps the loop owing the
 *     panel, and a stale completion cannot retire a newer fade;
 *   - the revision tag belongs to the frame, so published pixels and the
 *     published revision cannot come from different moments.
 *
 * Everything is CPU raster (no display, no Vulkan) with a fixed wall clock,
 * so a byte comparison is exact. Scenes are one flat colour and one word:
 * the fixture judges composition, not layout.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include "openshow/scene/scene.h"

#include "render/Layer.h"
#include "render/SceneRenderer.h"

namespace {

using openshow::worker::ActiveScene;
using openshow::worker::SceneFrame;
using openshow::worker::SceneRenderer;
using openshow::worker::SceneTransitionKind;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

// Flat colours far apart in two channels each, and one word per scene so the
// words layer has something to compose (its alpha track is the text half of
// the same captured frame).
constexpr std::uint32_t kRed = 0xE03030u;
constexpr std::uint32_t kBlue = 0x3030E0u;
constexpr std::uint32_t kGreen = 0x30C030u;

// A wall clock that never moves: two frames of one composition must be byte
// identical, and the clock layer would otherwise differ by the second.
constexpr std::time_t kFixedWall = 1234567890;

// The crossfade's durations (SceneRenderer's media/text defaults, FreeShow's
// two transition times: 800 ms media, 500 ms text). The scenarios step the
// audience clock by them and name the blends they expect back: half media,
// four-fifths text at the halfway instant.
constexpr double kAudienceStart = 100.0;  // an arbitrary moment in the show
constexpr double kHalfFade = 0.4;         // media 0.5, text 0.8
constexpr double kFullFade = 0.8;         // both tracks settled

// The audience panel and the operator's preview, at the ratios the session
// really draws (a third of the authored canvas, and half of that again).
constexpr int kPanelWidth = 640;
constexpr int kPanelHeight = 360;
constexpr int kPreviewWidth = 320;
constexpr int kPreviewHeight = 180;

// How far apart the two targets' redness may read. They differ only in
// scale, so the difference is antialiasing of the word's edges: measured
// 0.11 at worst across every scenario here. One frame's slip of the fade
// (1/60 s of the 800 ms media track) moves this number by ~7, so the 3.0
// ceiling still catches a preview running ahead of the panel.
constexpr double kTargetAgreement = 3.0;

// Measured ink on the 640x360 panel for one 120pt word: ~1370 px on the red
// scene and ~1220 px on the blue one, against ~2030 px mid fade, where both
// words are on screen (the text track dissolves at 0.8/0.2). The floors
// below sit at well under half of what the targets really draw, and the
// mid-fade ratio at 5/4 separates "two words" from "one".
constexpr int kPanelInkFloor = 400;
constexpr int kPreviewInkFloor = 200;

// The mixed background at media blend 0.5 (half of 0xE03030 plus half of
// 0x3030E0 = 0x873087), which is what "ink" is measured against mid fade.
constexpr std::uint32_t kMidBlendColour = 0x873087u;

// The fade's ends and middle, in redness (mean red minus mean blue):
// measured +175 for the red scene, -175 for the blue one, 0.0 at the media
// midpoint. The bounds below keep the midpoint honest.
constexpr double kEndDistance = 150.0;
constexpr double kMidpointCeiling = 40.0;

ActiveScene stage(std::uint32_t colour, const char* word) {
    auto scene = std::make_shared<openshow::scene::Scene>();
    scene->showId = "r13";
    scene->slideId = word;
    scene->background.color = colour;
    openshow::scene::TextItem item;
    item.lines = {word};
    item.color = 0xFFFFFFu;
    item.fontSize = 120.0;
    scene->items.push_back(item);
    return scene;
}

std::unique_ptr<SceneRenderer> createRenderer() {
    std::unique_ptr<SceneRenderer> renderer = std::make_unique<SceneRenderer>();
    std::string error;
    if (!renderer->create(&error)) {
        std::fprintf(stderr, "FATAL: SceneRenderer::create: %s\n", error.c_str());
        std::exit(1);
    }
    return renderer;
}

bool nearBlend(float value, double expected, double tolerance = 0.02) {
    return std::abs(static_cast<double>(value) - expected) <= tolerance;
}

// One drawing target: a raster canvas of the given device size, read back
// after every draw so the checks judge the pixels the target was handed.
class Target {
public:
    Target(SceneRenderer& renderer, int width, int height)
        : fRenderer(renderer), fWidth(width), fHeight(height) {
        fSurface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (!fSurface) {
            std::fprintf(stderr, "FATAL: raster surface allocation failed\n");
            std::exit(1);
        }
    }

    void draw(const SceneFrame& frame) {
        fRenderer.draw(frame, fSurface->getCanvas(), fWidth, fHeight);
        fPixels.resize(static_cast<std::size_t>(fWidth) * static_cast<std::size_t>(fHeight));
        const bool ok =
            fSurface->readPixels(SkImageInfo::MakeN32Premul(fWidth, fHeight),
                                 fPixels.data(),
                                 static_cast<std::size_t>(fWidth) * sizeof(SkColor),
                                 0,
                                 0);
        if (!ok) {
            std::fprintf(stderr, "FATAL: target readback failed\n");
            std::exit(1);
        }
    }

    // Mean red minus mean blue over the whole target. Both scenes are flat
    // and their words are white (R == B, so ink barely moves this number),
    // which makes it a direct read of WHERE a target is in the fade while
    // staying insensitive to how much text a smaller target rasterises: the
    // two targets must differ in scale, never in composition.
    double redness() const {
        double red = 0.0;
        double blue = 0.0;
        for (const SkColor pixel : fPixels) {
            red += SkColorGetR(pixel);
            blue += SkColorGetB(pixel);
        }
        const double count = static_cast<double>(fPixels.size());
        return red / count - blue / count;
    }

    double meanGreen() const {
        double green = 0.0;
        for (const SkColor pixel : fPixels) {
            green += SkColorGetG(pixel);
        }
        return green / static_cast<double>(fPixels.size());
    }

    // Pixels that are not the background colour: ink, whatever it is.
    int inkAgainst(std::uint32_t background, int tolerance) const {
        int count = 0;
        for (const SkColor pixel : fPixels) {
            if (std::abs(static_cast<int>(SkColorGetR(pixel)) -
                         static_cast<int>((background >> 16) & 0xFFu)) > tolerance ||
                std::abs(static_cast<int>(SkColorGetG(pixel)) -
                         static_cast<int>((background >> 8) & 0xFFu)) > tolerance ||
                std::abs(static_cast<int>(SkColorGetB(pixel)) -
                         static_cast<int>(background & 0xFFu)) > tolerance) {
                ++count;
            }
        }
        return count;
    }

    // Pixels whose every channel is at least `floor`: a word drawn at full
    // strength on a flat background reads white here, while the same word
    // drawn AT the frame's textBlend is tinted by the picture under it. This
    // is the meter for the text track's alpha, which mean-channel measures
    // cannot see (white ink barely moves a mean of means).
    int nearWhite(int floor = 245) const {
        int count = 0;
        for (const SkColor pixel : fPixels) {
            if (SkColorGetR(pixel) >= floor && SkColorGetG(pixel) >= floor &&
                SkColorGetB(pixel) >= floor) {
                ++count;
            }
        }
        return count;
    }

    bool identicalTo(const Target& other) const {
        if (other.fPixels.size() != fPixels.size()) {
            return false;
        }
        for (std::size_t i = 0; i < fPixels.size(); ++i) {
            if (fPixels[i] != other.fPixels[i]) {
                return false;
            }
        }
        return true;
    }

private:
    SceneRenderer& fRenderer;
    int fWidth;
    int fHeight;
    sk_sp<SkSurface> fSurface;
    std::vector<SkColor> fPixels;
};

// --- one frame, two targets ---------------------------------------------
//
// The review's headline: a preview could show a blend the audience had not
// been shown. Here one captured frame goes to both targets and the two must
// agree — and, to keep the check from passing on two black frames, the ends
// of the fade are measured first and the midpoint required to sit between
// them.
void testBothTargetsDrawTheSameComposition() {
    std::unique_ptr<SceneRenderer> renderer = createRenderer();
    Target panel(*renderer, kPanelWidth, kPanelHeight);
    Target preview(*renderer, kPreviewWidth, kPreviewHeight);

    const ActiveScene red = stage(kRed, "ALPHA");
    renderer->setActiveScene(red, SceneTransitionKind::kCut);
    const SceneFrame steady = renderer->captureFrame(10.0, 1, kFixedWall);
    check(steady.fadeFrom == nullptr, "a cut leaves nothing to fade from");
    check(steady.mediaBlend >= 1.0f && steady.textBlend >= 1.0f,
          "a steady frame is the on-air scene at full strength");
    panel.draw(steady);
    preview.draw(steady);
    const double redOnPanel = panel.redness();
    const double redOnPreview = preview.redness();
    check(redOnPanel > kEndDistance, "the panel's steady frame is the red scene, not a black one");
    check(redOnPreview > kEndDistance, "and so is the preview's, at a different scale");
    check(panel.inkAgainst(kRed, 12) > kPanelInkFloor, "the panel composed the words");
    check(preview.inkAgainst(kRed, 12) > kPreviewInkFloor,
          "the preview composed the words too, not just the colour fill");

    renderer->setActiveScene(stage(kBlue, "BETA"), SceneTransitionKind::kFade);
    check(renderer->transitionRunning(), "a fade is running after a fade presentation");

    // Fade origin: the first capture stamps the start, so this frame is
    // still the outgoing scene.
    const SceneFrame opening =
        renderer->captureFrame(kAudienceStart, 7, kFixedWall);
    check(opening.fadeFrom != nullptr, "the fade's first frame carries the outgoing scene");
    check(nearBlend(opening.mediaBlend, 0.0), "and it is all outgoing media");
    panel.draw(opening);
    preview.draw(opening);
    check(std::abs(panel.redness() - preview.redness()) <= kTargetAgreement,
          "both targets hold the SAME blend of one frame (fade start)");
    check(panel.redness() > kEndDistance, "at the start that blend is still the red scene");

    // Halfway: media 0.5, text 0.8. The two targets must still agree, and
    // must be nowhere near either end — a preview-only advance would show
    // up here as the preview reading a higher blend than the panel.
    const SceneFrame middle =
        renderer->captureFrame(kAudienceStart + kHalfFade, 8, kFixedWall);
    check(nearBlend(middle.mediaBlend, 0.5), "half the media duration is a half media blend");
    check(nearBlend(middle.textBlend, 0.8),
          "the text track runs on its own faster clock (0.4s of 0.5s)");
    panel.draw(middle);
    preview.draw(middle);
    const double midPanel = panel.redness();
    const double midPreview = preview.redness();
    check(std::abs(midPanel - midPreview) <= kTargetAgreement,
          "both targets hold the SAME blend of one frame (mid fade)");
    check(std::abs(midPanel) < kMidpointCeiling,
          "the midpoint is a genuine mix — neither the red nor the blue end");
    // The words have their own alpha track inside the same frame, so mid fade
    // BOTH words are on screen and more pixels differ from the mixed
    // background than on a settled frame. A words layer that drew at full
    // strength instead of the frame's textBlend would show one word here.
    const int midInk = panel.inkAgainst(kMidBlendColour, 12);
    // The text track's alpha, measured rather than trusted. At textBlend 0.8
    // the incoming word is tinted by the mix under it: its brightest channel
    // can reach no more than 0.8*255 + 0.2*0x87 = 231, so no pixel is
    // "white" (measured: 0 on both targets). A settled word is white ink
    // (measured: 829 pixels on the panel, 147 on the preview). Mean-channel
    // metrics cannot see this rule at all — white ink barely moves a mean —
    // which is why the fixture counts pixels instead.
    const int midWhite = panel.nearWhite();

    // Settled: one scene, full strength, both targets.
    const SceneFrame settled =
        renderer->captureFrame(kAudienceStart + kFullFade, 9, kFixedWall);
    check(settled.fadeFrom == nullptr, "the fade's last frame is the on-air scene alone");
    check(settled.mediaBlend >= 1.0f && settled.textBlend >= 1.0f,
          "and both tracks are complete");
    panel.draw(settled);
    preview.draw(settled);
    const int settledInk = panel.inkAgainst(kBlue, 12);
    const int settledWhite = panel.nearWhite();
    check(std::abs(panel.redness() - preview.redness()) <= kTargetAgreement,
          "both targets hold the SAME blend of one frame (settled)");
    check(panel.redness() < -kEndDistance, "which is the blue scene at full strength");
    // The metric itself, stated so the numbers above mean what they claim.
    check(redOnPanel - midPanel > kEndDistance - kMidpointCeiling &&
              midPanel - panel.redness() > kEndDistance - kMidpointCeiling,
          "redness separates the ends from the middle — the checks above bite");
    check(midInk * 4 >= settledInk * 5,
          "mid fade carries BOTH words: the text track dissolves through the frame's "
          "textBlend instead of the words snapping to full strength");
    check(settledWhite > 400,
          "a settled word is white ink — the whiteness meter has something to measure");
    check(midWhite * 4 <= settledWhite,
          "the incoming word is drawn AT the frame's textBlend: mid fade it is tinted by "
          "the picture under it, not opaque white");
}

// --- drawing is not a clock ---------------------------------------------
//
// A hundred preview ticks, a stalled preview consumer that retries, a debug
// target that redraws the same frame: none of it may move the audience's
// show. The frame is drawn 500 times to the preview and 200 to the panel;
// then the same instant is captured again and must be identical, in field
// values and in bytes, while the fade's clock still belongs to the audience.
void testDrawingMovesNeitherPixelsNorClock() {
    std::unique_ptr<SceneRenderer> renderer = createRenderer();
    Target panel(*renderer, kPanelWidth, kPanelHeight);
    Target preview(*renderer, kPreviewWidth, kPreviewHeight);

    renderer->setActiveScene(stage(kRed, "ALPHA"), SceneTransitionKind::kCut);
    const SceneFrame first = renderer->captureFrame(10.0, 3, kFixedWall);
    panel.draw(first);
    renderer->commitFrame(first);

    renderer->setActiveScene(stage(kBlue, "BETA"), SceneTransitionKind::kFade);
    renderer->captureFrame(kAudienceStart, 20, kFixedWall);  // stamps the origin
    const SceneFrame middle =
        renderer->captureFrame(kAudienceStart + kHalfFade, 20, kFixedWall);
    check(nearBlend(middle.mediaBlend, 0.5), "the frame under test is mid fade");
    panel.draw(middle);

    for (int i = 0; i < 500; ++i) {
        preview.draw(middle);
    }
    for (int i = 0; i < 200; ++i) {
        panel.draw(middle);
    }
    check(renderer->transitionRunning(),
          "seven hundred draws did not finish the audience's fade");

    const SceneFrame again =
        renderer->captureFrame(kAudienceStart + kHalfFade, 20, kFixedWall);
    check(again.mediaBlend == middle.mediaBlend && again.textBlend == middle.textBlend,
          "the same instant still computes the same blends after all that drawing");
    check(again.scene == middle.scene && again.fadeFrom == middle.fadeFrom,
          "and the same two scenes");
    check(again.revision == middle.revision, "with the same revision tag");
    Target copy(*renderer, kPanelWidth, kPanelHeight);
    copy.draw(again);
    check(copy.identicalTo(panel),
          "the panel's picture is byte for byte what it was before the retries");

    // The audience's clock is the only thing that moved it: same scene,
    // later instant, further blend.
    const SceneFrame later =
        renderer->captureFrame(kAudienceStart + kFullFade, 20, kFixedWall);
    check(nearBlend(later.mediaBlend, 1.0) && nearBlend(later.textBlend, 1.0),
          "one audience frame later, the fade has reached its end");
    check(renderer->captureFrame(11.0, 21).wallClock != 0,
          "wallClock 0 means the audience's clock, not an unset one");
}

// --- a captured frame does not follow the show --------------------------
//
// The session republishes the LAST PRESENTED frame to the preview when the
// audience is owed nothing. That is only sound while a frame's pixels depend
// on nothing but the frame: cut the show away underneath it and a redraw
// must still produce the blend the audience was shown.
void testCapturedFrameDoesNotFollowTheShow() {
    std::unique_ptr<SceneRenderer> renderer = createRenderer();
    Target panel(*renderer, kPanelWidth, kPanelHeight);

    renderer->setActiveScene(stage(kRed, "ALPHA"), SceneTransitionKind::kCut);
    renderer->captureFrame(10.0, 10, kFixedWall);
    renderer->setActiveScene(stage(kBlue, "BETA"), SceneTransitionKind::kFade);
    renderer->captureFrame(kAudienceStart, 11, kFixedWall);
    const SceneFrame held =
        renderer->captureFrame(kAudienceStart + kHalfFade, 12, kFixedWall);
    panel.draw(held);
    const double heldRedness = panel.redness();
    check(std::abs(heldRedness) < kMidpointCeiling, "the held frame is the mid-fade mix");

    // The show moves on underneath it: a hard cut to green.
    renderer->setActiveScene(stage(kGreen, "GAMMA"), SceneTransitionKind::kCut);
    Target replay(*renderer, kPanelWidth, kPanelHeight);
    replay.draw(held);
    check(replay.identicalTo(panel),
          "redrawing the captured frame still shows the blend the audience was owed");
    check(held.scene->slideId == "BETA", "and the frame still names the scene it drew");

    const SceneFrame fresh = renderer->captureFrame(kAudienceStart + 0.5, 13, kFixedWall);
    check(fresh.scene != held.scene, "a new capture composes the new scene");
    check(fresh.fadeFrom == nullptr, "the cut left no fade behind");
    Target now(*renderer, kPanelWidth, kPanelHeight);
    now.draw(fresh);
    check(!now.identicalTo(replay), "and it is a different picture");
    check(now.meanGreen() > 120.0, "which is the green scene");
}

// --- the layers read the frame, not the renderer ------------------------
//
// The exact mutation R13 describes: layers that pointed at the renderer's
// live scene and transition. Here the renderer is told one thing and the
// frame says another; the pixels must follow the frame.
void testLayersDrawTheFrameNotTheLiveState() {
    std::unique_ptr<SceneRenderer> renderer = createRenderer();
    Target panel(*renderer, kPanelWidth, kPanelHeight);

    const ActiveScene red = stage(kRed, "ALPHA");
    const ActiveScene blue = stage(kBlue, "BETA");
    renderer->setActiveScene(red, SceneTransitionKind::kCut);
    renderer->captureFrame(10.0, 30, kFixedWall);

    // A frame hand-built as "all incoming blue", while the renderer itself
    // still has only red on air and no fade.
    SceneFrame synthetic;
    synthetic.scene = blue;
    synthetic.fadeFrom = red;
    synthetic.mediaBlend = 1.0f;
    synthetic.textBlend = 1.0f;
    synthetic.timeSeconds = kAudienceStart;
    synthetic.wallClock = kFixedWall;
    synthetic.revision = 31;
    panel.draw(synthetic);
    check(panel.redness() < -kEndDistance,
          "the layers drew the FRAME's scene (blue), not the renderer's red");

    // And a frame that says "still fading out of blue" while the renderer is
    // on red with nothing in flight: the outgoing scene must appear.
    SceneFrame fading;
    fading.scene = red;
    fading.fadeFrom = blue;
    fading.mediaBlend = 0.0f;
    fading.textBlend = 0.0f;
    fading.timeSeconds = kAudienceStart;
    fading.wallClock = kFixedWall;
    fading.revision = 32;
    panel.draw(fading);
    check(panel.redness() < -kEndDistance,
          "a frame at blend 0 shows ITS outgoing scene (the blue it names), not the "
          "renderer's on-air red");
    check(renderer->activeScene() == red, "and the renderer's state was not touched by it");
}

// --- a fade ends on commit, not on computation --------------------------
//
// Retirement is the present's job: the event-driven loop presents while
// transitionRunning() is true, so a completion that was computed but never
// reached the panel must keep the loop paying — otherwise the audience is
// left holding a blend nobody intends to finish.
void testFadeEndsWhenTheAudienceHasTheFrame() {
    std::unique_ptr<SceneRenderer> renderer = createRenderer();
    Target panel(*renderer, kPanelWidth, kPanelHeight);
    Target preview(*renderer, kPreviewWidth, kPreviewHeight);

    renderer->setActiveScene(stage(kRed, "ALPHA"), SceneTransitionKind::kCut);
    renderer->commitFrame(renderer->captureFrame(10.0, 40, kFixedWall));
    renderer->setActiveScene(stage(kBlue, "BETA"), SceneTransitionKind::kFade);

    const SceneFrame opening = renderer->captureFrame(kAudienceStart, 41, kFixedWall);
    panel.draw(opening);
    renderer->commitFrame(opening);
    check(renderer->transitionRunning(), "a credited first blend is not the end of the fade");

    const SceneFrame done =
        renderer->captureFrame(kAudienceStart + kFullFade, 42, kFixedWall);
    check(done.mediaBlend >= 1.0f && done.textBlend >= 1.0f,
          "this frame carries the settled picture");
    check(renderer->transitionRunning(),
          "computing the last blend does not retire the fade");
    panel.draw(done);
    preview.draw(done);
    panel.draw(done);
    check(renderer->transitionRunning(),
          "drawing a completed frame never retires the fade either — only the present does");

    renderer->commitFrame(done);
    check(!renderer->transitionRunning(),
          "the present retires it: the loop stops owing the panel a frame");
    check(renderer->captureFrame(kAudienceStart + 1.0, 43, kFixedWall).fadeFrom == nullptr,
          "after the commit nothing fades");
    // Committing the same frame twice must be harmless (a retried credit).
    renderer->commitFrame(done);
    check(!renderer->transitionRunning(), "crediting it again changes nothing");

    // A stale completion cannot retire a fade a NEWER presentation started:
    // `done` fades to blue, blue is no longer on air.
    const ActiveScene green = stage(kGreen, "GAMMA");
    renderer->setActiveScene(green, SceneTransitionKind::kFade);
    renderer->captureFrame(200.0, 44, kFixedWall);
    const SceneFrame midNew = renderer->captureFrame(200.4, 45, kFixedWall);
    check(midNew.fadeFrom != nullptr, "a new fade is in flight");
    renderer->commitFrame(done);
    check(renderer->transitionRunning(),
          "a stale completion cannot retire a newer fade — the audience keeps its blend");
    const SceneFrame stillFading = renderer->captureFrame(200.4, 46, kFixedWall);
    check(stillFading.fadeFrom != nullptr && nearBlend(stillFading.mediaBlend, 0.5),
          "the new fade is still mid blend, exactly where it was");
    const SceneFrame newDone = renderer->captureFrame(200.8, 47, kFixedWall);
    renderer->commitFrame(newDone);
    check(!renderer->transitionRunning(), "its OWN completion ends it");
    check(renderer->activeScene() == green, "and the on-air scene is the newer one");
}

// --- the revision belongs to the frame ----------------------------------
//
// Task 6.4's rule, moved inside the snapshot: the tag a preview publishes
// must belong to the pixels it publishes. So a frame captured under revision
// 41 keeps drawing revision 41's scene after a newer cut, and only a new
// capture carries the newer tag.
void testRevisionTravelsWithTheFrame() {
    std::unique_ptr<SceneRenderer> renderer = createRenderer();
    Target preview(*renderer, kPreviewWidth, kPreviewHeight);

    renderer->setActiveScene(stage(kRed, "ALPHA"), SceneTransitionKind::kCut);
    const SceneFrame tagged = renderer->captureFrame(10.0, 41, kFixedWall);
    check(tagged.revision == 41, "captureFrame stamps the revision it was given");
    check(tagged.timeSeconds == 10.0 && tagged.wallClock == kFixedWall,
          "and the frame carries its own clock");
    preview.draw(tagged);

    renderer->setActiveScene(stage(kBlue, "BETA"), SceneTransitionKind::kCut);
    const SceneFrame next = renderer->captureFrame(10.5, 42, kFixedWall);
    check(next.revision == 42 && tagged.revision == 41, "each frame keeps its own tag");

    Target replay(*renderer, kPreviewWidth, kPreviewHeight);
    replay.draw(tagged);
    check(replay.identicalTo(preview),
          "the older frame still draws the older scene: tag and pixels cannot come from "
          "different moments");
    check(replay.redness() > kEndDistance, "which is the red the tag names");
    Target latest(*renderer, kPreviewWidth, kPreviewHeight);
    latest.draw(next);
    check(latest.redness() < -kEndDistance, "and the newer frame is the blue its tag names");
}

}  // namespace

int main() {
    testBothTargetsDrawTheSameComposition();
    testDrawingMovesNeitherPixelsNorClock();
    testCapturedFrameDoesNotFollowTheShow();
    testLayersDrawTheFrameNotTheLiveState();
    testFadeEndsWhenTheAudienceHasTheFrame();
    testRevisionTravelsWithTheFrame();

    if (gFailures != 0) {
        std::fprintf(stderr, "scene_frame_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("ok: one captured frame serves every target, and only the audience's "
                "clock and the present move it\n");
    return 0;
}
