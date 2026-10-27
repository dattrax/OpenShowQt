/*
 * Scene compilation and the wire codec — see scene.h for the contract.
 */

#include "openshow/scene/scene.h"

#include <cmath>
#include <initializer_list>
#include <string>
#include <string_view>

namespace openshow::scene {

namespace {

constexpr int kSceneFormat = 2;

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

// Cuts a quoted key or value out of an error message (task 6.8a, review R11):
// "this key is enormous" must not be reported by a reply that is itself
// enormous, because the error travels the same wire as the payload. The cut
// steps back off a continuation byte so a UTF-8 character is never sliced in
// half. The scene library links nothing but the domain and Boost.JSON, so
// this stays local rather than pulling in the protocol layer for it.
std::string clipForError(std::string_view text, std::size_t maxBytes = 64) {
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    if (maxBytes < 3) {
        return std::string();  // not even an ellipsis fits: quote nothing at all
    }
    std::size_t cut = maxBytes > 3 ? maxBytes - 3 : 0;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return std::string(text.substr(0, cut)) + "...";
}

// The one hex-colour parser, matching the domain's accepted syntax
// exactly: "#rgb" and "#rrggbb" (createStyle has already vetted model
// colours; fromJson re-vets anything arriving over the socket).
bool parseColor(const std::string& text, Color* out) {
    if (text.size() != 4 && text.size() != 7) {
        return false;
    }
    if (text.front() != '#') {
        return false;
    }
    Color value = 0;
    for (size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        Color digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<Color>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<Color>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<Color>(c - 'A' + 10);
        } else {
            return false;
        }
        value = value * 16 + digit;
    }
    if (text.size() == 4) {
        // #abc -> #aabbcc, the CSS expansion.
        const Color r = (value >> 8) & 0xF;
        const Color g = (value >> 4) & 0xF;
        const Color b = value & 0xF;
        value = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    }
    *out = value;
    return true;
}

const boost::json::value* field(const boost::json::object& obj, const char* key) {
    const auto found = obj.find(key);
    return found == obj.end() ? nullptr : &found->value();
}

// Every string the wire carries is bounded and NUL-free (task 6.8a, R11).
// The length is what keeps a slide inside the frame budget; the NUL rule is
// smaller and nastier — the path travels into open(), a std::string that
// stops at the first NUL does not name the file the operator chose, and Skia
// is handed a (pointer, size) pair it will happily draw the tail of.
bool wantString(const boost::json::object& obj, const char* key, std::string* out,
                std::size_t maxChars, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_string()) {
        fail(error, std::string("scene: '") + key + "' must be a string");
        return false;
    }
    std::string text(value->as_string());
    if (text.find('\0') != std::string::npos) {
        fail(error, std::string("scene: '") + key + "' contains a NUL byte");
        return false;
    }
    if (text.size() > maxChars) {
        fail(error, std::string("scene: '") + key + "' is " + std::to_string(text.size()) +
                        " characters, over the limit of " + std::to_string(maxChars));
        return false;
    }
    *out = std::move(text);
    return true;
}

// The keys a scene object may carry. An unknown key is refused rather than
// ignored: this payload is the audience's content, and a field this build
// does not read is a field that silently never appears on air (see scene.h).
// The error names the key, clipped — the reply must not be as large as the
// key it is complaining about.
bool noUnknownKeys(const boost::json::object& obj,
                   const std::initializer_list<const char*>& known, const char* where,
                   std::string* error) {
    for (const auto& entry : obj) {
        bool matched = false;
        for (const char* candidate : known) {
            if (entry.key() == candidate) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            fail(error, std::string("scene: ") + where + " has no field '" +
                            clipForError(entry.key()) + "'");
            return false;
        }
    }
    return true;
}

bool wantColor(const boost::json::object& obj, const char* key, Color* out,
               std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !(value->is_int64() || value->is_uint64())) {
        fail(error, std::string("scene: '") + key + "' must be a packed 0xRRGGBB integer");
        return false;
    }
    const long long signedValue =
        value->is_int64() ? value->as_int64() : static_cast<long long>(value->as_uint64());
    if (signedValue < 0 || signedValue > 0xFFFFFFLL) {
        fail(error, std::string("scene: '") + key + "' is outside 0xRRGGBB");
        return false;
    }
    *out = static_cast<Color>(signedValue);
    return true;
}

