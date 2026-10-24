/*
 * Checks the compiled scene type (task 2.4a): the compiler's style
 * resolution and background handling, the immutability of an already
 * compiled scene against later edits, and the strictness of the wire
 * codec that scene.prepare will rely on. Non-zero exit so ctest
 * reports failures.
 */

#include <cstdio>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "openshow/domain/documents.h"
#include "openshow/domain/storage.h"
#include "openshow/scene/demo.h"
#include "openshow/scene/scene.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::domain::Documents;
using openshow::domain::Id;
namespace scene = openshow::scene;

// A small service exercising every resolution branch: one slide under a
// show default, one under a per-slide override, one with no style at
// all, and one carrying a background asset.
struct Fixture {
    Documents docs;
    Id show;      // the show carrying the default style
    Id defaulted;   // slide under the show default style
    Id overridden;  // slide with its own override
    Id unstyled;    // slide with no style anywhere
    Id backed;      // slide with the asset as background
    Id asset;
};

Fixture buildFixture() {
    Fixture fx;
    const Id house =
        fx.docs.createStyle("House", "#ffffff", "#202040", "DejaVu Serif", 64.0);
    const Id loud = fx.docs.createStyle("Loud", "#ff0", "#000", "Impact", 96.0);
    fx.asset = fx.docs.addAsset("stage loop.jpg", "/services/svc/assets/asset-loop.jpg");

    const Id show = fx.docs.createShow("Order");
    fx.docs.setShowDefaultStyle(show, house);
    fx.defaulted = fx.docs.appendSlide(show);
    fx.overridden = fx.docs.appendSlide(show);
    fx.unstyled = fx.docs.appendSlide(show);
    fx.backed = fx.docs.appendSlide(show);
    fx.docs.setSlideStyle(show, fx.overridden, loud);
    fx.docs.setSlideBackground(show, fx.backed, fx.asset);
    fx.docs.setSlideLines(show, fx.backed, {"Background lyric"});
    fx.docs.setSlideLines(show, fx.defaulted, {"Come, all people", "", "Sing"});

    // The unstyled slide lives in a show without a default style.
    const Id bare = fx.docs.createShow("Bare");
    fx.unstyled = fx.docs.appendSlide(bare);
    fx.docs.setSlideLines(bare, fx.unstyled, {"Plain"});
    fx.show = show;
    return fx;
}

void exerciseCompile(const Fixture& fx) {
    std::string error;
    scene::Scene out;

    // Show default resolution: House colours and font.
    check(scene::compileSlide(fx.docs, fx.show, fx.defaulted, &out, &error),
          "a defaulted slide compiles");
    check(out.items.size() == 1 && out.items[0].color == 0xFFFFFFu &&
              out.items[0].fontFamily == "DejaVu Serif" && out.items[0].fontSize == 64.0,
          "the show default style is baked into the text item");
    check(out.background.color == 0x202040u && !out.background.hasImage,
          "the resolved background colour fills, no image");
    check(out.showId == fx.show && out.slideId == fx.defaulted, "provenance ids travel");
    check(out.items[0].lines.size() == 3 && out.items[0].lines[1].empty(),
          "lines keep their blank stanza break");

    // Override wins over the default; the #rgb short form expands.
    check(scene::compileSlide(fx.docs, fx.show, fx.overridden, &out, &error),
          "an overridden slide compiles");
    check(out.items[0].color == 0xFFFF00u && out.items[0].fontFamily == "Impact" &&
              out.items[0].fontSize == 96.0,
          "the per-slide override wins over the show default");
    check(out.background.color == 0x000000u, "#ff0/#000 short forms expand correctly");

    // No style anywhere: the built-in white on black.
    // find the Bare show: the unstyled slide's show
    Id bareId;
    for (const Id& id : fx.docs.showIds()) {
        for (const openshow::domain::Slide& slide : fx.docs.findShow(id)->slides) {
            if (slide.id == fx.unstyled) {
                bareId = id;
            }
        }
    }
    check(scene::compileSlide(fx.docs, bareId, fx.unstyled, &out, &error),
          "an unstyled slide compiles");
    check(out.items[0].color == scene::kWhite &&
              out.background.color == scene::kBlack &&
              out.items[0].fontFamily.empty() &&
              out.items[0].fontSize == scene::kRendererDefaultFontSize,
          "no style at all yields the built-in defaults");

    // Background asset resolves to id + path.
    check(scene::compileSlide(fx.docs, fx.show, fx.backed, &out, &error),
          "a backed slide compiles");
    check(out.background.hasImage && out.background.assetId == fx.asset &&
              out.background.assetPath == "/services/svc/assets/asset-loop.jpg",
          "the background asset resolves to its id and recorded path");

    // Refusals name the problem and leave the target alone.
    scene::Scene keeper;
    keeper.showId = "keepme";
    error.clear();
    check(!scene::compileSlide(fx.docs, "show-none", fx.defaulted, &keeper, &error) &&
              keeper.showId == "keepme" && !error.empty(),
          "an unknown show refuses and leaves the target alone");
    error.clear();
    check(!scene::compileSlide(fx.docs, fx.show, "slide-none", &keeper, &error) &&
              keeper.showId == "keepme" && !error.empty(),
          "an unknown slide refuses and leaves the target alone");
}

