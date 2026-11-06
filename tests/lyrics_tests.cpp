/*
 * The clipboard lyrics parser's regressions (task SI.1): line endings,
 * Unicode, labels versus prose, stanza boundaries, the title suggestion,
 * the CCLI/author/copyright footer and the named bounds. Qt-free.
 */

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "openshow/importer/lyrics.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::importer::LyricsDraft;
using openshow::importer::looksLikeSectionLabel;
using openshow::importer::parseLyrics;

std::string joinedLyrics(const LyricsDraft& draft) {
    std::string out;
    for (const auto& stanza : draft.stanzas) {
        for (const std::string& line : stanza.lines) {
            if (!out.empty()) {
                out.push_back('\n');
            }
            out += line;
        }
    }
    return out;
}

bool contains(const std::vector<openshow::catalog::Stanza>& stanzas,
              const std::string& label) {
    for (const auto& stanza : stanzas) {
        if (stanza.label == label) {
            return true;
        }
    }
    return false;
}

const char* kSyntheticSong =
    "Morning Light\n"
    "\n"
    "[Verse 1]\n"
    "Morning light is breaking\n"
    "Every shadow gone\n"
    "\n"
    "[Chorus]\n"
    "Sing it again and again\n"
    "Sing it again and again\n"
    "\n"
    "Bridge\n"
    "We will not turn back\n"
    "\n"
    "[Post-Chorus]\n"
    "Praise the Lord\n"
    "\n"
    "Example Writer, Second Writer\n"
    "CCLI Song #9000001\n"
    "\u00a9 2024 Example Music\n"
    "For use solely with the Example Terms of Use. All rights reserved.\n"
    "CCLI Licence No.12345\n";

void testFullSongAndFooter() {
    const LyricsDraft draft = parseLyrics(kSyntheticSong);
    check(draft.ok, "the synthetic song parses");
    check(draft.error.empty(), "a successful parse carries no error");
    check(draft.title == "Morning Light", "the first line is the title suggestion");
    check(draft.hasMetadata, "the attribution footer is recognised");
    check(draft.metadata.ccli == "9000001", "the CCLI number is extracted");
    check(draft.metadata.authors == "Example Writer, Second Writer",
          "the author line is extracted");
    check(draft.metadata.copyright.find("\xc2\xa9 2024 Example Music") !=
              std::string::npos,
          "the copyright line is kept");
    check(draft.metadata.copyright.find("For use solely") != std::string::npos,
          "the permission line is kept in the attribution block");
    check(draft.metadata.copyright.find("CCLI Licence No.12345") !=
              std::string::npos,
          "the licence line is kept, not dropped");

    check(draft.stanzas.size() == 4, "only verse, chorus, bridge and post-chorus become stanzas");
    check(draft.stanzas[0].label == "Verse 1"
              && joinedLyrics(draft).find("Morning Light") == std::string::npos,
          "the leading title stays metadata and never becomes lyric text");
    check(contains(draft.stanzas, "Verse 1"), "the bracketed verse label is kept");
    check(contains(draft.stanzas, "Chorus"), "the bracketed chorus label is kept");
    check(contains(draft.stanzas, "Bridge"), "the bare bridge label is kept");
    check(contains(draft.stanzas, "Post-Chorus"),
          "the bare post-chorus label is kept");
    check(joinedLyrics(draft).find("Morning light is breaking") !=
              std::string::npos,
          "the words survive");
    check(joinedLyrics(draft).find("9000001") == std::string::npos &&
              joinedLyrics(draft).find("Example Writer") == std::string::npos,
          "the footer never becomes slide words");
}

void testLineEndingsAndUnicode() {
    std::string crlf =
        "[Verse 1]\r\nCaf\u00e9 na\u00efve \u2014 dancing\r\n\u201cQuote\u201d\r\n\r\n"
        "[Chorus]\r\nLa la la\r\n";
    LyricsDraft draft = parseLyrics(crlf);
    check(draft.ok, "CRLF text parses");
    check(draft.stanzas.size() == 2, "CRLF blank lines still split stanzas");
    check(draft.stanzas[0].lines.size() == 2,
          "CRLF did not leave blank lines inside a stanza");
    check(draft.stanzas[0].lines[0] ==
              "Caf\u00e9 na\u00efve \u2014 dancing",
          "Unicode letters, dashes and quotes survive byte for byte");
    check(draft.stanzas[0].lines[1] == "\u201cQuote\u201d",
          "curly quotes survive");

    std::string cr = "Verse\rline one\rline two\r";
    draft = parseLyrics(cr);
    check(draft.ok || draft.error.size() > 0, "a lone-CR paste is handled");
    if (draft.ok) {
        check(draft.stanzas.size() == 1 && draft.stanzas[0].lines.size() == 2,
              "a lone CR is one line break, not part of the words");
    }

    draft = parseLyrics("[Chorus]\n\u00c9l\u00e8ve \u4e16\u754c \xF0\x9F\x99\x8C\n");
    check(draft.ok, "multi-script text parses");
    check(draft.stanzas[0].lines[0] ==
              "\u00c9l\u00e8ve \u4e16\u754c \xF0\x9F\x99\x8C",
          "Latin, CJK and an emoji survive");
}

