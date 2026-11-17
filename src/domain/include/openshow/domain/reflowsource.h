/*
 * The bounded, ordered source a reflow is allowed to read (task PT.4).
 *
 * Reflow must never fetch words from the Library or a Bible provider at
 * apply time. It may only re-paginate the words this service already
 * contains, and it needs to know which authored blocks (verses, sections)
 * those words came from. That knowledge is a VALUE stored on the show:
 * source kind, ordered block identities with original-case lines, the
 * passage provenance a selected-verse Add used, and a fingerprint of the
 * service text projection it describes.
 *
 * The fingerprint is what makes a stored source HONEST. A style-only edit
 * leaves the projection untouched, so the source stays usable. Any text,
 * order or section edit changes the fingerprint, and the stored source
 * stops matching — callers then fall back to the conservative
 * current-word source below instead of pretending the old blocks still
 * describe the slides.
 *
 * This is authored service content, not a chapter/provider cache and not an
 * installed Bible database: nothing here can answer a lookup, and a saved
 * service remains self-contained.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace openshow::domain {

class Documents;
struct Show;

// Where a source came from, as far as the service can honestly say.
enum class ReflowSourceKind {
    // A song prepared from a shared-catalog record (the id is provenance).
    kCatalogSong,
    // A song whose service copy is the only source there is.
    kLocalSong,
    // A scripture passage added from a lookup; the verse labels are the
    // ones that Add actually used.
    kScripture,
};

// One authored block: a verse, a chorus, or a slide's verse label.
struct ReflowSourceBlock {
    // Section/verse identity ("Verse 1", "Chorus", "John 3:16"). Empty is
    // legal for an authored song with no section names.
    std::string label;
    // Original-case lines, in authored order. Blank lines are preserved.
    std::vector<std::string> lines;

    bool operator==(const ReflowSourceBlock&) const = default;
};

struct ReflowSource {
    ReflowSourceKind kind = ReflowSourceKind::kLocalSong;
    // Provenance, empty where it does not apply. A catalog id is not a
    // dependency: the service keeps its words when the Library is absent.
    std::string catalogItemId;
    std::string reference;
    std::string translation;
    std::string attribution;
    std::vector<ReflowSourceBlock> blocks;
    // The projection this describes, as reflowFingerprint() spells it.
    std::string projectionFingerprint;

    bool operator==(const ReflowSource&) const = default;
};

// Budgets for one show's source. A service is a portable package: a source
// that could hold a chapter and a half of arbitrary text is not authored
// content but an unbounded cache, so the limits refuse by name.
inline constexpr std::size_t kMaxReflowSourceBlocks = 200;
inline constexpr std::size_t kMaxReflowSourceLines = 2000;
inline constexpr std::size_t kMaxReflowSourceLineBytes = 4000;
inline constexpr std::size_t kMaxReflowSourceLabelChars = 512;
inline constexpr std::size_t kMaxReflowSourceReferenceChars = 512;

// The one canonical fingerprint of an ordered text projection. Fixed,
// length-prefixed input over labels and lines, so equal projections always
// produce equal strings and any edit to a word, line count, order or
// section label produces a different one. Returns 16 lowercase hex digits.
std::string reflowFingerprint(const std::vector<ReflowSourceBlock>& blocks);

// The text projection a show currently presents: one ordered block per
// slide that carries a primary text item, with the slide's section label
// (`group`, else `notes`) and the item's authored lines. Wordless cues and
// picture-only slides contribute no block; their eligibility is a separate
// question. Empty for an unknown show or a show with no text.
std::vector<ReflowSourceBlock> showTextProjection(const Documents& documents,
                                                  const std::string& showId);

// Whether a stored source still describes the show's current projection.
// This is the ONLY usability test: a matching fingerprint means the saved
// blocks still account for every word on the slides.
bool reflowSourceMatches(const Documents& documents, const std::string& showId,
                         const ReflowSource& source);

// The conservative source for a legacy or edited item: the words this
// service shows NOW, in their existing section order and authored line
// breaks. For a scripture show without per-slide labels the blocks carry
// the whole known passage reference rather than an invented verse range.
// Never consults the Library or a provider.
ReflowSource sourceFromShowProjection(const Documents& documents,
                                      const std::string& showId);

}  // namespace openshow::domain