bool wantBool(const boost::json::object& obj, const char* key, bool* out,
              std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_bool()) {
        fail(error, std::string("scene: '") + key + "' must be a boolean");
        return false;
    }
    *out = value->as_bool();
    return true;
}

bool wantFontSize(const boost::json::object& obj, const char* key, double* out,
                  std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_number()) {
        fail(error, std::string("scene: '") + key + "' must be a number");
        return false;
    }
    const double size = value->to_number<double>();
    if (!std::isfinite(size) || size < 0.0) {
        fail(error, std::string("scene: '") + key +
                        "' must be finite and >= 0 (0 means renderer default)");
        return false;
    }
    *out = size;
    return true;
}

// One authored text-box edge: any finite coordinate.
bool wantBoxEdge(const boost::json::object& obj, const char* key, double* out,
                 std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_number()) {
        fail(error, std::string("scene: '") + key + "' must be a number");
        return false;
    }
    const double edge = value->to_number<double>();
    if (!std::isfinite(edge)) {
        fail(error, std::string("scene: '") + key + "' must be finite");
        return false;
    }
    *out = edge;
    return true;
}

// One authored text-box size: finite and strictly positive, or the item
// could neither wrap at nor be centred in it.
bool wantBoxSize(const boost::json::object& obj, const char* key, double* out,
                 std::string* error) {
    if (!wantBoxEdge(obj, key, out, error)) {
        return false;
    }
    if (*out <= 0.0) {
        fail(error, std::string("scene: '") + key + "' must be positive");
        return false;
    }
    return true;
}

// Where the lines sit inside the box (task 4.4) — the three words the
// domain stores, and nothing else.
bool wantAlign(const boost::json::object& obj, const char* key, std::string* out,
               std::string* error) {
    // Sixteen is generous for three words the next check narrows to a set;
    // the budget exists so an over-long value is reported as oversized rather
    // than as a wrong alignment.
    if (!wantString(obj, key, out, 16, error)) {
        return false;
    }
    if (*out != "left" && *out != "center" && *out != "right") {
        fail(error, std::string("scene: '") + key +
                        "' must be \"left\", \"center\" or \"right\"");
        return false;
    }
    return true;
}

// The line-box multiplier, in the sane band the layout assumes.
bool wantLineSpacing(const boost::json::object& obj, const char* key, double* out,
                     std::string* error) {
    if (!wantBoxEdge(obj, key, out, error)) {
        return false;
    }
    if (*out < 0.25 || *out > 8.0) {
        fail(error, std::string("scene: '") + key + "' must be within 0.25..8");
        return false;
    }
    return true;
}

}  // namespace

bool operator==(const Background& a, const Background& b) {
    return a.color == b.color && a.hasImage == b.hasImage && a.isVideo == b.isVideo &&
           a.assetId == b.assetId && a.assetPath == b.assetPath;
}
bool operator!=(const Background& a, const Background& b) { return !(a == b); }

bool operator==(const SceneItem& a, const SceneItem& b) {
    return a.isImage == b.isImage && a.lines == b.lines && a.color == b.color &&
           a.fontFamily == b.fontFamily && a.fontSize == b.fontSize && a.x == b.x &&
           a.y == b.y && a.width == b.width && a.height == b.height &&
           a.align == b.align && a.lineSpacing == b.lineSpacing &&
           a.assetId == b.assetId && a.assetPath == b.assetPath;
}
bool operator!=(const SceneItem& a, const SceneItem& b) { return !(a == b); }

bool operator==(const Scene& a, const Scene& b) {
    return a.showId == b.showId && a.slideId == b.slideId &&
           a.background == b.background && a.items == b.items;
}
bool operator!=(const Scene& a, const Scene& b) { return !(a == b); }

Scene clearedScene() {
    return Scene{};  // background black by declaration, no items
}

bool assetPathIsVideo(const std::string& path) {
    // One name rule for both uses (task 6.17b): the model refuses a video
    // asset for a foreground image item with the SAME function this
    // delegates to, so a kind cannot disagree between the two places a
    // path's kind is asked. The scene lib still owns no filesystem access.
    return domain::assetPathLooksLikeVideo(path);
}

