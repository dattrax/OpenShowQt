/*
 * Scene compilation and the wire codec — see scene.h for the contract.
 */

#include "openshow/scene/scene.h"

#include <cmath>
#include <string>

namespace openshow::scene {

namespace {

constexpr int kSceneFormat = 1;

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
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

bool wantString(const boost::json::object& obj, const char* key, std::string* out,
                std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_string()) {
        fail(error, std::string("scene: '") + key + "' must be a string");
        return false;
    }
    *out = std::string(value->as_string());
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
    if (!wantString(obj, key, out, error)) {
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

bool operator==(const TextItem& a, const TextItem& b) {
    return a.lines == b.lines && a.color == b.color && a.fontFamily == b.fontFamily &&
           a.fontSize == b.fontSize && a.x == b.x && a.y == b.y && a.width == b.width &&
           a.height == b.height && a.align == b.align && a.lineSpacing == b.lineSpacing;
}
bool operator!=(const TextItem& a, const TextItem& b) { return !(a == b); }

bool operator==(const Scene& a, const Scene& b) {
    return a.showId == b.showId && a.slideId == b.slideId &&
           a.background == b.background && a.items == b.items;
}
bool operator!=(const Scene& a, const Scene& b) { return !(a == b); }

Scene clearedScene() {
    return Scene{};  // background black by declaration, no items
}

bool assetPathIsVideo(const std::string& path) {
    // Extension-based, case-insensitive: the scene lib owns no filesystem
    // access, so the kind travels with the name (see scene.h).
    const size_t slash = path.find_last_of('/');
    const std::string name =
        slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return false;
    }
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    static constexpr const char* kVideoExtensions[] = {
        "mp4", "mov", "m4v", "mkv", "webm", "avi",
        "mpg", "mpeg", "wmv", "flv", "ogv", "ts",
    };
    for (const char* candidate : kVideoExtensions) {
        if (ext == candidate) {
            return true;
        }
    }
    return false;
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

    TextItem item;
    item.lines = slide->lines;
    item.color = textColor;
    item.fontFamily = fontFamily;
    item.fontSize = fontSize;
    // The authored text box travels unchanged (task 4.3).
    item.x = slide->x;
    item.y = slide->y;
    item.width = slide->width;
    item.height = slide->height;
    // Typography: the resolved style's alignment and line box (task 4.4),
    // falling back to the item defaults when the slide carries no style.
    item.align = style != nullptr ? style->align : std::string("center");
    item.lineSpacing = style != nullptr ? style->lineSpacing : domain::kDefaultLineSpacing;
    scene.items.push_back(std::move(item));

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
    for (const TextItem& item : scene.items) {
        boost::json::object jsonItem;
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
        fail(error, "scene: unsupported scene format (this build reads scene 1)");
        return false;
    }

    Scene scene;
    if (!wantString(root, "showId", &scene.showId, error) ||
        !wantString(root, "slideId", &scene.slideId, error) ||
        !wantColor(root, "backgroundColor", &scene.background.color, error)) {
        return false;
    }

    const boost::json::value* background = field(root, "background");
    if (background == nullptr || !background->is_object()) {
        fail(error, "scene: 'background' must be an object");
        return false;
    }
    const boost::json::object& jsonBackground = background->as_object();
    if (!wantBool(jsonBackground, "image", &scene.background.hasImage, error) ||
        !wantString(jsonBackground, "assetId", &scene.background.assetId, error) ||
        !wantString(jsonBackground, "assetPath", &scene.background.assetPath, error)) {
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
    for (const boost::json::value& element : items->as_array()) {
        if (!element.is_object()) {
            fail(error, "scene: 'items' holds a non-object");
            return false;
        }
        const boost::json::object& jsonItem = element.as_object();
        TextItem item;
        const boost::json::value* lines = field(jsonItem, "lines");
        if (lines == nullptr || !lines->is_array()) {
            fail(error, "scene: an item's 'lines' must be an array");
            return false;
        }
        for (const boost::json::value& line : lines->as_array()) {
            if (!line.is_string()) {
                fail(error, "scene: an item's 'lines' holds a non-string");
                return false;
            }
            item.lines.push_back(std::string(line.as_string()));
        }
        if (!wantColor(jsonItem, "color", &item.color, error) ||
            !wantString(jsonItem, "fontFamily", &item.fontFamily, error) ||
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