void testLabelsVersusProse() {
    std::string label;
    check(looksLikeSectionLabel("[Verse 1]", &label) && label == "Verse 1",
          "a bracketed label is recognised and cleaned");
    check(looksLikeSectionLabel("  [Chorus]  ", &label) && label == "Chorus",
          "surrounding whitespace is trimmed");
    check(looksLikeSectionLabel("Verse 2", &label) && label == "Verse 2",
          "a bare numbered label is recognised");
    check(looksLikeSectionLabel("Bridge 1:", &label) && label == "Bridge 1",
          "a trailing colon is not part of the label");
    check(!looksLikeSectionLabel("Chorus of angels", &label),
          "prose containing the word chorus is not a label");
    check(!looksLikeSectionLabel("I love the chorus", &label),
          "a lyric line is not a label");
    check(looksLikeSectionLabel("Verse", &label),
          "a bare 'Verse' is a label");
    check(!looksLikeSectionLabel("Chorusline", &label),
          "a run-on word is not a label");

    const LyricsDraft draft = parseLyrics(
        "Chorus of angels\nSinging in the night\n\nVerse 1\nA line\n");
    check(draft.ok, "the prose song parses");
    check(draft.stanzas.size() == 2, "two stanzas");
    check(draft.stanzas[0].label.empty() &&
              draft.stanzas[0].lines[0] == "Chorus of angels",
          "the prose line stays words, not a label");
    check(draft.stanzas[1].label == "Verse 1",
          "the real label is still recognised");
}

void testStanzaBoundariesAndRepeats() {
    const LyricsDraft draft = parseLyrics(
        "Line one\nLine two\n\n\n\nLine three\n\n[Chorus]\nAgain\nAgain\n");
    check(draft.ok, "the stanza song parses");
    check(draft.stanzas.size() == 3,
          "consecutive blank lines collapse and no empty stanza is made");
    check(draft.stanzas[0].lines.size() == 2 &&
              draft.stanzas[1].lines.size() == 1,
          "authored line grouping is preserved");
    check(draft.stanzas[2].lines ==
              std::vector<std::string>{"Again", "Again"},
          "repeated chorus lines are preserved, not deduplicated");

    // A label with no words never becomes an empty slide.
    const LyricsDraft emptyLabel = parseLyrics("[Chorus]\n\n[Verse]\nwords\n");
    check(emptyLabel.ok, "the label-only stanza parses");
    check(emptyLabel.stanzas.size() == 1 && emptyLabel.stanzas[0].label == "Verse",
          "the wordless Chorus is dropped and the next label survives");
}

void testMissingMetadataAndTitle() {
    const LyricsDraft draft = parseLyrics("[Verse]\nJust words\n");
    check(draft.ok, "a plain paste parses");
    check(draft.title.empty(), "no title line means no suggestion");
    check(!draft.hasMetadata, "no footer means no metadata");
    check(draft.metadata.ccli.empty() && draft.metadata.authors.empty() &&
              draft.metadata.copyright.empty(),
          "nothing is invented");
    check(draft.stanzas.size() == 1 && draft.stanzas[0].label == "Verse",
          "the label is still kept");

    const LyricsDraft titleFirst = parseLyrics("A Title\n[Verse]\nwords\n");
    check(titleFirst.title == "A Title",
          "a title immediately before a label is still suggested");
    check(titleFirst.stanzas.size() == 1 && titleFirst.stanzas[0].lines[0] == "words",
          "a title directly before a section is metadata only");
}

void testTitleSafety() {
    const auto titled = parseLyrics("\n  A title  \r\n\r\nChorus\r\nActual words\r\n");
    check(titled.ok && titled.title == "A title" && titled.stanzas.size() == 1
              && joinedLyrics(titled) == "Actual words", "title removal handles whitespace and CRLF");
    const auto unlabelled = parseLyrics("First lyric\n\nSecond lyric\n");
    check(unlabelled.ok && joinedLyrics(unlabelled) == "First lyric\nSecond lyric",
          "unlabelled lyrics are never mistaken for a removable title");
    const auto preamble = parseLyrics("First lyric\nSecond lyric\n\nVerse 1\nMore words\n");
    check(preamble.ok && preamble.stanzas[0].lines.size() == 2,
          "a multi-line preamble keeps all of its lyric words");
    check(!parseLyrics("Title only\n\nVerse 1\n").ok,
          "a title and empty section cannot create a song with no lyrics");
}