bool assetPathIsLocal(const std::string& path, std::string* why) {
    if (why != nullptr) {
        why->clear();  // set only when the answer is "no", always when it is
    }
    auto reject = [&](std::string reason) {
        if (why != nullptr) {
            *why = std::move(reason);
        }
        return false;
    };
    if (path.empty()) {
        return true;  // "no media asset" is legal; fromJson decides when it is not
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(path[i]);
        if (byte == 0u) {
            return reject("the asset path contains a NUL byte");
        }
        if (byte < 0x20u || byte == 0x7Fu) {
            return reject("the asset path contains a control character");
        }
    }
    // A scheme prefix means this is not a filesystem path at all. "data:"
    // would smuggle media BYTES into a control message (the thing the whole
    // transport exists to avoid); any other scheme asks the worker to fetch
    // over a network it has no contract for. Schemes are case-insensitive,
    // and a colon may not appear before the first slash, so
    // "weird:name/file.png" stays the ordinary relative path it is.
    const std::size_t colon = path.find(':');
    if (colon != std::string::npos) {
        const std::size_t slash = path.find('/');
        if (slash == std::string::npos || colon < slash) {
            std::string scheme;
            for (std::size_t i = 0; i < colon; ++i) {
                const char c = path[i];
                scheme += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            }
            return reject("the asset path is a '" + clipForError(scheme) +
                          "' reference; the worker opens local files only");
        }
    }
    return true;
}

bool compileSlide(const domain::Documents& documents, const domain::Id& showId,
                  const domain::Id& slideId, Scene* out, std::string* error) {
    if (out == nullptr) {
        fail(error, "compileSlide needs a target");
        return false;
    }
    const domain::Show* show = documents.findShow(showId);
    if (show == nullptr) {
        fail(error, "no show '" + showId + "' to compile");
        return false;
    }
    const domain::Slide* slide = nullptr;
    for (const domain::Slide& candidate : show->slides) {
        if (candidate.id == slideId) {
            slide = &candidate;
            break;
        }
    }
    if (slide == nullptr) {
        fail(error, "show '" + showId + "' has no slide '" + slideId + "'");
        return false;
    }

    // The style chain is the model's; nullptr means built-in defaults.
    const domain::Style* style = documents.styleForSlide(showId, slideId);

    Scene scene;
    scene.showId = showId;
    scene.slideId = slideId;
    Color textColor = kWhite;
    Color backgroundColor = kBlack;
    std::string fontFamily;
    double fontSize = kRendererDefaultFontSize;
    if (style != nullptr) {
        if (!parseColor(style->textColor, &textColor)) {
            fail(error, "style '" + style->id + "' text colour '" + style->textColor +
                            "' is not #rgb or #rrggbb");
            return false;
        }
        if (!parseColor(style->backgroundColor, &backgroundColor)) {
            fail(error, "style '" + style->id + "' background colour '" +
                            style->backgroundColor + "' is not #rgb or #rrggbb");
            return false;
        }
        fontFamily = style->fontFamily;
        fontSize = style->fontSize;
    }

    // One wire item per authored box, in the model's DRAW order (task 6.17a,
    // review R17): the renderer already lays out and paints a list, so what
    // the operator stacked is what gets painted, later boxes over earlier
    // ones. A slide with no boxes compiles to a scene with no items — a
    // background with no words is a thing operators make.
    if (slide->items.size() > kMaxSceneItems) {
        // Unreachable through the model's own operations, which cap a slide at
        // kMaxTextBoxes and are pinned equal to this constant by a test. It
        // stays because the two libraries are compiled separately: a scene this
        // function emitted but its own codec refused would be a present-time
        // failure for a slide that composed fine.
        fail(error, "slide '" + slideId + "' holds " + std::to_string(slide->items.size()) +
                        " items and a scene carries at most " +
                        std::to_string(kMaxSceneItems));
        return false;
    }
    for (const domain::SlideItem& source : slide->items) {
        SceneItem item;
        // The authored box travels unchanged (task 4.3), either kind.
        item.x = source.x;
        item.y = source.y;
        item.width = source.width;
        item.height = source.height;
        if (source.kind == domain::ItemKind::Image) {
            // A foreground picture (task 6.17b): resolve asset -> path with
            // the same discipline as the background below, so an item that
            // names what the model cannot show fails the compile rather
            // than painting a silent gap. The model already refuses an
            // unknown or video-named asset at addItem; these guards are
            // the same belt the unknown-BACKGROUND message wears — the
            // libraries are compiled separately.
            const domain::Asset* asset = documents.findAsset(source.assetId);
            if (asset == nullptr) {
                fail(error, "slide '" + slideId + "' image item '" + source.id +
                                "' names unknown asset '" + source.assetId + "'");
                return false;
            }
            if (domain::assetPathLooksLikeVideo(asset->sourcePath)) {
                fail(error, "slide '" + slideId + "' image item '" + source.id +
                                "' names the video asset '" + source.assetId +
                                "'; foreground items composite stills");
                return false;
            }
            item.isImage = true;
            item.assetId = asset->id;
            item.assetPath = asset->sourcePath;
            scene.items.push_back(std::move(item));
            continue;
        }
        item.lines = source.lines;
        item.color = textColor;
        item.fontFamily = fontFamily;
        item.fontSize = fontSize;
        // Typography: the resolved style's alignment and line box (task 4.4),
        // falling back to the item defaults when the slide carries no style.
        // Boxes on one slide share its style — per-box typography is a later
        // slice of R17, and inventing a field the editor cannot set yet is how
        // a file gets a meaning nothing can write.
        item.align = style != nullptr ? style->align : std::string("center");
        item.lineSpacing = style != nullptr ? style->lineSpacing : domain::kDefaultLineSpacing;
        scene.items.push_back(std::move(item));
    }

    if (!slide->backgroundAssetId.empty()) {
        const domain::Asset* asset = documents.findAsset(slide->backgroundAssetId);
        if (asset == nullptr) {
            // Not reachable through the model's own operations (removal
            // clears references) — but a compiled scene must never
            // silently drop a requested background.
            fail(error, "slide '" + slideId + "' names unknown asset '" +
                            slide->backgroundAssetId + "'");
            return false;
        }
        scene.background.color = backgroundColor;
        scene.background.hasImage = true;
        // Task 5.5: the kind is decided by the name, not the bytes —
        // a video-extension asset plays frames; anything else stays a
        // decoded still. Whether the file cooperates is reported by the
        // renderer (and degrades to the colour fill).
        scene.background.isVideo = assetPathIsVideo(asset->sourcePath);
        scene.background.assetId = asset->id;
        scene.background.assetPath = asset->sourcePath;
    } else {
        scene.background.color = backgroundColor;
    }

    *out = std::move(scene);
    return true;
}

