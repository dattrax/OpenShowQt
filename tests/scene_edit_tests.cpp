/*
 * Task 3.4 fixtures: SceneEdit — the clear controls' transforms on the
 * scene wire form (Qt Core only, no display).
 *
 * The clear buttons derive a presentation from the scene JSON on air, so
 * the transforms must be wire-honest: the result must satisfy the strict
 * scene::fromJson validator the worker applies, remove exactly the layer
 * asked for, and leave everything else (provenance, style, the other
 * layer) byte-equivalent. The content flags answer the other half of the
 * acceptance — which clear buttons are enabled and which layers the live
 * indicators light — so they are pinned here too. Task 6.21e (review F4)
 * added the gradient: it is part of the background, so a clear drops it and
 * the flag counts it, in both of its shapes (bare black-to-colour canvas and
 * under a picture).
 */

#include <cstdio>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include <QCoreApplication>

#include "openshow/scene/scene.h"

#include "SceneEdit.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::scene::Scene;
using openshow::scene::SceneItem;
using openshow::ui::SceneContent;

// A scene with every interesting part present: a coloured background
// carrying an image and a second colour, and two text items. The
// transforms are judged against this full picture.
QString fullSceneJson() {
    Scene scene;
    scene.showId = "show7";
    scene.slideId = "slide3";
    scene.background.color = 0x112233;
    scene.background.gradient = 0x336699;  // 6.21e: part of the background
    scene.background.hasGradient = true;
    scene.background.hasImage = true;
    scene.background.assetId = "asset2";
    scene.background.assetPath = "/media/grape.jpg";
    scene.items = {
        SceneItem{.lines = {"First line", "second line"}, .color = 0xFFFFFF,
                  .fontFamily = "serif", .fontSize = 64},
        SceneItem{.lines = {"Doxology"}, .color = 0xFFDD44, .fontSize = 48},
    };
    return QString::fromStdString(boost::json::serialize(openshow::scene::toJson(scene)));
}

Scene parseScene(const QString& json, bool* ok) {
    Scene scene;
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(json.toStdString(), parseError);
    *ok = !parseError && openshow::scene::fromJson(parsed, &scene);
    return scene;
}

// Task 6.21e (review F4): the gradient payloads the flat fixtures never
// carried. A black-to-colour scene paints a whole canvas of ramp while its
// top colour alone reads as "no background" — the exact payload that made
// the clear button sit disabled over a coloured audience.
QString gradientScene(bool withImage) {
    Scene scene;
    scene.showId = "show7";
    scene.slideId = withImage ? "slide-grad-image" : "slide-grad-black";
    scene.background.color = openshow::scene::kBlack;
    scene.background.gradient = 0xFF8000u;
    scene.background.hasGradient = true;
    if (withImage) {
        scene.background.hasImage = true;
        scene.background.assetId = "asset-grad";
        scene.background.assetPath = "/media/plate.jpg";
    }
    SceneItem word;
    word.lines = {"Graduated"};
    scene.items.push_back(std::move(word));
    return QString::fromStdString(boost::json::serialize(openshow::scene::toJson(scene)));
}

bool carriesGradientKey(const QString& json) {
    boost::system::error_code error;
    const boost::json::value parsed = boost::json::parse(json.toStdString(), error);
    if (error || !parsed.is_object()) {
        return false;
    }
    const auto background = parsed.as_object().find("background");
    if (background == parsed.as_object().end() || !background->value().is_object()) {
        return false;
    }
    return background->value().as_object().contains("gradient");
}

void exerciseContentFlags() {
    const SceneContent full = openshow::ui::sceneContent(fullSceneJson());
    check(full.words && full.background, "a slide with items and a coloured fill has both");

    Scene imageless;
    imageless.background.color = 0x0000FF;  // colour only, no image
    check(openshow::ui::sceneContent(
              QString::fromStdString(boost::json::serialize(openshow::scene::toJson(imageless))))
              .background,
          "a non-black fill alone counts as a background on air");

    Scene black;  // default: black, no items
    const SceneContent cleared = openshow::ui::sceneContent(
        QString::fromStdString(boost::json::serialize(openshow::scene::toJson(black))));
    check(!cleared.words && !cleared.background,
          "black with no items has nothing on air — the buttons sit disabled");

    const SceneContent garbage = openshow::ui::sceneContent(QStringLiteral("not json {"));
    check(!garbage.words && !garbage.background, "unparseable input reports nothing, loudly");

    // Task 6.21e (review F4): the gradient counts as a background even with
    // a black top colour — before the fix this exact payload reported
    // nothing on air, and the clear button sat disabled over a glowing
    // audience.
    check(openshow::ui::sceneContent(gradientScene(false)).background,
          "a black-to-colour gradient without an image counts as a background");
    check(openshow::ui::sceneContent(gradientScene(true)).background,
          "and so does one riding under a picture");
    // The key's presence is the answer whatever its colour: a black-on-
    // black pair is still a payload whose clear must be reachable (and the
    // flag only ever goes FALSE-after-clear on a payload with the key gone).
    Scene blackPair;
    blackPair.background.gradient = 0u;
    blackPair.background.hasGradient = true;
    check(openshow::ui::sceneContent(
              QString::fromStdString(boost::json::serialize(openshow::scene::toJson(blackPair))))
              .background,
          "a present black-on-black gradient is still a live background definition");
}

