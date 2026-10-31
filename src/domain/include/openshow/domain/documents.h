/*
 * The document model (task 2.1): shows, slides, groups, styles and
 * playlists as plain data plus the operations every edit goes through.
 *
 * The vocabulary follows docs/reference.md's mapping table: a show is a
 * deck of slides, a group is a chorus/verse label carried by slides, a
 * playlist (FreeShow's "project") is an ordered run of shows with
 * optional section markers. Styles are named appearance records; 2.1
 * owns their content, scene compilation reads them later (2.4).
 *
 * Everything here is value-semantic standard-library data: no Qt, no
 * FFmpeg, no Vulkan, no files. Persistence (2.2) and undo (2.3) wrap
 * these operations, which is why every mutating call is a named
 * operation on Documents rather than free access to the members.
 *
 * Identity: every object owns a string Id issued by Documents
 * ("show1", "slide2", "text7", "image3" — a kind prefix plus a store-wide
 * counter, so
 * ids stay unique across kinds and are reproducible in tests). Slides
 * live inside their show's vector (presentation order is vector order)
 * but keep their Id, which operations use; Ids never recycle. The counter is
 * shared rather than per-kind, which is what makes a slide id skip a number
 * once the slide owns stack items — ids name objects, and an operator reads
 * names, never numbering.
 *
 * Failure contract, matching the rest of the project: operations return
 * false and fill the optional error string when an argument is unknown
 * (bad Id), empty (names) or out of range (indices). A failed operation
 * changes nothing.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace openshow::domain {

// Identifies one object within one Documents instance. Opaque beyond
// uniqueness; persistence (2.2) serialises it verbatim.
using Id = std::string;

// What one stack entry draws (task 6.17b, review R17). A slide's stack is
// heterogeneous from here on: words and pictures share one draw order, and
// the kind says which of the two content fields means anything.
enum class ItemKind { Text, Image };

// How the background picture sits on the canvas (tasks 6.17c1/c2, review
// R17). COVER — scale to fill, centre, crop — is what the worker has drawn
// since 3.2 and remains the DEFAULT: a file written before this field
// existed means exactly what it drew. CONTAIN scales the picture inside the
// canvas and lets the colour fill show in the bars. BLUR is the reference's
// rule (blur 6, opacity 0.3): the CONTAIN-fitted picture over a blurred
// COVER backdrop of the same picture at 30% — the backdrop is computed once
// per image where its bytes are cached, never per frame, and it is refused
// (at the compile) for a VIDEO background, because playing frames cannot
// pay a per-frame blur within the composition budget. `fill` (distorting
// stretch) is not in the agreed set, and neither this enum nor any reader
// will grow it silently.
enum class BackgroundFit { Cover, Contain, Blur };

// The ONE spelling of the fits, shared by the service file and the scene
// wire (the assetPathLooksLikeVideo pattern: two libraries that each parsed
// their own copy of a name list is how a model and a compiler drift).
inline const char* backgroundFitName(BackgroundFit fit) {
    switch (fit) {
        case BackgroundFit::Contain:
            return "contain";
        case BackgroundFit::Blur:
            return "blur";
        case BackgroundFit::Cover:
            break;
    }
    return "cover";
}
inline bool backgroundFitFromName(const std::string& name, BackgroundFit* fit) {
    if (name == "cover") {
        *fit = BackgroundFit::Cover;
        return true;
    }
    if (name == "contain") {
        *fit = BackgroundFit::Contain;
        return true;
    }
    if (name == "blur") {
        *fit = BackgroundFit::Blur;
        return true;
    }
    return false;
}

// What the media pipeline makes of a file's NAME (task UI.4a). The rule is
// extension-based and case-insensitive — the same one task 5.5 gave
// backgrounds, held here so the model can refuse a video asset for a
// foreground image item at add time rather than at present time, so
// scene::assetPathIsVideo is the same answer, and so the media bin's label
// for a row is the opinion the pipeline will act on rather than a second
// one written in QML.
//
// A browse-time label, not a validation: whether the bytes exist and whether
// they decode stay the renderer's problem, exactly as the scene's background
// comment says. Audio is named because the bin has to say what an imported
// soundtrack IS even though nothing presents it yet (its transport arrives
// with the show controls), and Other is the honest answer for a name nothing
// claims — the bin shows that as unsupported instead of hiding the row, which
// is the only way an operator can find out the file is the wrong thing.
enum class AssetKind { Picture, Video, Audio, Other };

// The three lists the kind rule claims, in the order the kind asks them,
// null-terminated so a caller can walk them. The media import dialog builds
// its file filter from these (DocumentController::mediaImportNameFilters),
// which is how the dialog cannot offer a name the bin would label "other".
inline constexpr const char* const kVideoExtensions[] = {
    "mp4", "mov", "m4v", "mkv", "webm", "avi",
    "mpg", "mpeg", "wmv", "flv", "ogv", "ts", nullptr,
};
inline constexpr const char* const kPictureExtensions[] = {
    "jpg", "jpeg", "png", "webp", "gif", "bmp", nullptr,
};
inline constexpr const char* const kAudioExtensions[] = {
    "mp3", "wav", "ogg", "oga", "opus", "m4a", "aac", "flac", nullptr,
};

// The lower-cased extension of a path's file name, empty when it has none.
inline std::string assetPathExtension(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string name =
        slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return {};
    }
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

inline AssetKind assetPathKind(const std::string& path) {
    const std::string ext = assetPathExtension(path);
    auto named = [&ext](const char* const* names) {
        for (; *names != nullptr; ++names) {
            if (ext == *names) {
                return true;
            }
        }
        return false;
    };
    if (named(kVideoExtensions)) {
        return AssetKind::Video;
    }
    if (named(kPictureExtensions)) {
        return AssetKind::Picture;
    }
    if (named(kAudioExtensions)) {
        return AssetKind::Audio;
    }
    return AssetKind::Other;
}

// The wire/QML spelling of a kind: one word, lower case, never empty.
inline const char* assetKindName(AssetKind kind) {
    switch (kind) {
        case AssetKind::Picture:
            return "picture";
        case AssetKind::Video:
            return "video";
        case AssetKind::Audio:
            return "audio";
        case AssetKind::Other:
            break;
    }
    return "other";
}

// True when a file's NAME is one the media pipeline treats as video: the
// video half of the kind above, kept as a name because every existing caller
// asks exactly that question, and one function answers it everywhere.
inline bool assetPathLooksLikeVideo(const std::string& path) {
    return assetPathKind(path) == AssetKind::Video;
}

// One authored item (tasks 6.17a/6.17b, review R17): its own identity, its
// own content, its own place in the 1920x1080 canvas space.
//
// The id is store-unique like every other id here, and it is how every edit
// names the item — select it, move it, delete it — so it has to survive a
// save, a reload and an undo unchanged. That is the whole reason a second
// box needs an id at all: address by position and moving one item silently
// retargets whatever the operator had selected.
struct SlideItem {
    Id id;
    ItemKind kind = ItemKind::Text;
    // Text items only. Blank lines are meaningful (stanza breaks) and
    // survive untouched. An image item's lines stay empty — the model
    // refuses a text item that names an asset and an image item that
    // carries words, so "which fields mean something" is always kind.
    std::vector<std::string> lines;
    // Image items only: the asset whose picture composites here. The
    // document names the asset and never holds bytes; the compiler
    // resolves id -> path exactly as a slide's background does (task 2.4).
    Id assetId;
    // The authored box in the 1920x1080 canvas space (docs/reference.md §7).
    // Defaults to the canvas minus the lyric margins — the region M2/M3 laid
    // text inside, so untouched slides render exactly as before — and the
    // renderer centres the text in the box (task 4.3). An image item is
    // stretched to this box (fit modes are the 6.17c slice).
    double x = 160.0;
    double y = 100.0;
    double width = 1600.0;
    double height = 880.0;
};

// A slide holds at most this many stack entries, of either kind. The number
// is not a taste limit: a compiled scene is one RPC message, and the scene
// wire refuses a scene with more items than this (scene.h's kMaxSceneItems,
// pinned equal to this constant by a test). A model that could hold a tenth
// item could hold a slide that cannot be drawn, and the failure would
// surface at present time.
inline constexpr std::size_t kMaxSlideItems = 8;

// One slide of a show: a stack of items over a background.
struct Slide {
    Id id;
    // Group label ("Chorus", "Verse", ...). Empty means the slide is
    // ungrouped, which is the state a new slide starts in.
    std::string group;
    // The slide's content, as a stack in DRAW ORDER: index 0 is painted
    // first and a later item covers an earlier one where they overlap
    // (6.17b brought pictures into that stack beside words). Ordering is
    // part of the document, so it is stored and undone like any other field.
    //
    // Empty is a legal slide — a background with nothing on it — but only an
    // authored one: every operation that creates a slide gives it one text
    // box with the default geometry, which is what a service written before
    // multiple boxes existed describes.
    std::vector<SlideItem> items;
    std::string notes;
    // Style override; empty follows the show's default style.
    Id styleId;
    // Background image asset; empty means no background.
    Id backgroundAssetId;
    // How that picture sits on the canvas (6.17c1). The field belongs to the
    // SLIDE because the background reference does; a slide with no picture
    // still keeps its authored choice for when it gets one. Default COVER is
    // what every pre-6.17c file drew, so loading one is not a re-render.
    BackgroundFit backgroundFit = BackgroundFit::Cover;

    // The item the slide's single-text operations address (setSlideLines,
    // setSlideGeometry): the FIRST in draw order, which for every slide
    // written before 6.17a is the only one there is. Null when the slide has
    // no item at all.
    const SlideItem* firstItem() const {
        return items.empty() ? nullptr : &items.front();
    }
};

struct Show {
    Id id;
    std::string name;
    std::string notes;
    std::vector<Slide> slides;  // presentation order
    // Style new slides implicitly use; empty means "no style yet".
    Id defaultStyleId;
};

// A named appearance. Colours are "#rgb" or "#rrggbb"; a zero font size
// means "renderer default". Scene compilation (2.4) maps these onto
// Skia paint state; 2.1 only validates syntax it can actually check.
// The renderer's line-box factor doubles as the domain default, so a
// style created before typography existed keeps drawing as it did.
inline constexpr double kDefaultLineSpacing = 1.25;

inline bool isStyleAlign(const std::string& align) {
    return align == "left" || align == "center" || align == "right";
}

struct Style {
    Id id;
    std::string name;
    std::string textColor;
    std::string backgroundColor;
    // Second background colour (task 6.17c3, review R17). Empty — the
    // default — paints the flat backgroundColor it always did. When set,
    // the background is a vertical gradient from backgroundColor at the
    // top to this colour at the bottom: OpenShow's reference editor makes
    // backgrounds with exactly two colours and a downward axis, and the
    // field belongs to the STYLE because that is where the background
    // colour lives (unlike the fit, which followed the picture onto the
    // slide). Same "#rgb"/"#rrggbb" form and same rule: the renderer
    // refuses a string it cannot parse rather than approximating it.
    std::string backgroundGradient;
    std::string fontFamily;
    double fontSize = 0.0;
    // Typography (task 4.4): horizontal alignment inside the slide's text
    // box ("left" | "center" | "right") and the line-box multiplier the
    // renderer applies (its own default, 1.25, is kDefaultLineSpacing).
    std::string align = "center";
    double lineSpacing = kDefaultLineSpacing;
};

// One media file belonging to the service. The model records only the
// name and where to find the bytes; storage (task 2.2) copies them into
// the service package at save time. sourcePath is a plain OS path —
// existence is only checked when saving, so Documents stays fs-free.
struct Asset {
    Id id;
    std::string name;       // display/filename, e.g. "stage-loop.jpg"
    std::string sourcePath; // current location of the file's bytes
};

// One asset's bytes now live at `to` rather than `from`: what a save
// that adopts reports (task 6.12, review R7). Everyone still holding the
// old path — the model, a history snapshot, a cached scene payload — has
// to be told, because after the save the old location may be deleted
// while the session keeps running.
struct AssetRebind {
    Id assetId;
    std::string from;
    std::string to;
};

// One playlist row: a show, optionally introduced by a section marker.
struct PlaylistEntry {
    Id id;
    Id showId;
    std::string section;
};

struct Playlist {
    Id id;
    std::string name;
    std::vector<PlaylistEntry> entries;  // running order
};

// The registry plus every supported edit. Single-threaded by contract;
// the worker will own one instance behind its RPC loop.
class Documents {
public:
    // --- shows ---

    // Names must be non-blank; duplicates are allowed (ids distinguish).
    // Returns the new show's Id, or "" on failure.
    Id createShow(const std::string& name, std::string* error = nullptr);
    // Removing a show also drops its playlist entries (an entry naming
    // a deleted show could never run); nothing else references shows.
    bool removeShow(const Id& showId, std::string* error = nullptr);
    bool renameShow(const Id& showId, const std::string& name, std::string* error = nullptr);
    bool setShowNotes(const Id& showId, const std::string& notes, std::string* error = nullptr);
    // Lookups return nullptr when the Id is unknown. The Documents
    // object must outlive these views, and any operation touching the
    // affected show invalidates them.
    const Show* findShow(const Id& showId) const;
    std::vector<Id> showIds() const;  // creation order

    // --- slides ---

    // Insert positions are slide indices; append accepts size().
    // Created ungrouped, with no lines, following the show's default
    // style. Returns the new slide's Id, or "" on failure.
    Id appendSlide(const Id& showId, std::string* error = nullptr);
    Id insertSlide(const Id& showId, size_t index, std::string* error = nullptr);
    bool removeSlide(const Id& showId, const Id& slideId, std::string* error = nullptr);
    // Destination is where the slide lands after removal (moving onto
    // one's own position is a no-op that succeeds).
    bool moveSlide(const Id& showId, const Id& slideId, size_t toIndex, std::string* error = nullptr);
    // THE SLIDE'S FIRST TEXT BOX (task 6.17a). Both single-text operations
    // address box 0 in draw order, which is the only box a slide written
    // before multiple boxes existed has: the operator-facing text editor and
    // every old service keep working through them unchanged, and a slide
    // authored down to no boxes at all has nothing for them to edit, which is
    // refused rather than silently creating one.
    bool setSlideLines(const Id& showId, const Id& slideId,
                       const std::vector<std::string>& lines, std::string* error = nullptr);
    bool setSlideNotes(const Id& showId, const Id& slideId, const std::string& notes,
                       std::string* error = nullptr);
    // The first box's authored geometry in the 1920x1080 canvas space
    // (task 4.3): every value finite, width and height positive. The
    // renderer wraps at the width and centres the text in the box.
    bool setSlideGeometry(const Id& showId, const Id& slideId, double x, double y,
                          double width, double height, std::string* error = nullptr);

    // --- slide items (tasks 6.17a/6.17b, review R17) ---
    //
    // The only way to add, move or delete one of a slide's stack entries.
    // `item.id` is IGNORED: the store issues the id, so a clipboard, a
    // template or a half-built undo snapshot can never smuggle a duplicate
    // identity in. An image item must name an existing asset whose file is
    // not a video — the worker composites stills in the foreground, and an
    // item that could never compile is refused here rather than at present
    // time. A text item must name no asset. The new item goes on TOP of the
    // stack (drawn last), which is what an operator adding one to a composed
    // slide expects; reorder it with moveItem. Returns the new item's id,
    // or "" on failure.
    Id addItem(const Id& showId, const Id& slideId, const SlideItem& item,
               std::string* error = nullptr);
    bool removeItem(const Id& showId, const Id& slideId, const Id& itemId,
                    std::string* error = nullptr);
    // Words belong to a text item: naming an image item here is refused,
    // not silently ignored — the operator typed something somewhere it can
    // never draw.
    bool setItemLines(const Id& showId, const Id& slideId, const Id& itemId,
                      const std::vector<std::string>& lines, std::string* error = nullptr);
    bool setItemGeometry(const Id& showId, const Id& slideId, const Id& itemId,
                         double x, double y, double width, double height,
                         std::string* error = nullptr);
    // Reorders the DRAW stack: index 0 is painted first. As with moveSlide,
    // positions name destinations in the FINAL list, so the ceiling is the
    // unchanged count and the item's own slot is a legal no-op that succeeds.
    bool moveItem(const Id& showId, const Id& slideId, const Id& itemId, size_t toIndex,
                  std::string* error = nullptr);

    // --- groups ---

    // A group exists exactly while some slide of the show carries its
    // label; labels are compared case-sensitively. Setting "" ungroups.
    bool setSlideGroup(const Id& showId, const Id& slideId, const std::string& group,
                       std::string* error = nullptr);
    // Retags every slide of the show; the first slide of the renamed
    // group keeps its position in group order. Succeeds only if some
    // slide currently carries `from`.
    bool renameGroup(const Id& showId, const std::string& from, const std::string& to,
                     std::string* error = nullptr);
    // Dissolves the group: its slides become ungrouped. Returns false
    // (not "already done") when no slide carries `group`.
    bool dissolveGroup(const Id& showId, const std::string& group, std::string* error = nullptr);
    // The show's group labels in order of first appearance, slides in
    // presentation order.
    std::vector<std::string> groups(const Id& showId) const;
    // Slides of one group, presentation order; empty when unknown.
    std::vector<Id> slidesInGroup(const Id& showId, const std::string& group) const;

    // --- styles ---

    // Colours must be "#rgb" or "#rrggbb" (hex); backgroundGradient is
    // that or empty for a flat background (6.17c3 — it is a whole new
    // statement about the background, so every caller states it); fontSize
    // must be finite and >= 0; align one of "left"/"center"/"right";
    // lineSpacing a finite line-box multiplier in 0.25..8. Returns the new
    // style's Id, or "" on failure.
    Id createStyle(const std::string& name, const std::string& textColor,
                   const std::string& backgroundColor,
                   const std::string& backgroundGradient, const std::string& fontFamily,
                   double fontSize, const std::string& align, double lineSpacing,
                   std::string* error = nullptr);
    bool removeStyle(const Id& styleId, std::string* error = nullptr);
    bool renameStyle(const Id& styleId, const std::string& name, std::string* error = nullptr);
    bool updateStyle(const Id& styleId, const std::string& textColor,
                     const std::string& backgroundColor,
                     const std::string& backgroundGradient, const std::string& fontFamily,
                     double fontSize, const std::string& align, double lineSpacing,
                     std::string* error = nullptr);
    // Points a show's default (and therefore its unstyled slides) at a
    // style; "" clears back to none.
    bool setShowDefaultStyle(const Id& showId, const Id& styleId, std::string* error = nullptr);
    // Assigns a per-slide override; "" follows the show default.
    bool setSlideStyle(const Id& showId, const Id& slideId, const Id& styleId,
                       std::string* error = nullptr);
    const Style* findStyle(const Id& styleId) const;
    std::vector<Id> styleIds() const;  // creation order
    // Resolved appearance of one slide: its override when set, else the
    // show default, else nullptr.
    const Style* styleForSlide(const Id& showId, const Id& slideId) const;

    // --- assets ---

    // Records a media file. Neither name nor path is touched on disk —
    // saveService() is where bytes get copied into the package — so an
    // add of a currently-missing path succeeds and fails at save with
    // the asset named. The name must be non-blank; the path must be
    // non-empty but is never trimmed (spaces are legal in paths).
    // Returns the new asset's Id, or "" on failure.
    Id addAsset(const std::string& name, const std::string& sourcePath,
                std::string* error = nullptr);
    // Drops the asset and clears every slide's background reference.
    bool removeAsset(const Id& assetId, std::string* error = nullptr);
    // Points a slide's background at an asset; "" means no background.
    bool setSlideBackground(const Id& showId, const Id& slideId, const Id& assetId,
                            std::string* error = nullptr);
    // How the slide's background picture fits the canvas (6.17c1). The
    // choice is legal with or without a picture — it is the slide's standing
    // answer, not a property of the file behind it — so the only refusal is
    // the unknown slide.
    bool setSlideBackgroundFit(const Id& showId, const Id& slideId, BackgroundFit fit,
                               std::string* error = nullptr);
    const Asset* findAsset(const Id& assetId) const;
    std::vector<Id> assetIds() const;  // creation order
    // Retargets assets to the files that now own their bytes (task 6.12,
    // review R7): each rebind applies to the asset named by id, and only
    // while its sourcePath is still `from` — an asset re-imported or
    // retargeted after the save keeps its newer path. Returns how many
    // assets were retargeted. Purely in-memory; the bytes are storage's
    // business, and saveServiceAndAdopt() is what made them durable.
    std::size_t adoptAssets(const std::vector<AssetRebind>& rebinds);

    // --- playlists ---

    Id createPlaylist(const std::string& name, std::string* error = nullptr);
    bool removePlaylist(const Id& playlistId, std::string* error = nullptr);
    bool renamePlaylist(const Id& playlistId, const std::string& name, std::string* error = nullptr);
    // Appends (or inserts at index; size() appends) a run of the show.
    // The same show may appear more than once. Returns the entry Id,
    // or "" on failure.
    Id addPlaylistEntry(const Id& playlistId, const Id& showId, const std::string& section,
                        std::string* error = nullptr);
    Id insertPlaylistEntry(const Id& playlistId, const Id& showId, const std::string& section,
                           size_t index, std::string* error = nullptr);
    bool removePlaylistEntry(const Id& playlistId, const Id& entryId, std::string* error = nullptr);
    bool movePlaylistEntry(const Id& playlistId, const Id& entryId, size_t toIndex,
                           std::string* error = nullptr);
    bool setPlaylistEntrySection(const Id& playlistId, const Id& entryId,
                                 const std::string& section, std::string* error = nullptr);
    const Playlist* findPlaylist(const Id& playlistId) const;
    std::vector<Id> playlistIds() const;  // creation order

private:
    // storage.cpp reconstructs a loaded model wholesale (ids and all)
    // after validating the file; operations stay the only mutation path
    // for everything else. History snapshots whole states and needs to
    // keep the id counter monotonic across restores.
    friend class ServiceCodec;
    friend class History;

    Show* findShow_mut(const Id& showId);
    Style* findStyle_mut(const Id& styleId);
    Playlist* findPlaylist_mut(const Id& playlistId);
    // Index of the slide/entry within its container, or -1.
    static int findSlideIndex(const Show& show, const Id& slideId);
    static int findEntryIndex(const Playlist& playlist, const Id& entryId);
    // Index of the box within its slide's DRAW order, or -1 (task 6.17a).
    static int findItemIndex(const Slide& slide, const Id& itemId);
    // "#rgb"/"#rrggbb" only; error names the bad value.
    static bool validColor(const std::string& color, std::string* error);
    Id nextId(const char* kind);

    std::vector<Show> fShows;
    std::vector<Style> fStyles;
    std::vector<Asset> fAssets;
    std::vector<Playlist> fPlaylists;
    unsigned long long fNextOrdinal = 1;  // store-wide, ids never recycle
};

}  // namespace openshow::domain
