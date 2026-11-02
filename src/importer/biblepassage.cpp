// Bible Gateway passage-page extraction (task BI.1).
//
// A deliberately small, structural scan — not a general HTML parser and not
// a regex over the whole document. The importer's rule is that a page which
// stops looking like a passage FAILS rather than having its text guessed
// from: only the passage container is read, only spans the page itself
// marks as verses contribute text, and cross-reference/footnote/numbering
// subtrees are skipped whole. The markup facts this parser stands on were
// recorded in docs/ui/bible-import-plan.md from live passage pages
// (ESV and NIV) and the invalid-reference page.
#include "openshow/importer/biblepassage.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

namespace openshow::importer {
namespace {

// --- entities ------------------------------------------------------------

void appendCodePoint(std::string& out, std::uint32_t cp) {
    if (cp == 0x0) {
        return;  // a NUL byte in UTF-8 text is dropped, never passed on
    }
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool decodeNamedEntity(std::string_view name, std::string& out) {
    // Only what passage text actually carries. An unknown name is left as
    // the literal text it came from (see decodeEntities): silence beats
    // inventing a character the provider did not send.
    struct Entry {
        const char* name;
        const char* utf8;
    };
    static constexpr Entry kEntities[] = {
        {"amp", "&"},        {"lt", "<"},           {"gt", ">"},
        {"quot", "\""},      {"apos", "'"},         {"nbsp", "\xC2\xA0"},
        {"ensp", "\xE2\x80\x82"}, {"emsp", "\xE2\x80\x83"},
        {"thinsp", "\xE2\x80\x89"},
        {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"},
        {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"},
        {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"},
        {"hellip", "\xE2\x80\xA6"}, {"deg", "\xC2\xB0"},
        {"prime", "\xE2\x80\xB2"}, {"Prime", "\xE2\x80\xB3"},
        {"sect", "\xC2\xA7"},      {"pil", "\xC2\xB6"},
        {"middot", "\xC2\xB7"},    {"bull", "\xE2\x80\xA2"},
        {"trade", "\xE2\x84\xA2"}, {"reg", "\xC2\xAE"},
        {"copy", "\xC2\xA9"},      {"times", "\xC3\x97"},
    };
    for (const Entry& e : kEntities) {
        if (name == e.name) {
            out += e.utf8;
            return true;
        }
    }
    return false;
}

std::string decodeEntities(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if (c != '&') {
            out.push_back(c);
            ++i;
            continue;
        }
        const std::size_t semi = in.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 32) {
            out.push_back(c);
            ++i;
            continue;
        }
        const std::string_view body = in.substr(i + 1, semi - i - 1);
        if (!body.empty() && body.front() == '#') {
            const bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
            const std::string digits{hex ? body.substr(2) : body.substr(1)};
            std::uint32_t cp = 0;
            const auto* first = digits.data();
            const auto* last = digits.data() + digits.size();
            if (!digits.empty() && std::from_chars(first, last, cp, hex ? 16 : 10).ptr == last) {
                appendCodePoint(out, cp);
                i = semi + 1;
                continue;
            }
        } else if (decodeNamedEntity(body, out)) {
            i = semi + 1;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// --- text normalization ----------------------------------------------------

void collapseWhitespace(std::string& text) {
    // Single-space words, trimmed ends; the non-breaking space the page
    // hangs after verse numbers is an ordinary space here.
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0xC2 && i + 1 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA0) {
            space = true;
            ++i;
            continue;
        }
        if (std::isspace(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out.push_back(' ');
        }
        space = false;
        out.push_back(static_cast<char>(c));
    }
    text = std::move(out);
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t j = 0;
        while (j < needle.size() &&
               std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
                   std::tolower(static_cast<unsigned char>(needle[j]))) {
            ++j;
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}

std::string upper(std::string_view in) {
    std::string out{in};
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// --- the structural scan -----------------------------------------------------

struct Tag {
    std::string name;  // lower-cased
    bool closing = false;
    bool selfClosing = false;
    std::vector<std::pair<std::string, std::string>> attrs;

    std::string_view attr(std::string_view key) const {
        for (const auto& [name, value] : attrs) {
            if (name == key) {
                return value;
            }
        }
        return {};
    }

    // class attributes are token lists; a token match is exact, because
    // "versenum" must not answer for "crossreference-versenum".
    bool classToken(std::string_view token) const {
        const std::string_view cls = attr("class");
        for (std::size_t start = 0; start <= cls.size();) {
            const std::size_t end = cls.find(' ', start);
            const std::size_t stop = end == std::string_view::npos ? cls.size() : end;
            if (cls.substr(start, stop - start) == token) {
                return true;
            }
            start = stop + 1;
        }
        return false;
    }
};

bool isVoidElement(std::string_view name) {
    return name == "br" || name == "img" || name == "hr" || name == "input" ||
           name == "meta" || name == "link" || name == "source" || name == "wbr";
}

// A per-verse class token looks like "1Cor-1-2": an OSIS-style book
// abbreviation, then chapter, then verse. Returns "chapter:verse".
bool verseToken(std::string_view token, std::string& out) {
    const std::size_t secondDash = token.rfind('-');
    if (secondDash == std::string_view::npos || secondDash + 1 >= token.size()) {
        return false;
    }
    const std::size_t firstDash = token.rfind('-', secondDash - 1);
    if (firstDash == std::string_view::npos || firstDash + 1 >= secondDash) {
        return false;
    }
    const std::string_view head = token.substr(0, firstDash);
    // Book abbreviations are OSIS-style and NUMBERED books are common:
    // "1Cor", "2Tim", "1John". Letters and digits mix at the front, so the
    // only honest rules are "alphanumeric" and "carries a letter".
    if (head.empty()) {
        return false;
    }
    bool hasAlpha = false;
    for (char c : head) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            continue;
        }
        if (!std::isalpha(static_cast<unsigned char>(c))) {
            return false;
        }
        hasAlpha = true;
    }
    if (!hasAlpha) {
        return false;
    }
    const std::string_view chapter = token.substr(firstDash + 1, secondDash - firstDash - 1);
    const std::string_view verse = token.substr(secondDash + 1);
    if (chapter.empty() || verse.empty()) {
        return false;
    }
    for (char c : chapter) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    for (char c : verse) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    out = std::string{chapter} + ":" + std::string{verse};
    return true;
}

struct Frame {
    std::string name;
    enum Role {
        None,
        Passage,       // div.passage-text — the only region verses come from
        Attribution,   // div.publisher-info-bottom — the licence block
        Verse,         // the span the page marks as one verse
        Skip,          // subtree contributes nothing (sup/a/versenum/headings...)
    } role = None;
};

class Scanner {
public:
    Scanner(std::string requestedVersion) : fRequested{upper(std::move(requestedVersion))} {}

    void run(std::string_view html);

    // Page-shape guards found on the raw bytes; they can only ever make
    // finish() stricter.
    void flagBlocked() { fBlocked = true; }
    void flagNoResults() { fNoResults = true; }

    PassageParse finish() {
        if (fBlocked) {
            return {false,
                    "Bible Gateway refused the request (access restricted or automated "
                    "traffic check); nothing was imported",
                    {}};
        }
        if (!fSawPassage) {
            if (fNoResults) {
                return {false, "Bible Gateway found no passage for that reference", {}};
            }
            return {false, "the fetched page carried no passage text; nothing was imported", {}};
        }
        if (fVerses.empty()) {
            return {false,
                    "the passage page carried no verse text — its markup changed; nothing "
                    "was imported",
                    {}};
        }
        if (fAnswered.empty()) {
            return {false,
                    "the passage page did not identify its translation; nothing was "
                    "imported",
                    {}};
        }
        if (fAnswered != fRequested) {
            return {false,
                    "the page answered " + fAnswered + ", not the requested " + fRequested +
                        "; the importer never substitutes a translation",
                    {}};
        }
        Passage passage;
        passage.translation = fAnswered;
        passage.attribution = std::move(fAttribution);
        collapseWhitespace(passage.attribution);
        passage.verses = std::move(fVerses);
        return {true, {}, std::move(passage)};
    }

private:
    void open(const Tag& tag);
    void close(const Frame& frame);
    void text(std::string_view decoded);

    std::string fRequested;
    std::vector<Frame> fStack;
    std::vector<PassageVerse> fVerses;
    std::string fAnswered;
    std::string fAttribution;
    std::string fVerseText;
    std::string fVerseRef;
    bool fSawPassage = false;
    bool fNoResults = false;
    bool fBlocked = false;

    bool insidePassage() const {
        for (const Frame& f : fStack) {
            if (f.role == Frame::Passage) {
                return true;
            }
        }
        return false;
    }
    bool insideVerse() const {
        for (const Frame& f : fStack) {
            if (f.role == Frame::Verse) {
                return true;
            }
        }
        return false;
    }
    bool insideSkip() const {
        for (const Frame& f : fStack) {
            if (f.role == Frame::Skip) {
                return true;
            }
        }
        return false;
    }
    bool insideAttribution() const {
        for (const Frame& f : fStack) {
            if (f.role == Frame::Attribution) {
                return true;
            }
        }
        return false;
    }
};

void Scanner::run(std::string_view html) {
    // A flat scan: text between tags, tags with attributes, and the three
    // page-shape markers that are checked over the raw bytes for the
    // refusal messages (no verse data is ever taken from them).
    std::size_t i = 0;
    while (i < html.size()) {
        const char c = html[i];
        if (c != '<') {
            std::size_t next = html.find('<', i);
            next = next == std::string_view::npos ? html.size() : next;
            text(decodeEntities(html.substr(i, next - i)));
            i = next;
            continue;
        }
        if (html.substr(i, 4) == "<!--") {
            const std::size_t end = html.find("-->", i + 4);
            i = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        if (html.substr(i, 2) == "<!" || html.substr(i, 2) == "<?") {
            const std::size_t end = html.find('>', i + 2);
            i = end == std::string_view::npos ? html.size() : end + 1;
            continue;
        }
        std::size_t j = i + 1;
        Tag tag;
        tag.closing = j < html.size() && html[j] == '/';
        if (tag.closing) {
            ++j;
        }
        const std::size_t nameStart = j;
        while (j < html.size() && !std::isspace(static_cast<unsigned char>(html[j])) &&
               html[j] != '>' && html[j] != '/') {
            ++j;
        }
        tag.name.assign(html.substr(nameStart, j - nameStart));
        for (char& ch : tag.name) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        while (j < html.size() && html[j] != '>') {
            while (j < html.size() && std::isspace(static_cast<unsigned char>(html[j]))) {
                ++j;
            }
            if (j >= html.size() || html[j] == '>') {
                break;
            }
            if (html[j] == '/') {
                tag.selfClosing = true;
                ++j;
                continue;
            }
            const std::size_t keyStart = j;
            while (j < html.size() && html[j] != '=' && html[j] != '>' &&
                   !std::isspace(static_cast<unsigned char>(html[j]))) {
                ++j;
            }
            std::string key{html.substr(keyStart, j - keyStart)};
            for (char& ch : key) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            std::string value;
            if (j < html.size() && html[j] == '=') {
                ++j;
                if (j < html.size() && (html[j] == '"' || html[j] == '\'')) {
                    const char quote = html[j++];
                    const std::size_t valueStart = j;
                    while (j < html.size() && html[j] != quote) {
                        ++j;
                    }
                    value = decodeEntities(html.substr(valueStart, j - valueStart));
                    if (j < html.size()) {
                        ++j;
                    }
                } else {
                    const std::size_t valueStart = j;
                    while (j < html.size() && !std::isspace(static_cast<unsigned char>(html[j])) &&
                           html[j] != '>') {
                        ++j;
                    }
                    value = decodeEntities(html.substr(valueStart, j - valueStart));
                }
            }
            if (!key.empty()) {
                tag.attrs.emplace_back(std::move(key), std::move(value));
            }
        }
        if (j < html.size()) {
            ++j;  // consume the '>'
        }
        if (tag.name == "script" || tag.name == "style") {
            // Neither ever carries passage text; skip to the matching close
            // so their contents cannot reach the verse buffers at all.
            if (!tag.closing) {
                const std::size_t close = html.find("</" + tag.name, j);
                i = close == std::string_view::npos ? html.size() : close;
                continue;
            }
        }
        if (tag.closing) {
            // Pop to the matching open, tolerating an unbalanced page.
            for (std::size_t depth = fStack.size(); depth > 0; --depth) {
                if (fStack[depth - 1].name == tag.name) {
                    while (fStack.size() > depth) {
                        close(fStack.back());
                        fStack.pop_back();
                    }
                    close(fStack.back());
                    fStack.pop_back();
                    break;
                }
            }
        } else {
            // open() classifies the tag AND pushes its frame (a void or
            // self-closed tag is not pushed; a self-closed verse span is
            // finalized on the spot).
            open(tag);
        }
        i = j;
    }
}

void Scanner::open(const Tag& tag) {
    if (!tag.closing) {
        Frame frame{tag.name, Frame::None};
        if (tag.classToken("passage-text")) {
            frame.role = Frame::Passage;
            fSawPassage = true;
        } else if (tag.classToken("publisher-info-bottom")) {
            frame.role = Frame::Attribution;
        } else if (tag.name == "sup" || tag.name == "a" || tag.name == "button" ||
                   tag.name == "h1" || tag.name == "h2" || tag.name == "h3" ||
                   tag.name == "h4" || tag.name == "h5" || tag.name == "h6" ||
                   tag.classToken("versenum") || tag.classToken("chapternum")) {
            // Cross-reference markers, footnotes, section headings and the
            // verse numbers themselves are page furniture, not scripture.
            frame.role = Frame::Skip;
        } else if (insidePassage() && !insideVerse() && tag.name == "span" &&
                   tag.classToken("text")) {
            // The page's own per-verse mark: class "text 1Cor-1-2". A
            // heading's span carries the same token but lives inside an
            // h3, which already skipped its whole subtree.
            for (const auto& [key, value] : tag.attrs) {
                if (key != "class") {
                    continue;
                }
                for (std::size_t start = 0; start <= value.size();) {
                    const std::size_t end = value.find(' ', start);
                    const std::size_t stop = end == std::string_view::npos ? value.size() : end;
                    std::string ref;
                    if (verseToken(value.substr(start, stop - start), ref)) {
                        frame.role = Frame::Verse;
                        fVerseRef = std::move(ref);
                        fVerseText.clear();
                        break;
                    }
                    start = stop + 1;
                }
            }
        } else if (insidePassage() && tag.name == "div") {
            // The container names the answered translation: version-NIV.
            for (const auto& [key, value] : tag.attrs) {
                if (key != "class") {
                    continue;
                }
                for (std::size_t start = 0; start <= value.size();) {
                    const std::size_t end = value.find(' ', start);
                    const std::size_t stop = end == std::string_view::npos ? value.size() : end;
                    const std::string_view token = value.substr(start, stop - start);
                    if (token.substr(0, 8) == "version-" && token.size() > 8 &&
                        fAnswered.empty()) {
                        fAnswered = upper(token.substr(8));
                    }
                    start = stop + 1;
                }
            }
        }
        if (!tag.selfClosing && !isVoidElement(tag.name)) {
            fStack.push_back(std::move(frame));
        } else if (frame.role == Frame::Verse) {
            close(frame);  // a self-closed verse span is still a complete verse
        }
    }
}

void Scanner::close(const Frame& frame) {
    if (frame.role == Frame::Verse) {
        PassageVerse verse;
        verse.reference = fVerseRef;
        verse.text = fVerseText;
        collapseWhitespace(verse.text);
        if (!verse.text.empty()) {
            fVerses.push_back(std::move(verse));
        }
        fVerseRef.clear();
        fVerseText.clear();
    }
}

void Scanner::text(std::string_view decoded) {
    if (insideAttribution()) {
        fAttribution += decoded;
        return;
    }
    if (insideVerse() && !insideSkip()) {
        fVerseText += decoded;
        return;
    }
}

}  // namespace

PassageParse parsePassagePage(std::string_view html, std::string_view requestedVersion) {
    if (html.empty()) {
        return {false, "the lookup returned an empty page; nothing was imported", {}};
    }
    Scanner scanner{std::string{requestedVersion}};
    // The refusal markers are searched on the raw bytes because they are
    // page furniture the scanner never reads; they only ever make the
    // outcome stricter, never import anything.
    if (containsCaseInsensitive(html, "access restricted") ||
        containsCaseInsensitive(html, "unusual traffic")) {
        scanner.flagBlocked();
    }
    if (containsCaseInsensitive(html, "No results found")) {
        scanner.flagNoResults();
    }
    scanner.run(html);
    return scanner.finish();
}

std::vector<Translation> translations() {
    return {
        {"KJV", "King James Version", "public domain — no copyright statement required"},
        {"NKJV", "New King James Version", "carries the publisher's copyright notice"},
        {"NIV", "New International Version", "carries the publisher's copyright notice"},
        {"ESV", "English Standard Version", "carries the publisher's copyright notice"},
        {"NLT", "New Living Translation", "carries the publisher's copyright notice"},
        {"NASB", "New American Standard Bible", "carries the publisher's copyright notice"},
        {"CSB", "Christian Standard Bible", "carries the publisher's copyright notice"},
        {"NRSVUE", "New Revised Standard Version Updated Edition",
         "carries the publisher's copyright notice"},
    };
}

}  // namespace openshow::importer
