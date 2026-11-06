#include "openshow/importer/lyrics.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace openshow::importer {

namespace {

using openshow::catalog::Stanza;

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

char lowerAscii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

std::string lowerAscii(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        c = lowerAscii(c);
    }
    return out;
}

bool startsWithInsensitive(const std::string& text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lowerAscii(text[i]) != lowerAscii(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool containsInsensitive(const std::string& text, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lowerAscii(text[i + j]) != lowerAscii(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// "Verse 1", "Chorus", "Pre-Chorus", "Post Chorus", "Bridge 2", "Vamp"…
// The closed set the importer understands. Unknown bracketed text is a
// label too (the operator wrote "[Tag]"), but only when it is bracketed —
// a bare unknown word stays a lyric.
bool isKnownSectionName(const std::string& name) {
    static const char* const kNames[] = {
        "verse",       "chorus",      "bridge",    "pre-chorus",
        "pre chorus",  "post-chorus", "post chorus", "refrain",
        "intro",       "outro",       "ending",    "vamp",
        "tag",         "instrumental", "interlude", "turn",
        "breakdown",   "final",       "finale",    "hook",
    };
    for (const char* candidate : kNames) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

// A bare label: known name, optionally followed by a number ("verse 1")
// or number word ("verse one"). Everything else is prose. Case folded,
// internal whitespace collapsed, trailing ':'/'.' ignored.
bool bareLabel(const std::string& candidate, std::string* label) {
    std::string folded;
    folded.reserve(candidate.size());
    bool pendingSpace = false;
    for (char c : candidate) {
        if (isSpace(c)) {
            pendingSpace = !folded.empty();
            continue;
        }
        if (pendingSpace) {
            folded.push_back(' ');
            pendingSpace = false;
        }
        folded.push_back(lowerAscii(c));
    }
    while (!folded.empty() &&
           (folded.back() == ':' || folded.back() == '.' || folded.back() == ' ')) {
        folded.pop_back();
    }
    if (folded.empty()) {
        return false;
    }

    // Split a trailing number/number word off, then test the name.
    std::size_t split = folded.size();
    while (split > 0 && isAsciiDigit(folded[split - 1])) {
        --split;
    }
    std::string numberSuffix;
    if (split < folded.size()) {
        numberSuffix = folded.substr(split);
        if (split > 0 && folded[split - 1] == ' ') {
            --split;
        }
    } else {
        static const char* const kNumberWords[] = {
            "one", "two", "three", "four", "five", "six",
            "seven", "eight", "nine", "ten",
        };
        for (const char* word : kNumberWords) {
            const std::string suffix = std::string(" ") + word;
            if (folded.size() > suffix.size() &&
                folded.compare(folded.size() - suffix.size(), suffix.size(),
                               suffix) == 0) {
                split = folded.size() - suffix.size();
                numberSuffix = word;
                break;
            }
        }
    }
    const std::string name = folded.substr(0, split);
    if (!isKnownSectionName(name)) {
        return false;
    }
    // The label's displayed spelling is the operator's own text, trimmed
    // and without a trailing colon.
    std::string shown = trim(candidate);
    while (!shown.empty() &&
           (shown.back() == ':' || shown.back() == '.' || shown.back() == ' ')) {
        shown.pop_back();
    }
    *label = shown;
    return true;
}

bool validUtf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (std::size_t j = 1; j <= extra; ++j) {
            const unsigned char next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

std::string normalizeLineEndings(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;  // CRLF is one line break, not two
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

bool hasMetadataMarker(const std::string& line) {
    std::string digits;
    return lineNamesCcliSong(line, &digits) || lineIsCopyright(line) ||
           lineIsLicence(line);
}

// "Words and Music by X", "Written by X", or a comma-separated name list
// ("Example Writer, Second Writer, Third Writer"). Deliberately narrow: a
// prose line with a comma is not an author list.
bool looksLikeAuthorLine(const std::string& line) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || hasMetadataMarker(trimmed)) {
        return false;
    }
    const std::string folded = lowerAscii(trimmed);
    for (const char* prefix : {"words and music by ", "words & music by ",
                               "written by ", "words by ", "music by ",
                               "by "}) {
        if (folded.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    if (trimmed.find(',') == std::string::npos) {
        return false;
    }
    if (trimmed.back() == '.' || trimmed.back() == '!' || trimmed.back() == '?') {
        return false;
    }
    if (trimmed.size() > 300) {
        return false;
    }
    // Every comma-separated part must have at least two words and begin
    // with a letter, which excludes sentences and licence lines.
    std::size_t begin = 0;
    while (begin <= trimmed.size()) {
        const std::size_t comma = trimmed.find(',', begin);
        const std::string part =
            trim(trimmed.substr(begin, comma == std::string::npos
                                           ? std::string::npos
                                           : comma - begin));
        if (part.empty() || part.size() < 2 ||
            !std::isalpha(static_cast<unsigned char>(part.front()))) {
            return false;
        }
        const std::size_t space = part.find(' ');
        if (space == std::string::npos || trim(part.substr(space + 1)).empty()) {
            return false;
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return true;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    return out;
}

}  // namespace

bool lineNamesCcliSong(const std::string& line, std::string* digits) {
    const std::string folded = lowerAscii(line);
    const std::size_t marker = folded.find("ccli song");
    if (marker == std::string::npos) {
        return false;
    }
    // The number is the first run of 3+ digits after the marker (a "CCLI
    // Song #9000001" line). A line naming only "CCLI" with no number is a
    // licence line, not this one.
    std::size_t i = marker + std::string("ccli song").size();
    while (i < line.size() && !isAsciiDigit(line[i])) {
        ++i;
    }
    const std::size_t begin = i;
    while (i < line.size() && isAsciiDigit(line[i])) {
        ++i;
    }
    if (i - begin < 3) {
        return false;
    }
    if (digits != nullptr) {
        *digits = line.substr(begin, i - begin);
    }
    return true;
}

bool lineIsCopyright(const std::string& line) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed.rfind("\xc2\xa9", 0) == 0 ||      // UTF-8 ©
        trimmed.rfind("\xe2\x84\x97", 0) == 0 ||  // ℗
        startsWithInsensitive(trimmed, "copyright") ||
        startsWithInsensitive(trimmed, "(c)") ||
        startsWithInsensitive(trimmed, "(C)")) {
        return true;
    }
    return false;
}

bool lineIsLicence(const std::string& line) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }
    return startsWithInsensitive(trimmed, "for use solely with") ||
           startsWithInsensitive(trimmed, "ccli licence") ||
           startsWithInsensitive(trimmed, "ccli license") ||
           startsWithInsensitive(trimmed, "used by permission") ||
           startsWithInsensitive(trimmed, "all rights reserved") ||
           startsWithInsensitive(trimmed, "reprinted by permission");
}

bool looksLikeSectionLabel(const std::string& line, std::string* label) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed.front() == '[' && trimmed.back() == ']') {
        const std::string inner = trim(trimmed.substr(1, trimmed.size() - 2));
        if (inner.empty()) {
            return false;
        }
        // A bracketed name is a label regardless of the closed set: the
        // operator explicitly marked it as one. Keep their spelling.
        if (label != nullptr) {
            *label = inner;
        }
        return true;
    }
    std::string cleaned;
    if (bareLabel(trimmed, &cleaned)) {
        if (label != nullptr) {
            *label = cleaned;
        }
        return true;
    }
    return false;
}

LyricsDraft parseLyrics(const std::string& plainText) {
    LyricsDraft draft;

    if (plainText.size() > kMaxPastedBytes) {
        draft.error = "the pasted text is larger than " +
                      std::to_string(kMaxPastedBytes / 1024) +
                      " KB; paste only the song's words";
        return draft;
    }
    if (trim(plainText).empty()) {
        draft.error = "the clipboard is empty; copy the song's words first";
        return draft;
    }
    if (!validUtf8(plainText)) {
        draft.error = "the pasted text is not valid UTF-8; re-copy it from "
                      "the source";
        return draft;
    }

    std::vector<std::string> allLines =
        splitLines(normalizeLineEndings(plainText));
    if (allLines.size() > kMaxPastedLines) {
        draft.error = "the pasted text has more than " +
                      std::to_string(kMaxPastedLines) +
                      " lines; paste only the song's words";
        return draft;
    }
    // The input's own line count is bounded; trailing blank lines are not
    // lines of content.
    while (!allLines.empty() && trim(allLines.back()).empty()) {
        allLines.pop_back();
    }
    if (allLines.empty()) {
        draft.error = "the clipboard has no words to import";
        return draft;
    }

    // --- the attribution footer ------------------------------------------
    //
    // Only the final blank-separated block is considered, plus an
    // immediately preceding all-author block when the final one carries a
    // CCLI/© marker. A chorus that merely mentions "©" in a lyric remains
    // words because the block must END the paste and carry a marker in the
    // first place.
    std::size_t metadataStart = allLines.size();
    {
        std::size_t blockEnd = allLines.size();
        std::size_t blockStart = blockEnd;
        while (blockStart > 0 && !trim(allLines[blockStart - 1]).empty()) {
            --blockStart;
        }
        bool marked = false;
        for (std::size_t i = blockStart; i < blockEnd; ++i) {
            if (hasMetadataMarker(allLines[i])) {
                marked = true;
                break;
            }
        }
        if (marked) {
            metadataStart = blockStart;
            // Absorb a directly preceding block of author-looking lines
            // (writers sometimes sit above the CCLI line separated by a
            // blank line). Never absorb a block that holds ordinary words.
            if (blockStart > 0) {
                std::size_t previousEnd = blockStart - 1;
                std::size_t previousStart = previousEnd;
                while (previousStart > 0 && !trim(allLines[previousStart - 1]).empty()) {
                    --previousStart;
                }
                bool allAuthors = previousStart < previousEnd;
                for (std::size_t i = previousStart; i < previousEnd; ++i) {
                    if (!looksLikeAuthorLine(allLines[i])) {
                        allAuthors = false;
                        break;
                    }
                }
                if (allAuthors) {
                    metadataStart = previousStart;
                }
            }
        }
    }

    std::vector<std::string> metadataLines;
    if (metadataStart < allLines.size()) {
        metadataLines.assign(allLines.begin() + static_cast<std::ptrdiff_t>(metadataStart),
                             allLines.end());
        allLines.resize(metadataStart);
        while (!allLines.empty() && trim(allLines.back()).empty()) {
            allLines.pop_back();
        }
    }

    std::vector<std::string> attribution;
    for (const std::string& rawLine : metadataLines) {
        const std::string line = trim(rawLine);
        if (line.empty()) {
            continue;
        }
        std::string digits;
        if (lineNamesCcliSong(line, &digits)) {
            draft.metadata.ccli = digits;
            draft.hasMetadata = true;
            continue;
        }
        if (draft.metadata.authors.empty() && looksLikeAuthorLine(line)) {
            draft.metadata.authors = line;
            draft.hasMetadata = true;
            continue;
        }
        if (lineIsCopyright(line) || lineIsLicence(line) ||
            !attribution.empty()) {
            attribution.push_back(line);
            draft.hasMetadata = true;
            continue;
        }
        // A publisher line directly after the CCLI number ("SongSelect
        // Terms of Use") is attribution too; the marker checks above cover
        // the common spellings, and an unmarked leftover is kept rather
        // than dropped.
        attribution.push_back(line);
        draft.hasMetadata = true;
    }
    draft.metadata.copyright = joinLines(attribution);

    if (allLines.empty()) {
        draft.error = "the pasted text is only attribution; add the song's "
                      "words";
        return draft;
    }

    // --- the title suggestion -------------------------------------------
    //
    // The first non-blank line, provided it is not itself a section label.
    // A lone title before a section is removed below; other leading words
    // remain lyrics, even when they also supply the title suggestion.
    std::size_t firstContent = 0;
    while (firstContent < allLines.size() && trim(allLines[firstContent]).empty()) {
        ++firstContent;
    }
    if (firstContent < allLines.size()) {
        std::string ignoredLabel;
        if (!looksLikeSectionLabel(allLines[firstContent], &ignoredLabel)) {
            draft.title = trim(allLines[firstContent]);
        }
    }

    // A single leading line immediately before the first section (with
    // optional blank lines) is a title, not a sung stanza. A multi-line
    // preamble or unlabelled song retains every word.
    if (!draft.title.empty()) {
        std::size_t next = firstContent + 1;
        while (next < allLines.size() && trim(allLines[next]).empty()) ++next;
        if (next < allLines.size() && looksLikeSectionLabel(allLines[next], nullptr))
            allLines.erase(allLines.begin() + static_cast<std::ptrdiff_t>(firstContent));
    }

    // --- stanzas ---------------------------------------------------------
    std::vector<Stanza> stanzas;
    Stanza current;
    bool currentHasContent = false;
    const auto closeStanza = [&] {
        if (!current.lines.empty() || !trim(current.label).empty()) {
            if (!current.lines.empty()) {
                stanzas.push_back(current);
            }
        }
        current = Stanza();
        currentHasContent = false;
    };

    for (const std::string& rawLine : allLines) {
        const std::string line = trim(rawLine);
        if (line.empty()) {
            closeStanza();
            continue;
        }
        std::string label;
        if (looksLikeSectionLabel(line, &label)) {
            // A label opens a new stanza. A label with no words of its own
            // is replaced by the next label rather than becoming an empty
            // slide.
            closeStanza();
            current.label = label;
            continue;
        }
        current.lines.push_back(trim(rawLine));
        currentHasContent = true;
    }
    closeStanza();
    (void)currentHasContent;

    if (stanzas.empty()) {
        draft.error = "no song words were found after the labels and "
                      "attribution";
        return draft;
    }

    // The catalog's own limits are the final gate: the draft the page
    // offers must be a song the Library can store.
    std::string validation;
    if (!openshow::catalog::validSong(
            draft.title.empty() ? "Song" : draft.title, stanzas,
            draft.metadata, &validation)) {
        draft.error = validation;
        return draft;
    }

    draft.stanzas = std::move(stanzas);
    draft.ok = true;
    return draft;
}

}  // namespace openshow::importer