void exerciseClears() {
    const QString full = fullSceneJson();

    // Clear slide: words gone, background and provenance untouched, and
    // the result still passes the worker's strict validator.
    const QString wordsGone = openshow::ui::withWordsCleared(full);
    bool ok = false;
    Scene parsed = parseScene(wordsGone, &ok);
    check(ok && wordsGone != full && parsed.items.empty(),
          "clear-slide yields a valid scene with no items");
    check(ok && parsed.background.color == 0x112233 && parsed.background.hasImage &&
              parsed.background.hasGradient && parsed.background.gradient == 0x336699 &&
              parsed.background.assetId == "asset2" &&
              parsed.background.assetPath == "/media/grape.jpg",
          "clear-slide leaves the whole background in place");
    check(ok && parsed.showId == "show7" && parsed.slideId == "slide3",
          "a cleared slide still names its slide");

    // Clear background: colour to black and no image (the reference's
    // plain black with nothing left behind), words untouched.
    const QString backgroundGone = openshow::ui::withBackgroundCleared(full);
    parsed = parseScene(backgroundGone, &ok);
    check(ok && parsed.background.color == openshow::scene::kBlack &&
              !parsed.background.hasImage && !parsed.background.hasGradient &&
              parsed.background.assetId.empty() &&
              parsed.background.assetPath.empty(),
          "clear-background yields the black fill with no image, gradient or asset");
    check(ok && parsed.items.size() == 2 &&
              parsed.items[0].lines == std::vector<std::string>{"First line", "second line"},
          "clear-background leaves every word in place");

    // Task 6.21e (review F4): the gradient is part of the background, so
    // the clear takes it too — the KEY goes, not merely the value, because
    // presence is what the renderer reads as "draw a ramp". Both shapes:
    // the bare black-to-colour canvas, and the gradient under a picture.
    for (bool withImage : {false, true}) {
        const QString graded = gradientScene(withImage);
        const QString gradedGone = openshow::ui::withBackgroundCleared(graded);
        bool gradOk = false;
        const Scene clearedGraded = parseScene(gradedGone, &gradOk);
        const char* which = withImage ? "under a picture" : "on a bare canvas";
        check(gradOk && gradedGone != graded,
              (std::string("clear-background yields a valid scene for a gradient ") + which)
                  .c_str());
        check(gradOk && !clearedGraded.background.hasGradient,
              (std::string("clear-background drops the gradient ") + which).c_str());
        check(!carriesGradientKey(gradedGone),
              (std::string("the cleared payload does not even name the gradient ") + which)
                  .c_str());
        check(gradOk && clearedGraded.background.color == openshow::scene::kBlack &&
                  !clearedGraded.background.hasImage,
              (std::string("and leaves the flat black fill ") + which).c_str());
        check(gradOk && clearedGraded.items.size() == 1 &&
                  clearedGraded.items[0].lines.size() == 1,
              (std::string("with the words left in place ") + which).c_str());
        check(!openshow::ui::sceneContent(gradedGone).background,
              (std::string("and the flag reads the cleared scene as empty ") + which).c_str());
    }

    // Independence means the pair composes in either order onto the same
    // cleared output, and every step is presentable.
    const SceneContent chained = openshow::ui::sceneContent(
        openshow::ui::withBackgroundCleared(wordsGone));
    check(!chained.words && !chained.background, "successive clears compose");
    bool chainedOk = false;
    parseScene(openshow::ui::withWordsCleared(backgroundGone), &chainedOk);
    check(chainedOk, "the other order stays valid on the wire too");

    // Clear all is the scene library's cleared scene, serialized.
    const SceneContent clearedAll = openshow::ui::sceneContent(openshow::ui::clearedSceneJson());
    check(!clearedAll.words && !clearedAll.background, "clear-all reports nothing on air");
    parseScene(openshow::ui::clearedSceneJson(), &chainedOk);
    check(chainedOk, "clear-all is a scene the worker accepts");

    // Non-scenes are refused, not mangled.
    check(openshow::ui::withWordsCleared(QStringLiteral("{{")).isEmpty() &&
              openshow::ui::withBackgroundCleared(QStringLiteral("[]")).isEmpty(),
          "garbage in, empty out — nothing presents garbage");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    exerciseContentFlags();
    exerciseClears();
    if (gFailed) {
        return 1;
    }
    std::printf("scene_edit_tests: all fixtures passed\n");
    return 0;
}