void exerciseImmutability(Fixture& fx) {
    std::string error;
    scene::Scene compiled;
    check(scene::compileSlide(fx.docs, fx.show, fx.defaulted, &compiled, &error),
          "compile before the edit");
    const scene::Scene before = compiled;

    // Edit the model every way a live session would.
    fx.docs.setSlideLines(fx.show, fx.defaulted, {"Replaced"});
    fx.docs.createStyle("Newer", "#123456", "#000000", "Newer Font", 12.0);

    check(compiled == before, "an already compiled scene does not follow later edits");
    scene::Scene fresh;
    check(scene::compileSlide(fx.docs, fx.show, fx.defaulted, &fresh, &error) &&
              fresh.items[0].lines.size() == 1 && fresh.items[0].lines[0] == "Replaced" &&
              fresh != before,
          "recompiling after an edit yields a new scene");
}

void exerciseWireRoundTrip(const Fixture& fx) {
    std::string error;
    scene::Scene compiled;
    check(scene::compileSlide(fx.docs, fx.show, fx.backed, &compiled, &error),
          "compile the backed slide for the wire");

    const std::string text = boost::json::serialize(scene::toJson(compiled));
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(text, parseError);
    scene::Scene readBack;
    check(!parseError && scene::fromJson(parsed, &readBack, &error) && readBack == compiled,
          "a compiled scene survives the JSON round-trip intact");

    const std::string again = boost::json::serialize(scene::toJson(readBack));
    check(again == text, "serialisation is deterministic (byte-equal payloads)");

    // Refusals: each names its cause and leaves the target untouched.
    const auto refuses = [&](const std::string& tampered, const char* expectIn,
                             const char* what) {
        boost::system::error_code pe;
        const boost::json::value value = boost::json::parse(tampered, pe);
        scene::Scene keeper;
        keeper.showId = "keepme";
        error.clear();
        check(!pe && !scene::fromJson(value, &keeper, &error) &&
                  keeper.showId == "keepme" && error.find(expectIn) != std::string::npos,
              what);
    };
    const auto mutate = [&](const char* key, const boost::json::value& replacement) {
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj[key] = replacement;
        return boost::json::serialize(obj);
    };
    refuses(mutate("scene", 2), "unsupported scene format", "a future scene format refuses");
    refuses(boost::json::serialize(7), "top level must be an object",
            "a bare number refuses");
    refuses(mutate("backgroundColor", 0x1000000), "outside 0xRRGGBB",
            "a colour beyond 24 bits refuses");
    refuses(mutate("items", "none"), "'items' must be an array", "non-array items refuse");

    {  // An item whose fontSize is a string.
        boost::json::object obj = scene::toJson(compiled).as_object();
        boost::json::array& items = obj["items"].as_array();
        items[0].as_object()["fontSize"] = "big";
        refuses(boost::json::serialize(obj), "'fontSize' must be a number",
                "a non-numeric font size refuses");
    }
    {  // An item missing its authored box — required since task 4.3.
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj["items"].as_array()[0].as_object().erase("x");
        refuses(boost::json::serialize(obj), "'x' must be a number",
                "an item without a text box refuses");
    }
    {  // A box with nothing in it can neither wrap at nor centre in it.
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj["items"].as_array()[0].as_object()["width"] = 0.0;
        refuses(boost::json::serialize(obj), "'width' must be positive",
                "a zero-width text box refuses");
    }
    {  // An item holding a non-string line.
        boost::json::object obj = scene::toJson(compiled).as_object();
        boost::json::array& items = obj["items"].as_array();
        boost::json::array& lines = items[0].as_object()["lines"].as_array();
        lines[0] = 42;
        refuses(boost::json::serialize(obj), "holds a non-string",
                "a numeric line refuses");
    }

    // background.image false washes asset fields: a hand-built payload
    // claiming no image compiles to no image.
    {
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj["background"].as_object()["image"] = false;
        boost::system::error_code pe;
        scene::Scene readBack;
        check(scene::fromJson(boost::json::parse(boost::json::serialize(obj), pe), &readBack,
                              &error) &&
                  !readBack.background.hasImage && readBack.background.assetId.empty(),
              "image=false washes the asset fields on read");
    }
}

