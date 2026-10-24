/*
 * The compiled render scene (task 2.4): what a slide looks like, with
 * every decision already made.
 *
 * The ownership split (PLAN.md) says the frontend owns the editable
 * documents and the worker owns output — the scene is the boundary.
 * Compilation resolves everything the renderer would otherwise have to
 * ask about: the style chain (slide override → show default → built-in
 * defaults) is collapsed into concrete colours and font choices, and an
 * image background names its asset and carries the path its bytes live
 * at. A Scene therefore depends on nothing: no Qt, no Vulkan, no files,
 * not even the document model beyond the Id strings it quotes for
 * provenance. Once compiled it is treated as immutable — further edits
 * compile a new Scene; the old one is what any viewer already holds.
 *
 * Scenes are shared across the process boundary as Boost.JSON (the
 * project's one JSON library): toJson()/fromJson() are the wire form
 * scene.prepare will use (2.4b). Serialisation is deterministic — a
 * fixed key order — so equal scenes produce byte-equal payloads, and
 * fromJson validates strictly: a wire scene either fully satisfies the
 * contract below or the load fails naming what is wrong, leaving the
 * target untouched. The worker never trusts a scene it did not compile
 * itself; it trusts this validation instead.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "openshow/domain/documents.h"

namespace openshow::scene {

// Packed 0xRRGGBB. Styles store "#rgb"/"#rrggbb" text; compilation
// parses once so no renderer does.
using Color = std::uint32_t;

inline constexpr Color kWhite = 0xFFFFFFu;
inline constexpr Color kBlack = 0x000000u;

// Font size 0 means "renderer default" — the same convention the
// domain's styles use (task 2.1).
inline constexpr double kRendererDefaultFontSize = 0.0;

// The authored canvas (docs/reference.md §7: every scene is designed
// against 1920x1080 and scaled by width/1920) and the default text box:
// the canvas minus the lyric margins, the region M2/M3 laid text inside.
// The domain's Slide defaults mirror these numbers (kept in sync by
// scene_tests).
inline constexpr double kCanvasWidth = 1920.0;
inline constexpr double kCanvasHeight = 1080.0;
inline constexpr double kDefaultBoxX = 160.0;
inline constexpr double kDefaultBoxY = 100.0;
inline constexpr double kDefaultBoxWidth = 1600.0;
inline constexpr double kDefaultBoxHeight = 880.0;

// The fullscreen backdrop. The colour fills first; when hasImage is
// set the image asset is drawn over it (fit rules are 3.x's). The ids
// and path are what compilation resolved: assetId names the model
// asset, assetPath is where its bytes live.
struct Background {
    Color color = kBlack;
    bool hasImage = false;
    domain::Id assetId;
    std::string assetPath;
};

// One block of slide text with its resolved appearance. lines keeps
// blank strings (stanza breaks), exactly as authored. The x/y/width/height
// box is in authored canvas units (task 4.3): the text wraps at width and
// is centred — horizontally in the box, vertically in its height.
struct TextItem {
    std::vector<std::string> lines;
    Color color = kWhite;
    std::string fontFamily;  // "" = platform default
    double fontSize = kRendererDefaultFontSize;
    double x = kDefaultBoxX;
    double y = kDefaultBoxY;
    double width = kDefaultBoxWidth;
    double height = kDefaultBoxHeight;
};

// One slide compiled for presentation. showId/slideId are provenance
// for status lines and debug dumps; a cleared/black scene simply has
// no items (PLAN.md: worker restart begins black).
struct Scene {
    domain::Id showId;
    domain::Id slideId;
    Background background;
    std::vector<TextItem> items;
};

bool operator==(const Background& a, const Background& b);
bool operator!=(const Background& a, const Background& b);
bool operator==(const TextItem& a, const TextItem& b);
bool operator!=(const TextItem& a, const TextItem& b);
bool operator==(const Scene& a, const Scene& b);
bool operator!=(const Scene& a, const Scene& b);

// The scene a restart begins on: black, empty, no provenance.
Scene clearedScene();

// Compiles one slide. The style resolution chain is the model's
// (styleForSlide: override → show default → none); with no style the
// built-in defaults are white text on black, platform font, renderer
// size. A slide's background asset becomes the scene's image
// background with the path recorded in the model at compile time —
// whether that file still exists is the renderer's problem, matching
// the model's own stance (task 2.2: save is where bytes are checked).
// Unknown show or slide fails with an error and leaves *out unchanged.
bool compileSlide(const domain::Documents& documents, const domain::Id& showId,
                  const domain::Id& slideId, Scene* out, std::string* error = nullptr);

// --- wire form ---------------------------------------------------------------

// {"scene":1,"showId":...,"slideId":...,"backgroundColor":<int>,
//  "background":{"image":bool,"assetId":...,"assetPath":...},
//  "items":[{"lines":[...],"color":<int>,"fontFamily":...,"fontSize":<num>,
//            "x":<num>,"y":<num>,"width":<num>,"height":<num>}]}
// Colours are packed 0xRRGGBB integers. Serialisation order is fixed.
boost::json::value toJson(const Scene& scene);

// Strict reader for the above; see the header for the contract. Fails
// on: wrong scene format number, malformed or wrongly-typed fields,
// colours outside 0xRRGGBB, non-finite or negative font sizes, non-finite
// text-box edges, non-positive box width or height.
bool fromJson(const boost::json::value& value, Scene* out, std::string* error = nullptr);

}  // namespace openshow::scene
