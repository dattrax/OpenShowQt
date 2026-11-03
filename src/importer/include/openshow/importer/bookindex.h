// The canon as a built-in index (milestone BI-F, task BIF.3): the operator
// names a book and the reference field finishes the sentence. This is a
// table, not a service — the book list never comes off the network, and
// the suggester never gates a lookup: text that matches nothing still
// looks up exactly as typed.
//
// Matching is case-, whitespace- and ordinal-insensitive: "1 cor", "1cor",
// "i cor" and "1st cor" all land on 1 Corinthians; "jn" lands on John. The
// candidate is the LEADING book part of what was typed — a trailing
// chapter/verse ("1 cor 1:1-6") does not spoil the match and is carried
// whole into the completion.
//
// Qt-free on purpose, like the rest of openshow::importer: the table and
// the matcher are testable with no network, no widget and no display.
#ifndef OPENSHOW_IMPORTER_BOOKINDEX_H
#define OPENSHOW_IMPORTER_BOOKINDEX_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openshow::importer {

// One suggested book for what the operator typed so far. `name` is the
// book's full name; `completion` is what the field becomes if this row is
// taken — the name followed by whatever chapter/verse text already
// followed the book part.
struct BookSuggestion {
    std::string name;
    std::string completion;
};

// Books whose name-or-alias the typed text is a prefix of, in canon order
// (so numbered books offer 1 before 2 before 3), at most `limit` rows.
// Empty for blank input or nothing matched — an empty answer is the
// suggester saying "I have no idea", which the caller must treat as "hide
// the dropdown", never as "refuse the lookup".
std::vector<BookSuggestion> suggestBooks(std::string_view typed, std::size_t limit = 8);

// The book a typed leading part names outright (after the same
// normalization the suggester uses); nullopt when nothing matches. This
// exists for later milestones that need a book ID rather than a list —
// BIF.3's field itself is deliberately not gated on it.
std::optional<std::string> canonicalBook(std::string_view typed);

}  // namespace openshow::importer

#endif  // OPENSHOW_IMPORTER_BOOKINDEX_H