void exerciseClearedScene() {
    const scene::Scene black = scene::clearedScene();
    check(black.items.empty() && black.background.color == scene::kBlack &&
              !black.background.hasImage && black.showId.empty(),
          "the cleared scene is black, empty and anonymous");
    std::string error;
    scene::Scene readBack;
    check(scene::fromJson(scene::toJson(black), &readBack, &error) && readBack == black,
          "the cleared scene crosses the wire intact");
}

void exerciseDemoFixture() {
    const Documents docs = scene::demoDocuments();
    check(scene::demoSlideCount(docs) == 4, "the demo fixture carries four slides");

    std::string error;
    bool allCompile = true;
    bool distinct = true;
    scene::Scene previous;
    const std::size_t count = scene::demoSlideCount(docs);
    for (std::size_t i = 0; i < count; ++i) {
        scene::Scene compiled;
        allCompile = allCompile && scene::compileDemoSlide(docs, i, &compiled, &error);
        // Every slide of the fixture sits on the #14161f backdrop.
        allCompile = allCompile && compiled.background.color == 0x14161Fu;
        if (i > 0) {
            distinct = distinct && !(compiled == previous);
        }
        previous = compiled;
    }
    check(allCompile, "every demo slide compiles");
    check(distinct, "the demo slides compile to distinct scenes");

    // The title card resolves its override, the stanza the show default,
    // the interlude is empty — the three resolution shapes in one fixture.
    scene::Scene title;
    scene::Scene stanza;
    scene::Scene interlude;
    check(scene::compileDemoSlide(docs, 0, &title, &error) &&
              scene::compileDemoSlide(docs, 1, &stanza, &error) &&
              scene::compileDemoSlide(docs, 3, &interlude, &error),
          "the fixture's three slide kinds compile");
    check(title.items[0].fontSize == 110.0 && stanza.items[0].fontSize == 72.0,
          "the title override beats the show default of the stanza");
    check(stanza.items[0].lines.size() == 3 && stanza.items[0].lines[1].empty(),
          "the stanza keeps its blank middle line");
    check(interlude.items.size() == 1 && interlude.items[0].lines.empty(),
          "the interlude compiles to a backdrop with no text");
    check(!scene::compileDemoSlide(docs, count, &interlude, &error) && !error.empty(),
          "an out-of-range demo slide refuses with an error");

    // Wire round-trip of the picture the UI will actually prepare.
    const std::string payload = boost::json::serialize(scene::toJson(title));
    boost::system::error_code parseError;
    scene::Scene readBack;
    check(scene::fromJson(boost::json::parse(payload, parseError), &readBack, &error) &&
              readBack == title,
          "the demo title scene crosses the wire intact");
}

}  // namespace

void exerciseBoxConstants() {
    // The domain's Slide defaults and the scene library's constants are
    // one promise written twice (the domain cannot include scene.h):
    // a fresh slide must carry exactly the scene's default text box...
    const openshow::domain::Slide fresh;
    check(fresh.x == scene::kDefaultBoxX && fresh.y == scene::kDefaultBoxY &&
              fresh.width == scene::kDefaultBoxWidth &&
              fresh.height == scene::kDefaultBoxHeight,
          "a fresh Slide carries the scene library's default text box");
    // ...and that box is the canvas minus its margins, centred: the region
    // M2/M3 drew text in, so untouched slides keep drawing identically.
    check(scene::kDefaultBoxX + scene::kDefaultBoxWidth / 2.0 ==
              scene::kCanvasWidth / 2.0 &&
              scene::kDefaultBoxY + scene::kDefaultBoxHeight / 2.0 ==
                  scene::kCanvasHeight / 2.0,
          "the default text box is centred on the authored canvas");
}

int main() {
    Fixture fx = buildFixture();
    exerciseCompile(fx);
    exerciseImmutability(fx);
    exerciseWireRoundTrip(fx);
    exerciseClearedScene();
    exerciseDemoFixture();
    exerciseBoxConstants();

    if (gFailed) {
        std::fprintf(stderr, "scene_tests: FAILED\n");
        return 1;
    }
    std::printf("ok: compiled scenes exercise, round-trip and refuse\n");
    return 0;
}
