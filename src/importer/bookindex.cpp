// The built-in canon table and the suggester (BIF.3). Everything here is
// pure string work: the 66 books in order, each with its common
// abbreviations, and a matcher that ignores case, whitespace and how the
// ordinal on a numbered book is spelled. See the header for the rules.
#include "openshow/importer/bookindex.h"

#include <cctype>

namespace openshow::importer {

namespace {

struct BookRow {
    std::string_view name;
    // Space-separated lowercase abbreviations; the full name matches
    // without appearing here.
    std::string_view aliases;
};

// The canon, in book order — so numbered books suggest 1 before 2 before 3
// for free, and every suggestion list reads as the shelf does.
constexpr BookRow kBooks[] = {
    {"Genesis", "gen gn"},
    {"Exodus", "exo ex"},
    {"Leviticus", "lev"},
    {"Numbers", "num"},
    {"Deuteronomy", "deut dt"},
    {"Joshua", "josh jos"},
    {"Judges", "judg jdgs"},
    {"Ruth", "rut"},
    {"1 Samuel", "1sam 1sm"},
    {"2 Samuel", "2sam 2sm"},
    {"1 Kings", "1ki 1kgs"},
    {"2 Kings", "2ki 2kgs"},
    {"1 Chronicles", "1chr 1chron"},
    {"2 Chronicles", "2chr 2chron"},
    {"Ezra", "ezr"},
    {"Nehemiah", "neh"},
    {"Esther", "esth est"},
    {"Job", "job"},
    {"Psalms", "ps psalm"},
    {"Proverbs", "prov pr"},
    {"Ecclesiastes", "ecc eccl"},
    {"Song of Solomon", "sos ss song"},
    {"Isaiah", "isa"},
    {"Jeremiah", "jer"},
    {"Lamentations", "lam"},
    {"Ezekiel", "eze ezek"},
    {"Daniel", "dan"},
    {"Hosea", "hos"},
    {"Joel", "joe"},
    {"Amos", "amos"},
    {"Obadiah", "obad ob"},
    {"Jonah", "jon"},
    {"Micah", "mic"},
    {"Nahum", "nah"},
    {"Habakkuk", "hab"},
    {"Zephaniah", "zeph"},
    {"Haggai", "hag"},
    {"Zechariah", "zech zec"},
    {"Malachi", "mal"},
    {"Matthew", "matt mat"},
    {"Mark", "mr"},
    {"Luke", "luk"},
    {"John", "jn joh"},
    {"Acts", "act ac"},
    {"Romans", "rom ro"},
    {"1 Corinthians", "1cor 1co"},
    {"2 Corinthians", "2cor 2co"},
    {"Galatians", "gal"},
    {"Ephesians", "eph"},
    {"Philippians", "phil"},
    {"Colossians", "col"},
    {"1 Thessalonians", "1thess 1th"},
    {"2 Thessalonians", "2thess 2th"},
    {"1 Timothy", "1tim 1ti"},
    {"2 Timothy", "2tim 2ti"},
    {"Titus", "tit"},
    {"Philemon", "phm philem"},
    {"Hebrews", "heb"},
    {"James", "jas jm"},
    {"1 Peter", "1pet 1pe"},
    {"2 Peter", "2pet 2pe"},
    {"1 John", "1jn 1jo"},
    {"2 John", "2jn 2jo"},
    {"3 John", "3jn 3jo"},
    {"Jude", "jud"},
    {"Revelation", "rev re"},
};

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Lowercase with every whitespace run removed: "1  Cor  " -> "1cor".
std::string squash(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (!isSpace(c)) {
            out.push_back(lower(c));
        }
    }
    return out;
}

// The book part of what was typed, and where the rest begins. The book
// part runs to the first ':' minus one trailing chapter token: the book
// itself may open with a digit ("1 cor"), but a digit-only word after the
// first is the chapter, not the book. "1 cor 1:1-6" -> book "1 cor",
// consumed up to "cor"; "1 cor" -> book "1 cor", all consumed.
struct BookPart {
    std::string prefix;   // raw book part, for normalization
    std::size_t consumed = 0;  // characters of `typed` the book part covers
};

BookPart extractBookPart(std::string_view typed) {
    std::size_t end = typed.find(':');
    if (end == std::string_view::npos) {
        end = typed.size();
    }
    const std::string_view head = typed.substr(0, end);
    // Tokenize on whitespace, remembering each token's extent.
    struct Token {
        std::size_t start;
        std::size_t end;
    };
    std::vector<Token> tokens;
    for (std::size_t i = 0; i < head.size();) {
        while (i < head.size() && isSpace(head[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < head.size() && !isSpace(head[i])) {
            ++i;
        }
        if (i > start) {
            tokens.push_back(Token{start, i});
        }
    }
    if (tokens.empty()) {
        return {std::string(head), head.size()};
    }
    // A trailing digit-led token (after at least one other word) is the
    // chapter: "1 cor 1", "genesis 22", "1jn 3".
    if (tokens.size() >= 2) {
        const Token& last = tokens.back();
        const char first = head[last.start];
        if (first >= '0' && first <= '9') {
            return {std::string(head.substr(0, last.start)), last.start};
        }
    }
    return {std::string(head), head.size()};
}

// Normalize a book part for comparison: a leading ordinal — arabic ("1"),
// english ordinal ("1st", "2nd", "3rd") or roman ("i", "ii", "iii") —
// becomes its digit; the rest is squashed lowercase. "1st cor", "i  cor",
// "1cor" all answer "1cor".
std::string normalizeBookPart(std::string_view part) {
    std::size_t i = 0;
    while (i < part.size() && isSpace(part[i])) {
        ++i;
    }
    std::string digits;
    if (i < part.size() && part[i] >= '0' && part[i] <= '9') {
        while (i < part.size() && part[i] >= '0' && part[i] <= '9') {
            digits.push_back(part[i]);
            ++i;
        }
        // A spelled ordinal suffix rides off the digit: "1st" -> "1".
        if (part.size() - i >= 2) {
            const char a = lower(part[i]);
            const char b = lower(part[i + 1]);
            if ((a == 's' && b == 't') || (a == 'n' && b == 'd') || (a == 'r' && b == 'd')) {
                i += 2;
            }
        }
    } else {
        // Roman numerals, longest first, only when a word boundary (or the
        // end) follows — "isaiah" must not become "1saiah".
        static const struct {
            std::string_view roman;
            char digit;
        } kRomans[] = {{"iii", '3'}, {"ii", '2'}, {"i", '1'}};
        for (const auto& entry : kRomans) {
            if (part.size() >= i + entry.roman.size() &&
                part.compare(i, entry.roman.size(), entry.roman) == 0) {
                const std::size_t after = i + entry.roman.size();
                if (after == part.size() || isSpace(part[after])) {
                    digits.push_back(entry.digit);
                    i = after;
                    break;
                }
            }
        }
    }
    std::string rest;
    for (; i < part.size(); ++i) {
        if (!isSpace(part[i])) {
            rest.push_back(lower(part[i]));
        }
    }
    return digits + rest;
}

// How strongly one row answers the typed book part. Exact beats prefix
// (so "1 co" LOOKS UP as 1 Corinthians even though "1corinthians" also
// merely starts with it), and within a tier the canon order decides.
enum class Match { None, Prefix, Exact };

Match nameOrAliasMatches(const BookRow& book, const std::string& normalizedPrefix) {
    const std::string name = squash(book.name);
    if (name == normalizedPrefix) {
        return Match::Exact;
    }
    Match result = Match::None;
    if (name.rfind(normalizedPrefix, 0) == 0) {
        result = Match::Prefix;  // keep scanning: an alias may match exactly
    }
    // Scan the space-separated alias list.
    std::size_t i = 0;
    while (i < book.aliases.size()) {
        while (i < book.aliases.size() && book.aliases[i] == ' ') {
            ++i;
        }
        std::size_t j = i;
        while (j < book.aliases.size() && book.aliases[j] != ' ') {
            ++j;
        }
        const std::string alias(book.aliases.substr(i, j - i));
        if (alias == normalizedPrefix) {
            return Match::Exact;
        }
        if (alias.rfind(normalizedPrefix, 0) == 0) {
            result = Match::Prefix;
        }
        i = j;
    }
    return result;
}

}  // namespace

std::vector<BookSuggestion> suggestBooks(std::string_view typed, std::size_t limit) {
    std::vector<BookSuggestion> suggestions;
    const BookPart part = extractBookPart(typed);
    const std::string normalized = normalizeBookPart(part.prefix);
    if (normalized.empty()) {
        return suggestions;  // nothing typed is nothing suggested
    }
    const std::string_view remainder = typed.substr(part.consumed);

    // Exact matches first, prefixes second; canon order within each tier.
    for (int pass = 0; pass < 2; ++pass) {
        for (const BookRow& book : kBooks) {
            const Match match = nameOrAliasMatches(book, normalized);
            if (match == Match::None || (pass == 0) != (match == Match::Exact)) {
                continue;
            }
            BookSuggestion suggestion;
            suggestion.name = std::string(book.name);
            suggestion.completion = std::string(book.name);
            if (remainder.empty()) {
                suggestion.completion += ' ';  // room for what comes next
            } else {
                // The separator between book and chapter was consumed with
                // the book part; the completion restores it so the tail
                // never glues onto the name.
                if (!isSpace(remainder.front())) {
                    suggestion.completion += ' ';
                }
                suggestion.completion += remainder;
            }
            suggestions.push_back(std::move(suggestion));
            if (suggestions.size() >= limit) {
                return suggestions;
            }
        }
    }
    return suggestions;
}

std::optional<std::string> canonicalBook(std::string_view typed) {
    const BookPart part = extractBookPart(typed);
    const std::string normalized = normalizeBookPart(part.prefix);
    if (normalized.empty()) {
        return std::nullopt;
    }
    for (const BookRow& book : kBooks) {
        if (nameOrAliasMatches(book, normalized) == Match::Exact) {
            return std::string(book.name);
        }
    }
    return std::nullopt;
}

}  // namespace openshow::importer