// --- wire form ---------------------------------------------------------------

boost::json::value toJson(const Scene& scene) {
    boost::json::object root;
    root.emplace("scene", kSceneFormat);
    root.emplace("showId", scene.showId);
    root.emplace("slideId", scene.slideId);
    root.emplace("backgroundColor", static_cast<std::uint32_t>(scene.background.color));

    boost::json::object background;
    background.emplace("image", scene.background.hasImage);
    background.emplace("video", scene.background.isVideo);
    background.emplace("assetId", scene.background.assetId);
    background.emplace("assetPath", scene.background.assetPath);
    root.emplace("background", std::move(background));

    boost::json::array items;
    for (const SceneItem& item : scene.items) {
        boost::json::object jsonItem;
        jsonItem.emplace("kind", item.isImage ? "image" : "text");
        if (item.isImage) {
            // An image item carries the background's pair and its box —
            // and nothing else: the per-kind key set is as closed as the
            // scene's own, so a text item's typography can never travel
            // attached to a picture the renderer would ignore.
            jsonItem.emplace("assetId", item.assetId);
            jsonItem.emplace("assetPath", item.assetPath);
            jsonItem.emplace("x", item.x);
            jsonItem.emplace("y", item.y);
            jsonItem.emplace("width", item.width);
            jsonItem.emplace("height", item.height);
            items.push_back(std::move(jsonItem));
            continue;
        }
        boost::json::array lines;
        for (const std::string& line : item.lines) {
            lines.emplace_back(line);
        }
        jsonItem.emplace("lines", std::move(lines));
        jsonItem.emplace("color", static_cast<std::uint32_t>(item.color));
        jsonItem.emplace("fontFamily", item.fontFamily);
        jsonItem.emplace("fontSize", item.fontSize);
        jsonItem.emplace("x", item.x);
        jsonItem.emplace("y", item.y);
        jsonItem.emplace("width", item.width);
        jsonItem.emplace("height", item.height);
        jsonItem.emplace("align", item.align);
        jsonItem.emplace("lineSpacing", item.lineSpacing);
        items.push_back(std::move(jsonItem));
    }
    root.emplace("items", std::move(items));
    return root;
}

