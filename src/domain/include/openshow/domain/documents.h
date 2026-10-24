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
 * ("show1", "slide2", ... — a kind prefix plus a store-wide counter, so
 * ids stay unique across kinds and are reproducible in tests). Slides
 * live inside their show's vector (presentation order is vector order)
 * but keep their Id, which operations use; Ids never recycle.
 *
 * Failure contract, matching the rest of the project: operations return
 * false and fill the optional error string when an argument is unknown
 * (bad Id), empty (names) or out of range (indices). A failed operation
 * changes nothing.
 */

#pragma once

#include <string>
#include <vector>

namespace openshow::domain {

// Identifies one object within one Documents instance. Opaque beyond
// uniqueness; persistence (2.2) serialises it verbatim.
using Id = std::string;

// One line of slide text. Blank strings are meaningful (stanza breaks)
// and survive untouched.
struct Slide {
    Id id;
    // Group label ("Chorus", "Verse", ...). Empty means the slide is
    // ungrouped, which is the state a new slide starts in.
    std::string group;
    std::vector<std::string> lines;
    std::string notes;
    // Style override; empty follows the show's default style.
    Id styleId;
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
struct Style {
    Id id;
    std::string name;
    std::string textColor;
    std::string backgroundColor;
    std::string fontFamily;
    double fontSize = 0.0;
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
    bool setSlideLines(const Id& showId, const Id& slideId,
                       const std::vector<std::string>& lines, std::string* error = nullptr);
    bool setSlideNotes(const Id& showId, const Id& slideId, const std::string& notes,
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

    // Colours must be "#rgb" or "#rrggbb" (hex); fontSize must be
    // finite and >= 0. Returns the new style's Id, or "" on failure.
    Id createStyle(const std::string& name, const std::string& textColor,
                   const std::string& backgroundColor, const std::string& fontFamily,
                   double fontSize, std::string* error = nullptr);
    bool removeStyle(const Id& styleId, std::string* error = nullptr);
    bool renameStyle(const Id& styleId, const std::string& name, std::string* error = nullptr);
    bool updateStyle(const Id& styleId, const std::string& textColor,
                     const std::string& backgroundColor, const std::string& fontFamily,
                     double fontSize, std::string* error = nullptr);
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
    Show* findShow_mut(const Id& showId);
    Style* findStyle_mut(const Id& styleId);
    Playlist* findPlaylist_mut(const Id& playlistId);
    // Index of the slide/entry within its container, or -1.
    static int findSlideIndex(const Show& show, const Id& slideId);
    static int findEntryIndex(const Playlist& playlist, const Id& entryId);
    // "#rgb"/"#rrggbb" only; error names the bad value.
    static bool validColor(const std::string& color, std::string* error);
    Id nextId(const char* kind);

    std::vector<Show> fShows;
    std::vector<Style> fStyles;
    std::vector<Playlist> fPlaylists;
    unsigned long long fNextOrdinal = 1;  // store-wide, ids never recycle
};

}  // namespace openshow::domain
