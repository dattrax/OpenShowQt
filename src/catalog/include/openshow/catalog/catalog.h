/*
 * The shared song catalog (task SL.1).
 *
 * The Library is application data shared by every service, not a view of
 * whichever service happens to be open. This header owns its model: stable
 * global item ids, a closed pair of categories, independent record and
 * content revisions, and the operations every edit goes through.
 *
 * Why a separate store from Documents: a service is a portable package of
 * the things it will play tonight. The catalog is the operator's own
 * library and outlives any one service file. Keeping the two apart is what
 * lets a service be saved, loaded, copied or played with the catalog
 * absent, while the catalog keeps its songs across restarts.
 *
 * Categories are METADATA, not directories and not content kinds. The two
 * names are fixed by the operator brief — songs and presentations — and a
 * move changes only which branch lists the item: ids, revisions, titles,
 * lyrics, stanzas, metadata and layout stay exactly as they were. Duplicate
 * titles are distinct items; nothing here merges by title.
 *
 * Revisions: recordRevision changes for ANY mutation (a rename, a
 * re-categorisation, a lyric edit). contentRevision changes only when the
 * content or layout does, which is what a preparation step keys its
 * staleness check on. A category move therefore bumps the record and leaves
 * the content alone.
 *
 * Lyrics are canonical stanza text, not slides. How many slides a stanza
 * needs is decided later against the current service's Songs theme (SI.2);
 * storing slide boundaries here would freeze one service's typography into
 * the shared library.
 *
 * CCLI and attribution: a song carries the CCLI song number for reporting
 * and the author/copyright-holder fields the operator asked for. They are
 * part of the record, so a service prepared from a catalog song can name
 * them, and a report can read them without opening the words. The sample
 * import's footer ("CCLI Song #…", the writers, the © block) is parsed into
 * these fields rather than becoming slides.
 *
 * Qt-free on purpose, like the domain: the model and its regressions run
 * with no display, no network and no database server.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace openshow::catalog {

// Identifies one catalog item globally. Opaque beyond uniqueness; the
// catalog codec serialises it verbatim and the persistence layer validates
// it as a name, never a path.
using Id = std::string;

// Monotonic per-item counters (see the header comment).
using Revision = unsigned long long;

// The two fixed Library branches. No arbitrary folders, tags or online
// providers: the brief closes this set.
enum class Category { Songs, Presentations };

// The one spelling of the categories, shared by the file and the UI
// adapters so two copies of a name list cannot drift.
inline const char* categoryName(Category category) {
    return category == Category::Presentations ? "presentations" : "songs";
}
inline bool categoryFromName(const std::string& name, Category* category) {
    if (name == "songs") {
        *category = Category::Songs;
        return true;
    }
    if (name == "presentations") {
        *category = Category::Presentations;
        return true;
    }
    return false;
}

// What an item's content IS. Lyrics hold canonical stanzas; a deck (the
// existing authored presentations) will hold its slide document when the
// migration leaf lands. Classification never converts one into the other.
enum class ContentKind { Lyrics };

inline const char* contentKindName(ContentKind kind) {
    (void)kind;
    return "lyrics";
}
inline bool contentKindFromName(const std::string& name, ContentKind* kind) {
    if (name == "lyrics") {
        *kind = ContentKind::Lyrics;
        return true;
    }
    return false;
}

// One stanza as the operator authored it: an optional section label
// ("Verse 1", "Chorus", "Bridge 2", "" for an unlabelled run) and the
// authored lines in order. Blank lines inside a stanza are preserved; the
// parser uses blank lines between stanzas as boundaries and does not put
// them in the stanza. Labels are kept, not guessed away: they are what the
// planner puts in slide notes and what the operator reads.
struct Stanza {
    std::string label;
    std::vector<std::string> lines;

    bool operator==(const Stanza&) const = default;
};

// The reporting and attribution fields the operator requires. `ccli` is
// the song number as digits (empty when the source carried none); the
// authors and the copyright block are free text and may be empty. These
// never draw on a slide — they belong to the record and to reports.
struct SongMetadata {
    std::string ccli;
    std::string authors;
    std::string copyright;

    bool operator==(const SongMetadata&) const = default;
};


struct CatalogItem {
    Id id;
    Category category = Category::Songs;
    std::string title;
    Revision recordRevision = 1;
    Revision contentRevision = 1;
    ContentKind kind = ContentKind::Lyrics;
    // Lyrics content: canonical stanzas in authored order.
    std::vector<Stanza> stanzas;
    SongMetadata metadata;

    bool operator==(const CatalogItem&) const = default;
};

// Named limits on what one song may carry (SI.1's "bounded drafts"). The
// numbers are generous for a song and small enough that a pasted document
// cannot become an unbounded model, a giant JSON file or a runaway
// measurement batch. Refusals name the limit; nothing is silently truncated.
inline constexpr std::size_t kMaxStanzas = 200;
inline constexpr std::size_t kMaxStanzaLines = 500;
inline constexpr std::size_t kMaxLineBytes = 4000;
inline constexpr std::size_t kMaxTitleBytes = 512;
inline constexpr std::size_t kMaxMetadataBytes = 8000;
inline constexpr std::size_t kMaxCcliDigits = 20;

// The shared validation for one lyrics record. A title must be non-blank;
// a song must have at least one stanza with at least one non-blank line;
// every line must be within the byte cap (blank lines are legal); the CCLI
// field, when present, must be digits only (the number, not "CCLI #…");
// metadata fields stay within their cap. Returns false and fills `error`
// with the operator's sentence. This is the ONE validator: create, update,
// the loader and the clipboard parser all quote it, so a record the build
// accepts is a record it can save and honour.
bool validSong(const std::string& title, const std::vector<Stanza>& stanzas,
               const SongMetadata& metadata, std::string* error = nullptr);

// Trim ASCII whitespace from both ends (a small helper the parser and the
// validators share; deliberately not locale-dependent).
std::string trimAscii(const std::string& text);

// True when `text` is blank after ASCII trimming.
bool isBlank(const std::string& text);

// The registry plus every supported catalog edit. Single-threaded by
// contract; the UI controller owns one instance on the UI thread.
class Catalog {
public:
    Catalog() = default;

    // Value equality over the whole store — what the durable-write adapter
    // uses to tell an unchanged verb (a repeated category move, a rename
    // that trims to the same title) from a real edit, so a no-op costs no
    // disk write and no undo step.
    bool operator==(const Catalog&) const = default;

    // --- queries ---

    // nullptr when the id is unknown. The Catalog must outlive the view, and
    // any operation touching the item invalidates it.
    const CatalogItem* find(const Id& id) const;
    // Every item in creation order.
    std::vector<Id> itemIds() const;
    // The items in one category, creation order.
    std::vector<Id> idsInCategory(Category category) const;
    std::size_t count(Category category) const;
    std::size_t size() const { return fItems.size(); }

    // --- edits ---

    // Creates a Songs item. The title, stanzas and metadata are vetted by
    // validSong(); the new item gets a fresh global id and revisions 1/1.
    // Returns the new id, or "" on failure.
    Id createSong(const std::string& title, const std::vector<Stanza>& stanzas,
                  const SongMetadata& metadata, std::string* error = nullptr);

    // Replaces a lyrics item's stanzas AND metadata (metadata is content:
    // the operator corrected the CCLI number, so a prepared service must be
    // told). The title is not touched here — use rename. Both revisions
    // advance. Returns false and changes nothing on a refusal.
    bool updateSong(const Id& id, const std::vector<Stanza>& stanzas,
                    const SongMetadata& metadata, std::string* error = nullptr);

    // Replaces only the reporting/attribution block. contentRevision moves
    // because a report built from a stale CCLI number is wrong.
    bool setMetadata(const Id& id, const SongMetadata& metadata,
                     std::string* error = nullptr);

    // Renames without touching content. A rename advances recordRevision
    // only; a service's prepared snapshot is still valid.
    bool rename(const Id& id, const std::string& title, std::string* error = nullptr);

    // Moves only the classification. Preserves id, revisions' CONTENT half,
    // title, stanzas, metadata and layout; bumps recordRevision. Repeating
    // the move to the current category is a no-op that succeeds (no undo
    // step for a click that changed nothing).
    bool moveToCategory(const Id& id, Category category, std::string* error = nullptr);

    // Drops the record. Callers holding prepared service copies keep them:
    // a service snapshot is self-contained and playable without the catalog.
    bool remove(const Id& id, std::string* error = nullptr);

private:
    // The catalog codec reconstructs a loaded model wholesale (ids and all)
    // after validating the file; operations remain the only mutation path
    // for everything else. History snapshots whole states and keeps the id
    // counter monotonic across restores, exactly as domain::History does.
    friend class CatalogCodec;
    friend class History;

    Id nextId();

    std::vector<CatalogItem> fItems;
    unsigned long long fNextOrdinal = 1;  // global, ids never recycle
};

}  // namespace openshow::catalog