bool fromJson(const boost::json::value& value, Scene* out, std::string* error) {
    if (out == nullptr) {
        fail(error, "scene fromJson needs a target");
        return false;
    }
    if (!value.is_object()) {
        fail(error, "scene: top level must be an object");
        return false;
    }
    const boost::json::object& root = value.as_object();

    const boost::json::value* format = field(root, "scene");
    if (format == nullptr || !format->is_int64() ||
        format->as_int64() != kSceneFormat) {
        fail(error, "scene: unsupported scene format (this build reads scene 2)");
        return false;
    }

    Scene scene;
    if (!wantString(root, "showId", &scene.showId, kMaxIdChars, error) ||
        !wantString(root, "slideId", &scene.slideId, kMaxIdChars, error) ||
        !wantColor(root, "backgroundColor", &scene.background.color, error)) {
        return false;
    }
    if (!noUnknownKeys(root, {"scene", "showId", "slideId", "backgroundColor", "background",
                              "items"},
                       "the scene", error)) {
        return false;
    }

    const boost::json::value* background = field(root, "background");
    if (background == nullptr || !background->is_object()) {
        fail(error, "scene: 'background' must be an object");
        return false;
    }
    const boost::json::object& jsonBackground = background->as_object();
    if (!noUnknownKeys(jsonBackground, {"image", "video", "assetId", "assetPath"},
                       "'background'", error)) {
        return false;
    }
    if (!wantBool(jsonBackground, "image", &scene.background.hasImage, error) ||
        !wantString(jsonBackground, "assetId", &scene.background.assetId, kMaxIdChars,
                    error)) {
        return false;
    }
    // A media reference is checked WHOLE, before the hasImage clear below, and
    // BEFORE the length rule that wantString would apply to it: a 200 KB data:
    // URI is a media-byte payload smuggled into a control message, and calling
    // that "too long" hides what was actually wrong with it. So the raw value
    // goes through assetPathIsLocal first (refusing any scheme, a NUL and other
    // control bytes whatever its size) and only then through the length budget,
    // which now bounds what is left — a genuinely local path. Both are still
    // refused; what the order decides is which reason the operator is given.
    const boost::json::value* rawPath = field(jsonBackground, "assetPath");
    if (rawPath != nullptr && rawPath->is_string()) {
        std::string why;
        if (!assetPathIsLocal(std::string(rawPath->as_string()), &why)) {
            fail(error, "scene: " + why);
            return false;
        }
    }
    if (!wantString(jsonBackground, "assetPath", &scene.background.assetPath, kMaxPathChars,
                    error)) {
        return false;
    }
    if (scene.background.hasImage && scene.background.assetPath.empty()) {
        fail(error, "scene: background 'image' requires an 'assetPath'");
        return false;
    }
    // Task 5.5: the kind flag is optional (scene 1 predates it, so an
    // absent key means "image"); when present it must be a bool, and
    // "video" without a media asset is nonsense the validator refuses.
    const boost::json::value* video = field(jsonBackground, "video");
    if (video != nullptr) {
        if (!video->is_bool()) {
            fail(error, "scene: background 'video' must be a bool");
            return false;
        }
        scene.background.isVideo = video->as_bool();
    }
    if (scene.background.isVideo && !scene.background.hasImage) {
        fail(error, "scene: background 'video' requires 'image'");
        return false;
    }
    if (!scene.background.hasImage) {
        scene.background.assetId.clear();
        scene.background.assetPath.clear();
        scene.background.isVideo = false;
    }

    const boost::json::value* items = field(root, "items");
    if (items == nullptr || !items->is_array()) {
        fail(error, "scene: 'items' must be an array");
        return false;
    }
    if (items->as_array().size() > kMaxSceneItems) {
        fail(error, "scene: " + std::to_string(items->as_array().size()) +
                        " items is over the limit of " +
                        std::to_string(kMaxSceneItems));
        return false;
    }
    std::size_t sceneTextChars = 0;
    for (const boost::json::value& element : items->as_array()) {
        if (!element.is_object()) {
            fail(error, "scene: 'items' holds a non-object");
            return false;
        }
        const boost::json::object& jsonItem = element.as_object();
        // Scene 2 (task 6.17b): the kind comes first, because the CLOSED key
        // set is per kind — a text item's typography on a picture, or a
        // picture's path on a verse, is the same silent-gap class the scene
        // format has refused since 6.8a.
        const boost::json::value* kindValue = field(jsonItem, "kind");
        if (kindValue == nullptr || !kindValue->is_string()) {
            fail(error, "scene: an item's 'kind' must be a string");
            return false;
        }
        const std::string kind(kindValue->as_string());
        if (kind == "image") {
            if (!noUnknownKeys(jsonItem, {"kind", "assetId", "assetPath", "x", "y", "width",
                                          "height"},
                               "an image item", error)) {
                return false;
            }
            SceneItem item;
            item.isImage = true;
            if (!wantString(jsonItem, "assetId", &item.assetId, kMaxIdChars, error)) {
                return false;
            }
            if (item.assetId.empty()) {
                fail(error, "scene: an image item requires an 'assetId'");
                return false;
            }
            // The same whole-reference gate the background's assetPath is
            // put through: scheme, NUL and control bytes are refused on the
            // RAW value, before the length budget names what is left.
            const boost::json::value* rawPath = field(jsonItem, "assetPath");
            if (rawPath != nullptr && rawPath->is_string()) {
                std::string why;
                if (!assetPathIsLocal(std::string(rawPath->as_string()), &why)) {
                    fail(error, "scene: " + why);
                    return false;
                }
            }
            if (!wantString(jsonItem, "assetPath", &item.assetPath, kMaxPathChars, error)) {
                return false;
            }
            if (item.assetPath.empty()) {
                fail(error, "scene: an image item requires an 'assetPath'");
                return false;
            }
            if (!wantBoxEdge(jsonItem, "x", &item.x, error) ||
                !wantBoxEdge(jsonItem, "y", &item.y, error) ||
                !wantBoxSize(jsonItem, "width", &item.width, error) ||
                !wantBoxSize(jsonItem, "height", &item.height, error)) {
                return false;
            }
            scene.items.push_back(std::move(item));
            continue;
        }
        if (kind != "text") {
            fail(error, "scene: item kind '" + kind + "' is neither 'text' nor 'image'");
            return false;
        }
        if (!noUnknownKeys(jsonItem,
                           {"kind", "lines", "color", "fontFamily", "fontSize", "x", "y",
                            "width", "height", "align", "lineSpacing"},
                           "a text item", error)) {
            return false;
        }
        SceneItem item;
        const boost::json::value* lines = field(jsonItem, "lines");
        if (lines == nullptr || !lines->is_array()) {
            fail(error, "scene: an item's 'lines' must be an array");
            return false;
        }
        if (lines->as_array().size() > kMaxItemLines) {
            fail(error, "scene: an item's " + std::to_string(lines->as_array().size()) +
                            " lines are over the limit of " +
                            std::to_string(kMaxItemLines));
            return false;
        }
        std::size_t itemTextChars = 0;
        for (const boost::json::value& line : lines->as_array()) {
            if (!line.is_string()) {
                fail(error, "scene: an item's 'lines' holds a non-string");
                return false;
            }
            std::string text(line.as_string());
            if (text.find('\0') != std::string::npos) {
                fail(error, "scene: an item's line contains a NUL byte");
                return false;
            }
            if (text.size() > kMaxLineChars) {
                fail(error, "scene: a line of " + std::to_string(text.size()) +
                                " characters is over the limit of " +
                                std::to_string(kMaxLineChars));
                return false;
            }
            itemTextChars += text.size();
            if (itemTextChars > kMaxItemTextChars) {
                fail(error, "scene: one item's text exceeds " +
                                std::to_string(kMaxItemTextChars) + " characters");
                return false;
            }
            item.lines.push_back(std::move(text));
        }
        sceneTextChars += itemTextChars;
        // The binding budget of the set: however the text is spread across
        // layers and lines, one scene cannot carry more authored words than
        // this, which is what makes the serialised worst case a known number.
        if (sceneTextChars > kMaxSceneTextChars) {
            fail(error, "scene: the slide's text exceeds " +
                            std::to_string(kMaxSceneTextChars) + " characters");
            return false;
        }
        if (!wantColor(jsonItem, "color", &item.color, error) ||
            !wantString(jsonItem, "fontFamily", &item.fontFamily, kMaxFontFamilyChars,
                        error) ||
            !wantFontSize(jsonItem, "fontSize", &item.fontSize, error) ||
            !wantBoxEdge(jsonItem, "x", &item.x, error) ||
            !wantBoxEdge(jsonItem, "y", &item.y, error) ||
            !wantBoxSize(jsonItem, "width", &item.width, error) ||
            !wantBoxSize(jsonItem, "height", &item.height, error) ||
            !wantAlign(jsonItem, "align", &item.align, error) ||
            !wantLineSpacing(jsonItem, "lineSpacing", &item.lineSpacing, error)) {
            return false;
        }
        scene.items.push_back(std::move(item));
    }

    *out = std::move(scene);
    return true;
}

}  // namespace openshow::scene
