/*
 * Checks the compiled scene type (task 2.4a): the compiler's style
 * resolution and background handling, the immutability of an already
 * compiled scene against later edits, and the strictness of the wire
 * codec that scene.prepare will rely on. Task 6.8a (review R11) added
 * the application budgets (tested on both sides of each boundary), the
 * closed key set, what an asset reference may name, and the measurement
 * proving the largest supported scene fits the transport.
 */

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "openshow/domain/documents.h"
#include "openshow/domain/storage.h"
#include "openshow/protocol/protocol.h"
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
using openshow::domain::kDefaultLineSpacing;
namespace scene = openshow::scene;
namespace proto = openshow::protocol;

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
    Id videoBacked;  // slide with the video asset as background (task 5.5)
    Id asset;
    Id videoAsset;
};

Fixture buildFixture() {
    Fixture fx;
    const Id house =
        fx.docs.createStyle("House", "#ffffff", "#202040", "DejaVu Serif", 64.0, "center",
                            kDefaultLineSpacing);
    const Id loud = fx.docs.createStyle("Loud", "#ff0", "#000", "Impact", 96.0, "right", 1.5);
    fx.asset = fx.docs.addAsset("stage loop.jpg", "/services/svc/assets/asset-loop.jpg");
    fx.videoAsset =
        fx.docs.addAsset("Stage Loop.MP4", "/services/svc/assets/asset-loop.mp4");

    const Id show = fx.docs.createShow("Order");
    fx.docs.setShowDefaultStyle(show, house);
    fx.defaulted = fx.docs.appendSlide(show);
    fx.overridden = fx.docs.appendSlide(show);
    fx.unstyled = fx.docs.appendSlide(show);
    fx.backed = fx.docs.appendSlide(show);
    fx.docs.setSlideStyle(show, fx.overridden, loud);
    fx.docs.setSlideBackground(show, fx.backed, fx.asset);
    fx.docs.setSlideLines(show, fx.backed, {"Background lyric"});
    fx.videoBacked = fx.docs.appendSlide(show);
    fx.docs.setSlideBackground(show, fx.videoBacked, fx.videoAsset);
    fx.docs.setSlideLines(show, fx.videoBacked, {"Video stays"});
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
    check(out.items[0].align == "center" && out.items[0].lineSpacing == kDefaultLineSpacing,
          "a centre-set style compiles to a centred item at the default line box");

    // Override wins over the default; the #rgb short form expands.
    check(scene::compileSlide(fx.docs, fx.show, fx.overridden, &out, &error),
          "an overridden slide compiles");
    check(out.items[0].color == 0xFFFF00u && out.items[0].fontFamily == "Impact" &&
              out.items[0].fontSize == 96.0,
          "the per-slide override wins over the show default");
    check(out.background.color == 0x000000u, "#ff0/#000 short forms expand correctly");
    check(out.items[0].align == "right" && out.items[0].lineSpacing == 1.5,
          "the resolved style's typography (task 4.4) is baked into the text item");

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
              out.items[0].fontSize == scene::kRendererDefaultFontSize &&
              out.items[0].align == "center" &&
              out.items[0].lineSpacing == kDefaultLineSpacing,
          "no style at all yields the built-in defaults");

    // Background asset resolves to id + path.
    check(scene::compileSlide(fx.docs, fx.show, fx.backed, &out, &error),
          "a backed slide compiles");
    check(out.background.hasImage && out.background.assetId == fx.asset &&
              out.background.assetPath == "/services/svc/assets/asset-loop.jpg",
          "the background asset resolves to its id and recorded path");
    check(!out.background.isVideo, "an image-extension asset compiles as a still");

    // Task 5.5: the background's kind is the name's, decided at compile
    // time; case-insensitive, and the wire can say so.
    check(scene::compileSlide(fx.docs, fx.show, fx.videoBacked, &out, &error),
          "a video-backed slide compiles");
    check(out.background.hasImage && out.background.isVideo &&
              out.background.assetId == fx.videoAsset &&
              out.background.assetPath == "/services/svc/assets/asset-loop.mp4",
          "a video-extension asset compiles as a playing background");
    check(scene::assetPathIsVideo("a/b/movie.MKV") && scene::assetPathIsVideo("loop.webm") &&
              !scene::assetPathIsVideo("stage.jpg") && !scene::assetPathIsVideo("note") &&
              !scene::assetPathIsVideo("trailing.") && !scene::assetPathIsVideo(""),
          "assetPathIsVideo classifies extensions, case-insensitively");

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

