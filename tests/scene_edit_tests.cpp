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
 * indicators light — so they are pinned here too.
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
using openshow::scene::TextItem;
using openshow::ui::SceneContent;

// A scene with every interesting part present: a coloured background
// carrying an image, and two text items. The transforms are judged
// against this full picture.
QString fullSceneJson() {
    Scene scene;
    scene.showId = "show7";
    scene.slideId = "slide3";
    scene.background.color = 0x112233;
    scene.background.hasImage = true;
    scene.background.assetId = "asset2";
    scene.background.assetPath = "/media/grape.jpg";
    scene.items = {
        TextItem{{"First line", "second line"}, 0xFFFFFF, "serif", 64},
        TextItem{{"Doxology"}, 0xFFDD44, "", 48},
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
              !parsed.background.hasImage && parsed.background.assetId.empty() &&
              parsed.background.assetPath.empty(),
          "clear-background yields the black fill with no image or asset");
    check(ok && parsed.items.size() == 2 &&
              parsed.items[0].lines == std::vector<std::string>{"First line", "second line"},
          "clear-background leaves every word in place");

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
