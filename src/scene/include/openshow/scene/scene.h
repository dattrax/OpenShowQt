/*
 * The compiled render scene (task 2.4): what a slide looks like, with
 * every decision already made.
 *
 * The ownership split (PLAN.md) says the frontend owns the editable
 * documents and the worker owns output — the scene is the boundary.
 * Compilation resolves everything the renderer would otherwise have to
 * ask about: the theme chain (slide override → show default → built-in
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

// Packed 0xRRGGBB. Themes store "#rgb"/"#rrggbb" text; compilation
// parses once so no renderer does.
using Color = std::uint32_t;

inline constexpr Color kWhite = 0xFFFFFFu;
inline constexpr Color kBlack = 0x000000u;

// Font size 0 means "renderer default" — the same convention the
// domain's themes use (task 2.1).
inline constexpr double kRendererDefaultFontSize = 0.0;

// Task 5.5's classification: whether a path's extension names a video
// container (mp4, mov, m4v, mkv, webm, avi, mpg, mpeg, wmv, flv, ogv,
// ts — case-insensitive). compileSlide uses it to mark the background's
// kind without touching the filesystem; the model uses the same function
// (domain::assetPathLooksLikeVideo, which this is) to refuse a video for
// a FOREGROUND image item (task 6.17b), so one name list decides both.
// Everything else — images and the unknown — keeps image semantics; a
// video extension whose file is not a video is a reported decode failure,
// not a silent still. Animated gifs stay images (SkCodec decodes their
// first frame, unchanged from 3.2).
bool assetPathIsVideo(const std::string& path);

// True when `path` is a plain local filesystem reference the renderer can
// open (task 6.8a, review R11). Refused, each with its own reason in *why:
// a URI of any scheme ("data:image/png;base64,...", "http://host/x.png"),
// which would turn a control message into a media-byte payload or a network
// fetch the worker has no business making; an embedded NUL, which truncates
// every C-string API between here and Skia; and any other control byte, which
// no filesystem the project targets uses and which survives copy-paste from
// a browser address bar. *why is cleared on the way in, so a caller can tell
// "accepted" from a reason left over from an earlier call. An EMPTY path is
// accepted here — "no image" is legal — the validator refuses it only when the
// background claims an image. Legitimate absolute and relative local paths,
// with spaces, a colon after the first slash, or Unicode, pass.
bool assetPathIsLocal(const std::string& path, std::string* why = nullptr);

// --- application budgets (task 6.8a, review R11) ---------------------------
//
// How big a SCENE may be: the number an operator can grow just by typing,
// where kMaxMessageBytes only bounds the message wrapped around it. Measured
// rather than guessed, and scene_tests prints it — a representative slide
// serialises at 576 bytes (scene 4 writes the style block on every text
// item), and one pushed to EVERY budget below (64 KiB of text made of the
// characters JSON escapes at their longest, ids, font and path at their
// limits) reaches 400 KiB inside a scene.prepare: 39.1% of the frame
// budget, at depth 5 of kMaxParseDepth. Nothing an operator can author
// is refused by the wire.
inline constexpr std::size_t kMaxSceneItems = 8;    // items on one slide, either kind
inline constexpr std::size_t kMaxItemLines = 256;   // lines in one text layer
inline constexpr std::size_t kMaxLineChars = 1024;  // characters in one line
inline constexpr std::size_t kMaxItemTextChars = 16 * 1024;  // one layer's lines
inline constexpr std::size_t kMaxSceneTextChars = 64 * 1024;  // every layer's text
inline constexpr std::size_t kMaxIdChars = 512;   // showId, slideId, assetId
inline constexpr std::size_t kMaxPathChars = 4096;  // Linux PATH_MAX
inline constexpr std::size_t kMaxFontFamilyChars = 256;

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
// set the media asset is drawn over it (fit rules are 3.x's). The ids
// and path are what compilation resolved: assetId names the model
// asset, assetPath is where its bytes live.
//
// Task 5.5 adds the media kind: isVideo marks the asset as a playing
// background (decode drives frames and the clock; a background video
// survives lyric changes while its identity — assetId, path and kind —
// is unchanged), while isVideo=false keeps the task 3.2 image semantics
// (a still decoded once). The distinction is extension-based at compile
// time — the scene stays filesystem-independent exactly like the image
// background: whether the file exists and decodes stays the renderer's
// problem (and the renderer's reported error, task 5.5).
struct Background {
    // No backdrop supplied by this cue: presentation pixels remain alpha.
    bool transparent = false;
    Color color = kBlack;
    // Second colour of a gradient background (task 6.17c3, review R17):
    // the canvas fills from `color` at the top to `gradient` at the bottom
    // when hasGradient, and paints flat `color` when it does not. The pair
    // arrives from the THEME; the empty theme string compiles to
    // hasGradient=false, so every scene a pre-6.17c3 file describes draws
    // exactly what it always drew.
    Color gradient = kBlack;
    bool hasGradient = false;
    bool hasImage = false;
    bool isVideo = false;  // only meaningful when hasImage
    domain::Id assetId;
    std::string assetPath;
    // How the picture (still or video frame) sits on the canvas (task
    // 6.17c1, review R17) — compiled from the SLIDE, spelled by the domain's
    // one name function, and REQUIRED on the wire: a background whose fit
    // the reader has not validated must not draw. Cover is what every scene
    // before this key existed drew, and what a slide that carries no picture
    // will keep as its standing answer.
    domain::BackgroundFit fit = domain::BackgroundFit::Cover;
};

// One item of a slide's draw stack, compiled for presentation (tasks
// 4.3/6.17b). isImage says which half of the struct means something:
//
//  - a text item keeps its lines (blank strings included — stanza breaks),
//    exactly as authored, plus the resolved appearance. The x/y/width/
//    height box is in authored canvas units (task 4.3): the text wraps at
//    width and is centred — horizontally in the box, vertically in its
//    height. align places the lines inside the box (task 4.4), and
//    lineSpacing is the renderer's line-box multiplier.
//  - an image item names its asset (assetId) and carries the path
//    compileSlide resolved (assetPath), drawn stretched into the same
//    kind of box. Its text fields hold defaults and draw nothing —
//    the wire's closed per-kind key set keeps a hand-built payload from
//    filling them with something the renderer would ignore.
struct SceneItem {
    bool isImage = false;
    std::vector<std::string> lines;
    Color color = kWhite;
    std::string fontFamily;  // "" = platform default
    double fontSize = kRendererDefaultFontSize;
    double x = kDefaultBoxX;
    double y = kDefaultBoxY;
    double width = kDefaultBoxWidth;
    double height = kDefaultBoxHeight;
    std::string align = "center";  // "left" | "center" | "right"
    double lineSpacing = domain::kDefaultLineSpacing;
    // The TS.1 style fields, resolved CONCRETE like every other answer
    // here (task TS.1): the chain above has already chosen, and these
    // defaults are the contract's own — weight 400, upright, the shrink
    // policy, minimum 12, no ink. A scene-3 payload carries none of the
    // keys and lands on exactly these numbers, which is what the worker
    // always drew, so old payloads keep their exact pixels. The records
    // are the domain's own: one sentence describes an effect from the
    // editor to the wire.
    int fontWeight = domain::kDefaultFontWeight;
    std::string fontStyle = "normal";  // "normal" | "italic"
    std::string fitMode = "shrink";    // "shrink" | "fixed"
    double minFontSize = domain::kDefaultMinFontSize;
    domain::TextStroke stroke;   // disabled by default; enabled draws
    domain::TextShadow shadow;
    // Image items only (6.17b): what compileSlide resolved from the
    // model's asset reference, same discipline as the background's pair.
    domain::Id assetId;
    std::string assetPath;
};

// One slide compiled for presentation. showId/slideId are provenance
// for status lines and debug dumps; a cleared/black scene simply has
// no items (PLAN.md: worker restart begins black). items is the draw
// stack: index 0 is painted first and a later item covers an earlier
// one where they overlap.
struct Scene {
    domain::Id showId;
    domain::Id slideId;
    Background background;
    std::vector<SceneItem> items;
};

bool operator==(const Background& a, const Background& b);
bool operator!=(const Background& a, const Background& b);
bool operator==(const SceneItem& a, const SceneItem& b);
bool operator!=(const SceneItem& a, const SceneItem& b);
bool operator==(const Scene& a, const Scene& b);
bool operator!=(const Scene& a, const Scene& b);

// The scene a restart begins on: black, empty, no provenance.
Scene clearedScene();

// Compiles one slide. The full appearance chain is the model's
// (appearanceForSlide: customization → current theme → slide theme → show
// default); a property no layer spoke keeps the built-in default — white
// text on black, platform font, renderer size. A slide's background asset
// becomes the scene's image
// background with the path recorded in the model at compile time —
// whether that file still exists is the renderer's problem, matching
// the model's own stance (task 2.2: save is where bytes are checked).
// Unknown show or slide fails with an error and leaves *out unchanged.
bool compileSlide(const domain::Documents& documents, const domain::Id& showId,
                  const domain::Id& slideId, Scene* out, std::string* error = nullptr);

// --- wire form ---------------------------------------------------------------

// {"scene":4,"showId":...,"slideId":...,"backgroundColor":<int>,
//  "background":{"image":bool,"video":bool,
//                "fit":"cover"|"contain"|"blur","assetId":...,"assetPath":...,
//                "gradient":<int>},
//  "items":[{"kind":"text","lines":[...],"color":<int>,"fontFamily":...,
//            "fontSize":<num>,"x":<num>,"y":<num>,"width":<num>,
//            "height":<num>,"align":"left"|"center"|"right",
//            "lineSpacing":<num>,"fontWeight":<int>,"fontStyle":"normal"|"italic",
//            "fitMode":"shrink"|"fixed","minFontSize":<num>,
//            "stroke":{"enabled":bool,"color":<int>,"width":<num>},
//            "shadow":{"enabled":bool,"color":<int>,"opacity":<num>,
//                      "offsetX":<num>,"offsetY":<num>,"blurSigma":<num>}}
//           ,{"kind":"image","assetId":...,"assetPath":...,
//             "x":<num>,"y":<num>,"width":<num>,"height":<num>}]}
// Colours are packed 0xRRGGBB integers. Serialisation order is fixed.
// "video" (task 5.5) is always written out, and true requires image:true.
// Scene 2 (task 6.17b) names every item's "kind" and gives image items
// the background's asset pair; scene 3 (task 6.17c1) requires the
// background's "fit" — the domain's name for how the picture sits on the
// canvas, mandatory because a payload that leaves it out is a payload the
// reader would have to guess. "gradient" (task 6.17c3, review R17) is the
// exception that proves the rule rather than breaking it: written only when
// the theme names a second background colour, and absent means flat — the
// payload's own history, not a guess. It moves no format number; it is
// protocol MINOR 13's addition, named by "scene.background-gradient.v1".
// The closed key set is what makes that safe in the other direction: a
// worker older than minor 13 refuses a payload carrying "gradient" by name,
// so a gradient is loudly refused, never quietly drawn flat.
// Scene 4 (task TS.1) gives every TEXT item the resolved style block above
// — always written, never defaulted by absence, because a text item whose
// style the reader has not validated must not draw — and the per-kind key
// set stays closed, so style keys on an image item are refused naming
// themselves. Unlike 2→3, this build also READS scene 3: a legacy payload
// carries no style keys and lands on the contract's concrete defaults
// (400/normal/shrink/12/disabled effects), which is exactly what every
// scene-3 worker drew. That keeps one frontend able to drive a worker that
// has not heard of styles; sending a NON-default style to such a worker is
// the frontend's to refuse (capability "scene.text-style.v1", advertised
// from TS.2's renderer), because quietly drawing it unstyled is the silent
// omission the wire has refused since 6.8a.
boost::json::value toJson(const Scene& scene);

// Strict reader for the above; see the header for the contract. Fails
// on: wrong scene format number (only 4, and legacy 3, are read), malformed
// or wrongly-typed fields,
// colours outside 0xRRGGBB, non-finite or negative font sizes, non-finite
// text-box edges, non-positive box width or height, an alignment outside
// left/center/right, a line spacing outside 0.25..8, a "video" flag that
// is not a bool or set without "image" — and, since task 6.8a (review R11),
// anything over an application budget above (items, lines, characters, ids,
// path), a key the format does not define (named in the error), an asset
// reference that is not a local path, an empty asset path behind image:true,
// and a NUL in any string. Since scene 4 (task TS.1): a missing text-item
// style key, a weight outside 100..900 or not an integer, a font style or
// fit mode outside its two words, a minimum size outside 1..4096, an
// effect that is not a whole closed record, and any effect number outside
// its band (width 0..32, opacity 0..1, offsets -256..256, blur 0..64).
//
// The key set is CLOSED on purpose. Minor-version tolerance is a frontend
// rule — an unknown field in a STATUS object costs a viewer nothing — but a
// scene is content the audience must see, so accepting a key this build
// ignores would mean the operator set something that silently never appears
// on air. A format change bumps "scene", which refuses the payload loudly
// instead. The UI's own payloads come from toJson(), so it never trips this.
bool fromJson(const boost::json::value& value, Scene* out, std::string* error = nullptr);

}  // namespace openshow::scene