// Task 6.17a (review R17): the compiler's job with a stack of boxes is to
// hand the renderer the SAME stack — same order, same rectangles, same words
// per box — because the renderer has drawn several items since 4.3 and the
// whole point is that a second box is not a second opinion about layout.
void exerciseMultiBox(Fixture& fx) {
    std::string error;
    const Id show = fx.show;
    const Id slide = fx.docs.appendSlide(show);
    openshow::domain::TextBox title;
    title.lines = {"Title line"};
    title.x = 100.0;
    title.y = 40.0;
    title.width = 800.0;
    title.height = 120.0;
    const Id titleId = fx.docs.addTextBox(show, slide, title, &error);
    openshow::domain::TextBox credit;
    credit.lines = {"small", "credit"};
    credit.x = 1200.0;
    credit.y = 900.0;
    credit.width = 600.0;
    credit.height = 100.0;
    const Id creditId = fx.docs.addTextBox(show, slide, credit, &error);
    check(!titleId.empty() && !creditId.empty(), "the fixture's slide now has three boxes");

    scene::Scene out;
    check(scene::compileSlide(fx.docs, show, slide, &out, &error) && out.items.size() == 3,
          "a three-box slide compiles to three wire items");
    check(out.items[0].x == 160.0 && out.items[1].x == 100.0 && out.items[2].x == 1200.0,
          "the items arrive in the model's DRAW order, each at its own x");
    check(out.items[1].y == 40.0 && out.items[1].width == 800.0 && out.items[1].height == 120.0 &&
              out.items[2].y == 900.0 && out.items[2].width == 600.0,
          "every box's own rectangle travels; none of them inherits a neighbour's");
    check(out.items[1].lines == std::vector<std::string>({"Title line"}) &&
              out.items[2].lines == std::vector<std::string>({"small", "credit"}),
          "words stay in the box they were written in, blank lines and all");
    // Style is per slide today (R17's later slices own per-box typography), so
    // what every box must do is agree: same colour, same face, same typography.
    check(out.items[0].color == out.items[1].color &&
              out.items[1].fontFamily == out.items[2].fontFamily &&
              out.items[1].fontSize == out.items[2].fontSize &&
              out.items[1].align == out.items[2].align &&
              out.items[1].lineSpacing == out.items[2].lineSpacing,
          "the slide's resolved style is applied to every box alike");

    // The wire: three items with three geometries, unchanged through JSON.
    const std::string text = boost::json::serialize(scene::toJson(out));
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(text, parseError);
    scene::Scene readBack;
    check(!parseError && scene::fromJson(parsed.as_object(), &readBack, &error) &&
              readBack == out,
          "a multi-box scene round-trips through the wire unchanged");
    const boost::json::array& wireItems = parsed.as_object().at("items").as_array();
    check(wireItems.size() == 3 &&
              wireItems[2].as_object().at("x").to_number<double>() == 1200.0,
          "the JSON keeps the order and each box's own geometry");

    // A slide drained of boxes is a background, not a broken scene.
    const Id bare = fx.docs.appendSlide(show);
    check(fx.docs.removeTextBox(show, bare, fx.docs.findShow(show)->slides.back().items[0].id,
                                &error),
          "the fixture's bare slide loses its only box");
    scene::Scene words;
    check(scene::compileSlide(fx.docs, show, bare, &words, &error) && words.items.empty(),
          "a slide with no boxes compiles to a scene with no items");
    check(words.background.color == scene::Scene{}.background.color ||
              !words.background.hasImage,
          "and it still carries its background, just no words");

    // The compiler's own ceiling is unreachable because the model caps first;
    // it exists so a slide can never compile into a scene its own codec would
    // refuse. The constants being equal is the load-bearing half, pinned in
    // exerciseBoxConstants(); this is the sentence about which half is real.
    check(openshow::domain::kMaxTextBoxes == scene::kMaxSceneItems &&
              fx.docs.findShow(show)->slides[1].items.size() <= scene::kMaxSceneItems,
          "no slide the model can hold exceeds the scene wire's item budget");
}