void testAuthorsOnTheirOwnBlock() {
    const LyricsDraft draft = parseLyrics(
        "[Verse]\nwords\n\n"
        "First Writer, Second Writer\n\n"
        "CCLI Song #7654321\n\u00a9 2020 Holder\n");
    check(draft.ok, "the split footer parses");
    check(draft.metadata.authors == "First Writer, Second Writer",
          "an author-only block above the markers is absorbed");
    check(draft.metadata.ccli == "7654321", "the CCLI number is read");
    check(draft.stanzas.size() == 1 &&
              draft.stanzas[0].lines == std::vector<std::string>{"words"},
          "neither footer block became words");
}

void testBoundsAndRefusals() {
    LyricsDraft draft = parseLyrics("");
    check(!draft.ok, "an empty clipboard is refused");
    check(draft.error.find("empty") != std::string::npos,
          "the empty refusal says so");

    draft = parseLyrics("   \n \n\t\n");
    check(!draft.ok, "whitespace-only text is refused");

    std::string huge(openshow::importer::kMaxPastedBytes + 1, 'a');
    draft = parseLyrics(huge);
    check(!draft.ok, "an over-limit paste is refused");
    check(draft.error.find("KB") != std::string::npos,
          "the size refusal names the bound");

    std::string manyLines;
    for (std::size_t i = 0; i < openshow::importer::kMaxPastedLines + 1; ++i) {
        manyLines += "line\n";
    }
    draft = parseLyrics(manyLines);
    check(!draft.ok, "a line-count overflow is refused");
    check(draft.error.find("lines") != std::string::npos,
          "the line refusal names the bound");

    draft = parseLyrics(std::string("bad \xff utf8"));
    check(!draft.ok, "invalid UTF-8 is refused, not silently mangled");

    draft = parseLyrics("CCLI Song #1234567\n\u00a9 2024 Someone\n");
    check(!draft.ok, "attribution with no words is refused");
    check(draft.error.find("only attribution") != std::string::npos,
          "the attribution-only refusal says what is missing");

    // Over the catalog's stanza cap: refused with the catalog's sentence.
    std::string tooMany;
    for (int i = 0; i < 201; ++i) {
        tooMany += "[Verse]\nline\n\n";
    }
    draft = parseLyrics(tooMany);
    check(!draft.ok, "the catalog's stanza cap is enforced by the parser");
    check(draft.error.find("at most") != std::string::npos,
          "the stanza-cap refusal names the cap");
}

void testFixtureFiles() {
    // The stimulus files the operator copies to the clipboard. Keeping them
    // honest here means the fixtures cannot silently rot into an unparsable
    // shape. Paths come from the source-tree define so ctest's working
    // directory does not matter.
    const std::string root = OPENSHOW_SOURCE_DIR;
    const struct {
        const char* file;
        bool metadata;
        std::size_t stanzas;
    } fixtures[] = {
        {"morning-light.txt", true, 6},
        {"grace-abounding.txt", true, 6},
        {"simple-praise.txt", false, 4},
        {"long-verse.txt", true, 4},
    };
    for (const auto& fixture : fixtures) {
        std::ifstream in(root + "/docs/fixtures/songs/" + fixture.file,
                         std::ios::binary);
        check(in.good(), fixture.file);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const LyricsDraft draft = parseLyrics(buffer.str());
        check(draft.ok, fixture.file);
        if (!draft.ok) {
            std::fprintf(stderr, "  %s: %s\n", fixture.file,
                         draft.error.c_str());
            continue;
        }
        check(!draft.title.empty(), "a fixture suggests a title");
        check(draft.hasMetadata == fixture.metadata,
              "a fixture's metadata expectation holds");
        check(draft.stanzas.size() == fixture.stanzas,
              "a fixture's stanza count holds");
        check(joinedLyrics(draft).find("CCLI Song") == std::string::npos,
              "no fixture turns its footer into words");
    }

    // The long verse is the split-across-slides stimulus: its first stanza
    // must hold ten authored lines and no chorus words.
    std::ifstream in(root + "/docs/fixtures/songs/long-verse.txt",
                     std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const LyricsDraft longVerse = parseLyrics(buffer.str());
    check(longVerse.ok, "the long-verse fixture parses");
    if (longVerse.ok) {
        check(longVerse.stanzas[0].label == "Verse 1" &&
                  longVerse.stanzas[0].lines.size() == 10,
              "the long verse keeps its ten authored lines in one stanza");
        check(std::find(longVerse.stanzas[0].lines.begin(),
                        longVerse.stanzas[0].lines.end(),
                        "You are faithful, you are faithful") ==
                  longVerse.stanzas[0].lines.end(),
              "the chorus never joins the verse's stanza");
    }
}

}  // namespace

int main() {
    testFullSongAndFooter();
    testLineEndingsAndUnicode();
    testLabelsVersusProse();
    testStanzaBoundariesAndRepeats();
    testMissingMetadataAndTitle();
    testTitleSafety();
    testAuthorsOnTheirOwnBlock();
    testBoundsAndRefusals();
    testFixtureFiles();
    if (gFailed) {
        std::fprintf(stderr, "lyrics_tests: FAILED\n");
        return 1;
    }
    std::printf("lyrics_tests: all checks passed\n");
    return 0;
}
