/*
 * Plain-lyrics parser for the clipboard song import (task SI.1).
 *
 * Pure, Qt-free and network-free: the frontend reads the normal clipboard
 * only on an explicit Paste, hands the text here, and gets back a draft the
 * operator can edit before it touches the catalog.
 *
 * Rules, deliberately conservative:
 *   - CRLF and lone CR become "\n"; Unicode letters, punctuation and the
 *     authored line order are preserved byte for byte.
 *   - Blank lines end a stanza. Empty stanzas never become slides.
 *   - A line that is ONLY a known section label — "[Verse 1]", "Chorus",
 *     "Bridge 2", "Post-Chorus" — opens a labelled stanza and is not itself
 *     sung. Ordinary text that merely contains the word "chorus" stays
 *     lyrics; nothing guesses labels out of prose.
 *   - The first non-blank line is suggested as the title when it is neither
 *     a section label nor a lyric that continues straight into the first
 *     section. It is NOT removed from the lyrics (the plan's rule): the
 *     catalog keeps the whole pasted text, and the planner may give that
 *     first stanza its own slide.
 *   - A trailing blank-separated block that carries CCLI / © / SongSelect
 *     markers is attribution, not lyrics. Its CCLI number, author line and
 *     remaining copyright/licence lines fill the record's reporting fields
 *     and are removed from the slide words. If no such block exists,
 *     nothing is invented: the fields stay empty.
 *   - The input is bounded by named limits; an over-limit paste is refused
 *     with its own sentence, never silently truncated.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "openshow/catalog/catalog.h"

namespace openshow::importer {

// The hard ceiling one paste may carry. Generous for any song and small
// enough that a mis-pasted document cannot become a giant model or JSON
// file; the catalog's own stanza/line caps still apply afterwards.
inline constexpr std::size_t kMaxPastedBytes = 256 * 1024;
inline constexpr std::size_t kMaxPastedLines = 8000;

struct LyricsDraft {
    bool ok = false;
    std::string error;  // the operator's sentence; empty when ok
    // The suggested title ("" when the text names none). The import page
    // lets the operator replace it; Add requires it non-blank.
    std::string title;
    // Canonical stanzas, in authored order, labels kept.
    std::vector<openshow::catalog::Stanza> stanzas;
    // The attribution footer, when one was found.
    openshow::catalog::SongMetadata metadata;
    // True when a metadata footer was recognised and removed from the
    // words (so the page can say "CCLI 7231931 found" instead of guessing).
    bool hasMetadata = false;
};

// Parse a pasted song. Never throws; a refusal is ok=false with a sentence
// naming the problem and empty stanzas.
LyricsDraft parseLyrics(const std::string& plainText);

// True when `line` is only a section label; `label` receives the cleaned
// label ("Verse 1", "Chorus"). Exposed so the fixtures can pin exactly
// which lines count as labels and which do not.
bool looksLikeSectionLabel(const std::string& line, std::string* label);

// UTF-8-safe classification of the attribution block, exposed for the
// parser's own fixtures (and shared by the metadata editor if it ever wants
// to re-read a pasted footer).
bool lineNamesCcliSong(const std::string& line, std::string* digits);
bool lineIsCopyright(const std::string& line);
bool lineIsLicence(const std::string& line);

}  // namespace openshow::importer