void exerciseImmutability(Fixture& fx) {
    std::string error;
    scene::Scene compiled;
    check(scene::compileSlide(fx.docs, fx.show, fx.defaulted, &compiled, &error),
          "compile before the edit");
    const scene::Scene before = compiled;

    // Edit the model every way a live session would.
    fx.docs.setSlideLines(fx.show, fx.defaulted, {"Replaced"});
    fx.docs.createStyle("Newer", "#123456", "#000000", "Newer Font", 12.0, "center",
                        kDefaultLineSpacing);

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
    {  // An item missing its typography — required since task 4.4.
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj["items"].as_array()[0].as_object().erase("align");
        refuses(boost::json::serialize(obj), "'align' must be a string",
                "an item without an alignment refuses");
    }
    {  // An alignment that is none of the renderer's three.
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj["items"].as_array()[0].as_object()["align"] = "middle";
        refuses(boost::json::serialize(obj), "must be \"left\", \"center\" or \"right\"",
                "an unknown alignment refuses");
    }
    {  // Line boxes that collapse into each other or blow the box apart.
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj["items"].as_array()[0].as_object()["lineSpacing"] = 0.1;
        refuses(boost::json::serialize(obj), "'lineSpacing' must be within 0.25..8",
                "a collapsed line box refuses");
        obj["items"].as_array()[0].as_object()["lineSpacing"] = 9.0;
        refuses(boost::json::serialize(obj), "'lineSpacing' must be within 0.25..8",
                "an exploded line box refuses");
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

    // Task 5.5: the background kind crosses the wire, stays byte-stable,
    // and validates strictly.
    scene::Scene videoScene;
    check(scene::compileSlide(fx.docs, fx.show, fx.videoBacked, &videoScene, &error) &&
              videoScene.background.isVideo,
          "compile the video-backed slide for the wire");
    const std::string videoText = boost::json::serialize(scene::toJson(videoScene));
    scene::Scene videoBack;
    check(scene::fromJson(boost::json::parse(videoText), &videoBack, &error) &&
              videoBack == videoScene,
          "a video background survives the JSON round-trip intact");
    check(boost::json::serialize(scene::toJson(videoBack)) == videoText,
          "the video background re-serialises byte-equal");
    {  // Scene 1 payloads that predate the kind read as stills.
        boost::json::object obj = scene::toJson(videoScene).as_object();
        obj["background"].as_object().erase("video");
        scene::Scene legacy;
        check(scene::fromJson(boost::json::parse(boost::json::serialize(obj)), &legacy,
                              &error) &&
                  legacy.background.hasImage && !legacy.background.isVideo,
              "a payload without the kind key reads as a still");
    }
    {  // A non-bool kind refuses.
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj["background"].as_object()["video"] = "yes";
        refuses(boost::json::serialize(obj), "'video' must be a bool",
                "a non-bool kind refuses");
    }
    {  // A kind without a media asset is nonsense.
        boost::json::object obj = scene::toJson(compiled).as_object();
        obj["background"].as_object()["image"] = false;
        obj["background"].as_object()["video"] = true;
        refuses(boost::json::serialize(obj), "'video' requires 'image'",
                "a video without a media asset refuses");
    }
    {  // Kind participates in equality: same id and path, different kind,
       // different background (the session's continuity identity needs it).
        scene::Scene stillCopy = videoScene;
        stillCopy.background.isVideo = false;
        check(stillCopy != videoScene, "kind participates in background equality");
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

// --- task 6.8a (review R11): budgets, a closed key set, media references ---

// The scene's error text, or empty when the payload reads.
std::string readRefused(const boost::json::value& candidate) {
    scene::Scene target;
    std::string error;
    return scene::fromJson(candidate, &target, &error) ? std::string() : error;
}

bool accepted(const boost::json::value& candidate, const char* what) {
    const std::string error = readRefused(candidate);
    if (!error.empty()) {
        std::fprintf(stderr, "  refused: %s\n", error.c_str());
    }
    return check(error.empty(), what);
}

// A scene carrying exactly these lines, one item per entry, built on a compiled
// slide so every field the budgets do NOT talk about is already legal. The
// strings are inserted as views so an embedded NUL survives into the value:
// the point of one of these checks is what a reader does with one.
boost::json::object sceneWithLines(const scene::Scene& shape,
                                   const std::vector<std::vector<std::string>>& perItem) {
    boost::json::object root = scene::toJson(shape).as_object();
    const boost::json::object templateItem =
        root.at("items").as_array().at(0).as_object();
    boost::json::array items;
    for (const auto& lines : perItem) {
        boost::json::object item = templateItem;
        boost::json::array jsonLines;
        for (const std::string& line : lines) {
            jsonLines.push_back(boost::json::value(
                boost::json::string_view(line.data(), line.size())));
        }
        item["lines"] = std::move(jsonLines);
        items.push_back(std::move(item));
    }
    root["items"] = std::move(items);
    return root;
}

// The scene's shape on the wire: one more container level than the message that
// wraps it. Counted here rather than trusted from a comment, because the
// transport's parse-depth ceiling has to sit above it.
std::size_t depthOf(const boost::json::value& value) {
    std::size_t deepest = 0;
    if (value.is_object()) {
        for (const auto& entry : value.as_object()) {
            deepest = std::max(deepest, depthOf(entry.value()));
        }
        return 1 + deepest;
    }
    if (value.is_array()) {
        for (const auto& element : value.as_array()) {
            deepest = std::max(deepest, depthOf(element));
        }
        return 1 + deepest;
    }
    return 0;
}

void exerciseBudgets(const Fixture& fx) {
    std::string error;
    scene::Scene shape;
    if (!check(scene::compileSlide(fx.docs, fx.show, fx.backed, &shape, &error),
               "compile the slide the budget fixtures are built on")) {
        return;
    }
    const auto linesOf = [](std::size_t count, std::size_t chars) {
        return std::vector<std::string>(count, std::string(chars, 'a'));
    };

    // Every budget is a boundary, so each is tested on BOTH sides: a scene at
    // the limit is supported content and must read (otherwise the number is a
    // smaller budget than the one stated), and one past it is refused naming
    // the number it broke.
    accepted(scene::toJson(shape), "an ordinary scene reads");

    check(readRefused(boost::json::value(sceneWithLines(
              shape, std::vector<std::vector<std::string>>(scene::kMaxSceneItems,
                                                           linesOf(1, 4)))))
              .empty(),
          "a scene at the item budget reads");
    check(readRefused(boost::json::value(sceneWithLines(
              shape, std::vector<std::vector<std::string>>(scene::kMaxSceneItems + 1,
                                                           linesOf(1, 4)))))
              .find("text items is over the limit") != std::string::npos,
          "one item past the budget refuses naming the limit");

    {  // Lines in one layer.
        std::vector<std::vector<std::string>> atBudget(
            1, linesOf(scene::kMaxItemLines, 4));
        accepted(boost::json::value(sceneWithLines(shape, atBudget)),
                 "a layer at the line budget reads");
        atBudget[0].push_back("one too far");
        check(readRefused(boost::json::value(sceneWithLines(shape, atBudget)))
                  .find("lines are over the limit") != std::string::npos,
              "one line past the budget refuses naming the limit");
    }
    {  // Characters in one line: the lyric a slide actually shows.
        std::vector<std::vector<std::string>> atBudget(1, linesOf(1, scene::kMaxLineChars));
        accepted(boost::json::value(sceneWithLines(shape, atBudget)),
                 "a line at the character budget reads");
        atBudget[0][0] += 'z';
        check(readRefused(boost::json::value(sceneWithLines(shape, atBudget)))
                  .find("characters is over the limit") != std::string::npos,
              "one character past the line budget refuses naming the limit");
    }
    {  // All the text of one layer, then of the whole scene.
        const std::size_t linesPerItem = scene::kMaxItemTextChars / scene::kMaxLineChars;
        std::vector<std::vector<std::string>> oneItemOnly(
            1, linesOf(linesPerItem, scene::kMaxLineChars));
        accepted(boost::json::value(sceneWithLines(shape, oneItemOnly)),
                 "a layer at its own text budget reads");
        oneItemOnly[0].push_back(std::string(1, 'z'));
        check(readRefused(boost::json::value(sceneWithLines(shape, oneItemOnly)))
                  .find("item's text exceeds") != std::string::npos,
              "one character past a layer's text budget refuses naming the limit");

        const std::size_t itemsForSceneTotal =
            scene::kMaxSceneTextChars / scene::kMaxItemTextChars;
        std::vector<std::vector<std::string>> wholeScene(
            itemsForSceneTotal, linesOf(linesPerItem, scene::kMaxLineChars));
        accepted(boost::json::value(sceneWithLines(shape, wholeScene)),
                 "a scene at the whole-slide text budget reads");
        wholeScene.push_back(linesOf(1, 1));
        check(readRefused(boost::json::value(sceneWithLines(shape, wholeScene)))
                  .find("slide's text exceeds") != std::string::npos,
              "one character past the scene's text budget refuses naming the limit");
    }
    {  // Ids, font family, path: the strings that are not authored content.
        auto withString = [&](bool inBackground, const char* key, std::size_t chars) {
            boost::json::object root = scene::toJson(shape).as_object();
            boost::json::object& holder =
                inBackground ? root["background"].as_object() : root;
            holder[key] = std::string(chars, 'q');
            return boost::json::value(root);
        };
        accepted(withString(false, "showId", scene::kMaxIdChars),
                 "a show id at its budget reads");
        check(readRefused(withString(false, "showId", scene::kMaxIdChars + 1))
                  .find("over the limit of") != std::string::npos,
              "a show id one character over refuses naming the limit");
        accepted(withString(true, "assetPath", scene::kMaxPathChars),
                 "a path at its budget reads");
        check(readRefused(withString(true, "assetPath", scene::kMaxPathChars + 1))
                  .find("over the limit of") != std::string::npos,
              "a path one character over refuses naming the limit");

        boost::json::object fontsRoot = scene::toJson(shape).as_object();
        fontsRoot["items"].as_array()[0].as_object()["fontFamily"] =
            std::string(scene::kMaxFontFamilyChars, 'f');
        accepted(boost::json::value(fontsRoot), "a font family at its budget reads");
        fontsRoot["items"].as_array()[0].as_object()["fontFamily"] =
            std::string(scene::kMaxFontFamilyChars + 1, 'f');
        check(readRefused(boost::json::value(fontsRoot)).find("over the limit of") !=
                  std::string::npos,
              "a font family one character over refuses naming the limit");
    }
    {  // A NUL inside a string: std::string would carry it, every C API past
       // here would stop short of it, and the file drawn is not the file named.
        check(readRefused(boost::json::value(sceneWithLines(
                  shape, std::vector<std::vector<std::string>>(1, linesOf(1, 4)))))
                  .empty(),
              "the fixture used for NUL checks reads before the NUL");
        const std::string withNul = std::string("verse") + '\0' + "one";
        check(readRefused(boost::json::value(sceneWithLines(
                  shape, std::vector<std::vector<std::string>>(1, {withNul}))))
                  .find("NUL") != std::string::npos,
              "a NUL inside a line refuses");
        boost::json::object root = scene::toJson(shape).as_object();
        root["slideId"] = boost::json::value(boost::json::string_view(withNul.data(),
                                                                     withNul.size()));
        check(readRefused(boost::json::value(root)).find("NUL") != std::string::npos,
              "a NUL inside an id refuses");
        root = scene::toJson(shape).as_object();
        const std::string twoPaths = std::string("/tmp/good.png") + '\0' + "evil.png";
        root["background"].as_object()["assetPath"] =
            boost::json::value(boost::json::string_view(twoPaths.data(), twoPaths.size()));
        check(readRefused(boost::json::value(root)).find("NUL") != std::string::npos,
              "a NUL inside a path refuses, and does not open the truncated file");
    }
    {  // Unknown keys. A scene is the audience's content: a field this build
       // does not read is a field that silently never appears on air, so it is
       // refused at every level that has a defined shape.
        boost::json::object root = scene::toJson(shape).as_object();
        root["backgroundColour"] = 0x000000;
        check(readRefused(boost::json::value(root)).find("has no field") != std::string::npos,
              "a near-miss key at the top level refuses");
        root = scene::toJson(shape).as_object();
        root["background"].as_object()["asset"] = "/tmp/x.png";
        check(readRefused(boost::json::value(root)).find("'background' has no field") !=
                  std::string::npos,
              "an unknown key inside the background refuses naming where");
        root = scene::toJson(shape).as_object();
        root["items"].as_array()[0].as_object()["lineSpacingx"] = 1.0;
        check(readRefused(boost::json::value(root)).find("text item has no field") !=
                  std::string::npos,
              "an unknown key inside an item refuses naming where");
        root = scene::toJson(shape).as_object();
        root["items"].as_array()[0].as_object()[std::string(9000, 'u')] = 1.0;
        const std::string hugeKey = readRefused(boost::json::value(root));
        check(!hugeKey.empty() && hugeKey.size() < 400,
              "a 9000-byte unknown key is reported by a sentence, not a copy");
        check(hugeKey.find("...") != std::string::npos, "and the quoted key says it was cut");
        // The keys this format does define all stay accepted.
        accepted(boost::json::value(scene::toJson(shape)),
                 "the key set a compiled scene carries is the closed set");
    }
}

void exerciseAssetReferences(const Fixture& fx) {
    std::string error;
    scene::Scene shape;
    if (!scene::compileSlide(fx.docs, fx.show, fx.backed, &shape, &error)) {
        check(false, "compile the slide the reference fixtures are built on");
        return;
    }

    // What a media reference may be: a local file the renderer can open.
    for (const char* path : {"/home/user/shows/stage loop.jpg",
                             "assets/π-χ.jpg",
                             "./backdrop.png",
                             "/tmp/a b.png",
                             "dir/sub:board.png"}) {
        std::string why = "a reason from somewhere else";
        check(scene::assetPathIsLocal(path, &why), "a local path is a supported reference");
        check(why.empty(), "and a supported path leaves no reason behind");
    }
    {  // The out-param is written on every path, so a caller cannot mistake a
       // stale reason for the answer about this path.
        std::string why = "a reason from somewhere else";
        check(scene::assetPathIsLocal("/shows/backdrop.png", &why) && why.empty(),
              "an accepted path clears the reason it was handed");
    }
    check(scene::assetPathIsLocal("", nullptr), "an empty path is not an invalid reference");

    struct Refusal {
        const char* path;
        const char* expectIn;
    };
    for (const Refusal refusal : {Refusal{"data:image/png;base64,iVBORw0KGgo=", "data"},
                                  Refusal{"DATA:image/png;base64,iVBORw0KGgo=", "data"},
                                  Refusal{"http://host/backdrop.png", "http"},
                                  Refusal{"HTTPS://host/backdrop.png", "https"},
                                  Refusal{"file:///tmp/backdrop.png", "file"},
                                  Refusal{"weird:name/inside-a-directory.png", "weird"}}) {
        std::string why;
        const bool refused = !scene::assetPathIsLocal(refusal.path, &why);
        if (!refused) {
            std::fprintf(stderr, "  accepted as local: %s\n", refusal.path);
        }
        check(refused, "a media reference that is not a local file is refused");
        check(!why.empty(), "and the refusal says why");
        if (refusal.expectIn[0] != '\0') {
            check(why.find(refusal.expectIn) != std::string::npos,
                  "and names the scheme it refused");
        }
    }
    {  // Control bytes never survive into an open(): they truncate the C API
       // every path goes through, and a NUL is two filenames in one.
        const std::string nul = std::string("/tmp/a.png") + '\0' + "b.png";
        std::string why;
        check(!scene::assetPathIsLocal(nul, &why) && why.find("NUL") != std::string::npos,
              "a NUL in a path is refused as a NUL");
        check(!scene::assetPathIsLocal("/tmp/a\nb.png", &why) &&
                  why.find("control") != std::string::npos,
              "and so is any other control byte");
    }
    {  // A data: URI is a media-byte PAYLOAD in a control message: refused
       // whole, whatever the image flag claims, and before the wash that
       // discards asset fields for a background that claims no image.
        boost::json::object root = scene::toJson(shape).as_object();
        root["background"].as_object()["assetPath"] = "data:image/png;base64,iVBOR";
        root["background"].as_object()["image"] = false;
        const std::string refused = readRefused(boost::json::value(root));
        check(refused.find("local files only") != std::string::npos,
              "a data: background refuses even with image:false");
        root["background"].as_object()["assetPath"] = "https://host/x.png";
        check(readRefused(boost::json::value(root)).find("local files only") !=
                  std::string::npos,
              "and so does one that would be fetched over the network");
        // The legitimate case the review insisted must keep working.
        root["background"].as_object()["assetPath"] = "/services/svc/assets/loop.jpg";
        root["background"].as_object()["image"] = true;
        accepted(boost::json::value(root), "a local asset path on air still reads");
    }
    {  // Claiming an image with nowhere to read it from is a mistake the
       // operator has to hear about, not a black rectangle.
        boost::json::object root = scene::toJson(shape).as_object();
        root["background"].as_object()["assetPath"] = "";
        root["background"].as_object()["image"] = true;
        check(readRefused(boost::json::value(root)).find("requires an 'assetPath'") !=
                  std::string::npos,
              "image:true with an empty path refuses");
    }
}

void exerciseMeasuredSizes(const Fixture& fx) {
    std::string error;
    scene::Scene compiled;
    if (!check(scene::compileSlide(fx.docs, fx.show, fx.defaulted, &compiled, &error),
               "compile the representative slide for measurement")) {
        return;
    }
    const std::size_t representative = boost::json::serialize(scene::toJson(compiled)).size();

    // The largest scene this build SUPPORTS, at every budget at once, with the
    // text made of the characters JSON escapes at their longest (a control byte
    // becomes six bytes) and every id, font and path at its limit. This is the
    // number the 1 MiB frame has to hold, measured rather than assumed: if a
    // future budget grows the model past the transport, this is where it fails.
    scene::Scene worst = compiled;
    const std::size_t linesPerItem = scene::kMaxItemTextChars / scene::kMaxLineChars;
    worst.showId = std::string(scene::kMaxIdChars, 's');
    worst.slideId = std::string(scene::kMaxIdChars, 'd');
    worst.background.assetId = std::string(scene::kMaxIdChars, 'a');
    worst.background.assetPath = "/worst/" + std::string(scene::kMaxPathChars - 7, 'p');
    worst.items.clear();
    for (std::size_t item = 0; item < scene::kMaxSceneItems; ++item) {
        scene::TextItem text;
        text.fontFamily = std::string(scene::kMaxFontFamilyChars, 'f');
        for (std::size_t line = 0; line < scene::kMaxItemLines; ++line) {
            // A layer's own text budget caps how many of its lines can be
            // full, and the whole-scene budget caps how many layers can carry
            // text at all; the remaining lines stay empty, which is what a real
            // slide looks like too.
            const bool insideSceneBudget =
                item * scene::kMaxItemTextChars + (line + 1) * scene::kMaxLineChars <=
                scene::kMaxSceneTextChars;
            const bool full = line < linesPerItem && insideSceneBudget;
            text.lines.push_back(full ? std::string(scene::kMaxLineChars, '\x01')
                                      : std::string());
        }
        text.color = 0x000000;
        text.fontSize = 64.0;
        text.x = 0.0;
        text.y = 0.0;
        text.width = scene::kCanvasWidth;
        text.height = scene::kCanvasHeight;
        text.align = "center";
        text.lineSpacing = 1.2;
        worst.items.push_back(std::move(text));
    }
    const boost::json::value worstJson = scene::toJson(worst);
    const std::size_t worstBytes = boost::json::serialize(worstJson).size();
    const std::size_t depth = depthOf(boost::json::value(
        boost::json::object{{"params", worstJson}}));

    // Every supported scene has to fit the frame the transport refuses to
    // build, with room to spare: the message adds the envelope around it.
    const std::size_t asMessage =
        boost::json::serialize(boost::json::value(proto::makeRequest(
            1, "scene.prepare", boost::json::object{{"scene", worstJson}})))
            .size();
    check(asMessage < proto::kMaxMessageBytes,
          "the largest supported scene fits the frame budget with room to spare");
    check(depth <= proto::kMaxParseDepth,
          "the largest supported scene nests inside the parse-depth ceiling");
    check(readRefused(boost::json::value(worstJson)).empty(),
          "and the largest supported scene is content the reader accepts");

    std::printf("   measured: representative scene %zu bytes, largest supported scene %zu "
                "bytes (%zu inside a scene.prepare), frame budget %u — %.1f%% of it, "
                "depth %zu of %zu\n",
                representative, worstBytes, asMessage, proto::kMaxMessageBytes,
                100.0 * static_cast<double>(asMessage) / proto::kMaxMessageBytes, depth,
                proto::kMaxParseDepth);
}

}  // namespace

void exerciseBoxConstants() {
    // The domain's defaults and the scene library's constants are one promise
    // written twice (the domain cannot include scene.h): a fresh slide's box
    // must be exactly the scene's default text box...
    const openshow::domain::TextBox fresh;
    check(fresh.x == scene::kDefaultBoxX && fresh.y == scene::kDefaultBoxY &&
              fresh.width == scene::kDefaultBoxWidth &&
              fresh.height == scene::kDefaultBoxHeight,
          "a fresh Slide carries the scene library's default text box");
    // ...and the number of boxes a slide may hold must be the number a scene
    // may carry (task 6.17a). Two libraries, one limit, stated twice because
    // neither can include the other's header: if these ever disagree, a model
    // can hold a slide its own wire format refuses, which is a failure an
    // operator meets while presenting rather than while editing.
    check(openshow::domain::kMaxTextBoxes == scene::kMaxSceneItems,
          "a slide's box cap IS the scene wire's item cap");
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
    exerciseMultiBox(fx);
    exerciseImmutability(fx);
    exerciseWireRoundTrip(fx);
    exerciseBudgets(fx);
    exerciseAssetReferences(fx);
    exerciseMeasuredSizes(fx);
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
