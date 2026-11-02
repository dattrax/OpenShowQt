/*
 * The document model (task 2.1): shows, slides, groups, themes and
 * playlists as plain data plus the operations every edit goes through.
 *
 * The vocabulary follows docs/reference.md's mapping table: a show is a
 * deck of slides, a group is a chorus/verse label carried by slides, a
 * playlist (FreeShow's "project") is an ordered run of shows with
 * optional section markers. Themes are named appearance records; 2.1
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
#include <optional>
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

// A rectangle in the 1920x1080 authored canvas (docs/reference.md §7),
// spelled the way a SlideItem spells its own box. The theme placement
// (amendment 2026-11-01, themes plan) is one of these: where the slide's
// PRIMARY lyric box lands. Absent is a real answer meaning "no opinion" —
// the slide's authored box then draws where it was authored.
struct BoxRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const BoxRect& other) const {
        return x == other.x && y == other.y && width == other.width &&
               height == other.height;
    }
};

// What one slide's operator changed for itself (task TH.2, themes plan).
// Sparse by construction: a field is present exactly when the slide
// customized it, and resolution falls through per property — a slide that
// tweaked only the background still takes the current theme's font. The
// empty strings mean different things per field, and only where they are
// answers: backgroundGradient "" is "explicitly flat over a graded theme"
// and fontFamily "" is "no family, whatever the renderer picks"; an empty
// text colour, align or number is a hand-edit, refused.
struct SlideCustomization {
    std::optional<std::string> textColor;
    std::optional<std::string> backgroundColor;
    std::optional<std::string> backgroundGradient;
    std::optional<std::string> fontFamily;
    std::optional<double> fontSize;
    std::optional<std::string> align;
    std::optional<double> lineSpacing;
    // Where THIS slide's primary lyric box goes, whatever the themes say
    // (amendment 2026-11-01). One unit — a half-placed box is not a thing
    // an operator drags.
    std::optional<BoxRect> placement;

    bool operator==(const SlideCustomization& other) const {
        return textColor == other.textColor && backgroundColor == other.backgroundColor &&
               backgroundGradient == other.backgroundGradient &&
               fontFamily == other.fontFamily && fontSize == other.fontSize &&
               align == other.align && lineSpacing == other.lineSpacing &&
               placement == other.placement;
    }

    bool empty() const {
        return !textColor.has_value() && !backgroundColor.has_value() &&
               !backgroundGradient.has_value() && !fontFamily.has_value() &&
               !fontSize.has_value() && !align.has_value() && !lineSpacing.has_value() &&
               !placement.has_value();
    }
    // Remove one customized field; reverting "this thing" is a key away.
    void clearField(const std::string& field) {
        if (field == "textColor") {
            textColor.reset();
        } else if (field == "backgroundColor") {
            backgroundColor.reset();
        } else if (field == "backgroundGradient") {
            backgroundGradient.reset();
        } else if (field == "fontFamily") {
            fontFamily.reset();
        } else if (field == "fontSize") {
            fontSize.reset();
        } else if (field == "align") {
            align.reset();
        } else if (field == "lineSpacing") {
            lineSpacing.reset();
        } else if (field == "placement") {
            placement.reset();
        }
    }
    bool hasField(const std::string& field) const {
        if (field == "textColor") {
            return textColor.has_value();
        }
        if (field == "backgroundColor") {
            return backgroundColor.has_value();
        }
        if (field == "backgroundGradient") {
            return backgroundGradient.has_value();
        }
        if (field == "fontFamily") {
            return fontFamily.has_value();
        }
        if (field == "fontSize") {
            return fontSize.has_value();
        }
        if (field == "align") {
            return align.has_value();
        }
        if (field == "lineSpacing") {
            return lineSpacing.has_value();
        }
        if (field == "placement") {
            return placement.has_value();
        }
        return false;
    }
};

// The names the customization keys — and the UI's per-field revert — use.
// "placement" (amendment 2026-11-01) is one name for the whole rectangle:
// dragging a box changes four numbers, and reverting half a box is not a
// thing an operator wants or storage should carry.
inline bool isCustomizableField(const std::string& field) {
    return field == "textColor" || field == "backgroundColor" ||
           field == "backgroundGradient" || field == "fontFamily" ||
           field == "fontSize" || field == "align" || field == "lineSpacing" ||
           field == "placement";
}

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
    // Theme override; empty follows the show's default theme.
    Id themeId;
    // What this slide's operator changed for itself (task TH.2). Sparse —
    // only the tweaked fields — and it wins over every theme layer, field
    // by field, until it is removed.
    SlideCustomization customizations;
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

// Where an online Bible import's words came from (task BI.3). This is the
// schedule item's whole provenance: the reference the operator typed, the
// translation the PAGE answered with, and the licence text the translation
// requires carried beside the words. It is metadata only — the slide texts
// are ordinary schedule content, and this record is what keeps them
// attributable after the network answer is long gone.
struct ScriptureProvenance {
    std::string reference;    // as looked up, e.g. "1 Corinthians 1:1-6"
    std::string translation;  // the version the passage was fetched in, e.g. "NIV"
    std::string attribution;  // the publisher/copyright block, as the page gave it

    bool operator==(const ScriptureProvenance&) const = default;
};

struct Show {
    Id id;
    std::string name;
    std::string notes;
    std::vector<Slide> slides;  // presentation order
    // Theme new slides implicitly use; empty means "no theme yet".
    Id defaultThemeId;
    // A schedule-owned presentation (task BI.3): a real show that lives on
    // today's schedule but is NEVER a Library row — no Library list, count
    // or search sees it, and removing it leaves nothing behind. The online
    // Bible import creates only these.
    bool scheduleOwned = false;
    // Set exactly when the show came from a passage lookup; scheduleOwned
    // alone can also cover future schedule-only items without provenance.
    std::optional<ScriptureProvenance> scripture;
};

// A named appearance. Colours are "#rgb" or "#rrggbb"; a zero font size
// means "renderer default". Scene compilation (2.4) maps these onto
// Skia paint state; 2.1 only validates syntax it can actually check.
// The renderer's line-box factor doubles as the domain default, so a
// theme created before typography existed keeps drawing as it did.
inline constexpr double kDefaultLineSpacing = 1.25;

inline bool isThemeAlign(const std::string& align) {
    return align == "left" || align == "center" || align == "right";
}

struct Theme {
    Id id;
    std::string name;
    std::string textColor;
    std::string backgroundColor;
    // Second background colour (task 6.17c3, review R17). Empty — the
    // default — paints the flat backgroundColor it always did. When set,
    // the background is a vertical gradient from backgroundColor at the
    // top to this colour at the bottom: OpenShow's reference editor makes
    // backgrounds with exactly two colours and a downward axis, and the
    // field belongs to the THEME because that is where the background
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
    // Where a slide wearing this theme puts its PRIMARY lyric box
    // (amendment 2026-11-01, themes plan). Absent is no opinion: wearing
    // this theme never moves a slide's words. When set, the rectangle
    // travels the same chain as every other property — a slide that wants
    // its own spot customizes its placement and reverts it to follow again.
    std::optional<BoxRect> placement;
};

// One slide's appearance after the whole chain has been walked
// (task TH.2): per-slide customization first, then the service's current
// theme, then the slide's theme, then the show's default theme. A property
// no layer defined comes back empty ("" for strings, 0.0 for the size and
// the renderer's own line-box marker for lineSpacing): the scene compile's
// built-in defaults fill those, so the domain never has to claim to know
// what the renderer's white or font is.
struct ResolvedAppearance {
    std::string textColor;
    std::string backgroundColor;
    std::string backgroundGradient;
    std::string fontFamily;
    double fontSize = 0.0;
    std::string align;   // empty means "no layer said"; the compile centres
    double lineSpacing = kDefaultLineSpacing;
    // The primary lyric box's rectangle, resolved like the rest: the slide's
    // placement customization beats a themed rectangle, and a themed
    // rectangle beats the slide's authored box. Empty means no layer spoke,
    // so the slide draws its own authored box (the built-in lyric default
    // where the slide is untouched).
    std::optional<BoxRect> placement;
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
    // The Default theme (operator amendment 2026-11-02): one theme that is
    // always there, so a document always has a starting look and a deleted
    // or hand-nicked theme never leaves a document looking like nothing.
    // The application seeds it when a loaded service lacks it and refuses
    // to delete it; an operator may rename and restyle it like any other
    // theme. Its id is not drawn from the ordinal sequence, so a document
    // written before this existed cannot already hold one by accident.
    static inline constexpr const char kDefaultThemeId[] = "default";
    // A fresh document already carries its Default theme (operator
    // amendment 2026-11-02); the loader re-ensures after decoding, so the
    // entry exists in every model the application can hold.
    Documents();
    // The look the Default theme is seeded with: white on black, the
    // platform font at the renderer's own size, centred at the default
    // line spacing. Same numbers, two reasons: it is the look every
    // un-themed slide already draws, so seeding the Default theme moves
    // nothing, and the operator's own changes to it stick in the document
    // rather than being reset.
    static Theme defaultTheme();
    // Seed the Default theme when the service does not carry its id, so a
    // file hand-edited into its absence still opens on a look. An existing
    // theme with that id, whatever its author put in it, is left alone.
    // Idempotent.
    void ensureDefaultTheme();

    // --- shows ---

    // Names must be non-blank; duplicates are allowed (ids distinguish).
    // Returns the new show's Id, or "" on failure.
    Id createShow(const std::string& name, std::string* error = nullptr);
    // The online Bible import's destination shape (task BI.3): a
    // schedule-owned presentation carrying its provenance. Slides join
    // through appendSlide/setSlideLines like any other show's; what makes
    // it scripture is the provenance and the Library exclusion, not a
    // second kind of slide. The reference and the translation must be
    // there — an unattributable import is the one thing the plan forbids
    // persisting; the attribution may be empty (a public-domain version).
    Id createScriptureShow(const std::string& name, const ScriptureProvenance& scripture,
                           std::string* error = nullptr);
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
    // theme. Returns the new slide's Id, or "" on failure.
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

    // --- themes ---

    // Colours must be "#rgb" or "#rrggbb" (hex); backgroundGradient is
    // that or empty for a flat background (6.17c3 — it is a whole new
    // statement about the background, so every caller states it); fontSize
    // must be finite and >= 0; align one of "left"/"center"/"right";
    // lineSpacing a finite line-box multiplier in 0.25..8. Returns the new
    // theme's Id, or "" on failure.
    Id createTheme(const std::string& name, const std::string& textColor,
                   const std::string& backgroundColor,
                   const std::string& backgroundGradient, const std::string& fontFamily,
                   double fontSize, const std::string& align, double lineSpacing,
                   std::string* error = nullptr);
    bool removeTheme(const Id& themeId, std::string* error = nullptr);
    bool renameTheme(const Id& themeId, const std::string& name, std::string* error = nullptr);
    bool updateTheme(const Id& themeId, const std::string& textColor,
                     const std::string& backgroundColor,
                     const std::string& backgroundGradient, const std::string& fontFamily,
                     double fontSize, const std::string& align, double lineSpacing,
                     std::string* error = nullptr);
    // Give a theme a lyric-box placement (amendment 2026-11-01, themes
    // plan). The rectangle is vetted like a SlideItem's box: finite, with
    // positive extent. Setting it never moves a slide on its own; it
    // changes what the chain resolves for slides that follow this theme
    // and have not customized their own placement.
    bool setThemePlacement(const Id& themeId, const BoxRect& rect, std::string* error = nullptr);
    // Take the placement back: the theme keeps its colours and typography
    // but stops placing words, so wearing slides return to their authored
    // boxes (unless they customized placement). Refuses a theme that has
    // no placement to remove.
    bool clearThemePlacement(const Id& themeId, std::string* error = nullptr);
    // Points a show's default (and therefore its un-themed slides) at a
    // theme; "" clears back to none.
    bool setShowDefaultTheme(const Id& showId, const Id& themeId, std::string* error = nullptr);
    // Assigns a per-slide override; "" follows the show default.
    bool setSlideTheme(const Id& showId, const Id& slideId, const Id& themeId,
                       std::string* error = nullptr);
    const Theme* findTheme(const Id& themeId) const;
    std::vector<Id> themeIds() const;  // creation order
    // Resolved appearance of one slide: its override when set, else the
    // show default, else nullptr.
    const Theme* themeForSlide(const Id& showId, const Id& slideId) const;

    // --- themes: current theme, customization and the full chain
    // (task TH.2, themes plan) ---

    // The one theme the whole service wears right now; "" means none and
    // slides fall back to their own themes. The theme must exist.
    bool setCurrentTheme(const Id& themeId, std::string* error = nullptr);
    const Id& currentThemeId() const { return fCurrentThemeId; }

    // Customization: what a slide's operator changed for itself.
    // setSlideCustomizations REPLACES the whole set — a field the caller
    // leaves unset is not customized — so the UI edits one field by
    // reading the set, changing one key and writing it back. Values are
    // vetted by the same rules a theme's are; the one empty string that
    // means something is a backgroundGradient "" (explicitly flat over a
    // graded theme) or a fontFamily "" (no family). removeSlideCustomizations
    // restores one slide and refuses when it carried none;
    // removeShowCustomizations clears every slide of a show and refuses
    // when no slide carried any — an undo step costs what it changes.
    bool setSlideCustomizations(const Id& showId, const Id& slideId,
                                const SlideCustomization& customizations,
                                std::string* error = nullptr);
    bool removeSlideCustomizations(const Id& showId, const Id& slideId,
                                   std::string* error = nullptr);
    bool removeShowCustomizations(const Id& showId, std::string* error = nullptr);

    // One mutation naming every slide it touches (task TH.5's apply): the
    // theme (or "" to follow the show again) lands on exactly the slides
    // named, and the whole move is one undoable change. Every id must be
    // a slide of THIS show; an empty list refuses — a bulk that names
    // nothing is a wrong click, not a success.
    bool applyThemeToSlides(const Id& showId, const std::vector<Id>& slideIds,
                            const Id& themeId, std::string* error = nullptr);

    // The whole chain walked for one slide: customization -> current theme
    // -> slide theme -> show default -> (nobody) — see ResolvedAppearance.
    ResolvedAppearance appearanceForSlide(const Id& showId, const Id& slideId) const;

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
    Theme* findTheme_mut(const Id& themeId);
    Playlist* findPlaylist_mut(const Id& playlistId);
    // Index of the slide/entry within its container, or -1.
    static int findSlideIndex(const Show& show, const Id& slideId);
    static int findEntryIndex(const Playlist& playlist, const Id& entryId);
    // Index of the box within its slide's DRAW order, or -1 (task 6.17a).
    static int findItemIndex(const Slide& slide, const Id& itemId);
    // "#rgb"/"#rrggbb" only; error names the bad value.
    static bool validColor(const std::string& color, std::string* error);
    // The customization fields a caller writes are vetted by the same
    // rules a theme's are (task TH.2); the empty strings that mean
    // something (a flat gradient, no family) pass.
    static bool validCustomizations(const SlideCustomization& customizations,
                                    std::string* error);
    Id nextId(const char* kind);

    std::vector<Show> fShows;
    std::vector<Theme> fThemes;
    // The service's one current theme (task TH.2); "" is none.
    Id fCurrentThemeId;
    std::vector<Asset> fAssets;
    std::vector<Playlist> fPlaylists;
    unsigned long long fNextOrdinal = 1;  // store-wide, ids never recycle
};

}  // namespace openshow::domain
