// Task BI.4 — the pagination's properties, on a scripted measuring engine.
//
// The engine stand-in is a deliberately crude but MONOTONIC model (rows of
// uniform glyphs: codepoints per line by width, row height by size × line
// spacing). The properties under test are the plan's, and they must hold
// for ANY engine with the same monotonicity — order preserved, no word
// dropped or repeated, no block measured twice, splits only at whitespace,
// the chosen size never altered, an unusable area blocked with an
// explanation. That the real engine is the worker's SkParagraph is pinned
// separately by text_measure_tests against a live worker.
#include "openshow/importer/scripturepagination.h"

#include <cmath>
#include <memory>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace openshow::importer;

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++gFailed;
    }
    return condition;
}

// --- the scripted engine ----------------------------------------------------

size_t codepoints(const std::string& s) {
    size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

std::string stripWhitespace(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

struct MeasureScript {
    // The MeasureFn copies what it wraps, so the recording lives behind a
    // shared pointer: the script object the test keeps reading is the one
    // the port really talks to.
    struct State {
        // Every block the port was asked about, serialized, for the
        // "measured exactly once" claim.
        std::vector<std::string> asked;
        std::vector<double> sizes;  // every fontSize the port was asked with
        int calls = 0;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    int failFromCall = -1;      // refuse every call at or after this index
    bool shuffleKeys = false;   // answer with a mismatched key
    bool dropReply = false;     // answer with one reply short
    std::string failureMessage = "the worker said no";

    bool operator()(const std::vector<MeasureRequest>& requests,
                    std::vector<MeasureReply>* replies, std::string* error) {
        const int index = state->calls++;
        for (const MeasureRequest& request : requests) {
            // CONTENT only — the correlation key is unique by design and
            // would make the "asked once" check vacuous.
            std::string line;
            for (const std::string& text : request.lines) {
                line += text + "\u0001";
            }
            state->asked.push_back(std::move(line));
            state->sizes.push_back(request.fontSize);
        }
        if (failFromCall >= 0 && index >= failFromCall) {
            *error = failureMessage;
            return false;
        }
        replies->clear();
        for (const MeasureRequest& request : requests) {
            // Uniform glyph rows: charWidth = fontSize/2, every line takes
            // at least one row. Monotone in lines and in size.
            double rows = 0.0;
            const double charWidth = request.fontSize / 2.0;
            for (const std::string& text : request.lines) {
                rows += text.empty() ? 1.0 : std::ceil(codepoints(text) * charWidth /
                                                       request.width);
            }
            MeasureReply reply;
            reply.key = request.key;
            reply.height = rows * request.fontSize * request.lineSpacing;
            reply.lineCount = static_cast<int>(rows);
            replies->push_back(reply);
        }
        if (shuffleKeys && !replies->empty()) {
            replies->front().key = "not-the-asked-key";
        }
        if (dropReply && replies->size() > 1) {
            replies->pop_back();
        }
        return true;
    }
};

// The model's ruler: at size 10, spacing 1, a row is 10 px and
// width/5 codepoints fill one row.
PaginationStyle style(double width, double height, double size = 10.0) {
    PaginationStyle s;
    s.boxWidth = width;
    s.boxHeight = height;
    s.fontSize = size;
    s.lineSpacing = 1.0;
    return s;
}

PaginationInput input(std::vector<PaginationVerse> verses, PaginationStyle st,
                      std::string reference = {}, std::string attribution = {}) {
    PaginationInput in;
    in.verses = std::move(verses);
    in.style = st;
    in.reference = std::move(reference);
    in.attribution = std::move(attribution);
    return in;
}

std::string concatenated(const PaginationOutcome& outcome) {
    std::string all;
    for (const GeneratedSlide& slide : outcome.slides) {
        for (const std::string& line : slide.lines) {
            all += line;
            all += "\n";
        }
    }
    return all;
}

std::string passageText(const std::vector<PaginationVerse>& verses) {
    std::string all;
    for (const PaginationVerse& verse : verses) {
        all += verse.text;
        all += " ";
    }
    return all;
}

// The acceptance identity, asserted everywhere it applies: after
// whitespace normalisation the slides' words are the selected passage —
// nothing dropped, nothing added, nothing repeated.
void checkIdentity(const PaginationOutcome& outcome, const std::vector<PaginationVerse>& verses,
                   const char* what) {
    check(stripWhitespace(concatenated(outcome)) == stripWhitespace(passageText(verses)), what);
}

// --- the whole passage fits when the box does -------------------------------

void testFitsOneSlide() {
    const std::vector<PaginationVerse> verses{
        {"1:1", "In the beginning was the Word."},
        {"1:2", "The same was in the beginning."},
    };
    MeasureScript script;
    const PaginationOutcome outcome =
        paginateScripture(input(verses, style(1600, 900), "John 1:1-2"), script);
    check(outcome.ok && outcome.slides.size() == 1, "two short verses fill one slide in a big box");
    check(outcome.ok && outcome.slides[0].lines.size() == 2, "each whole verse keeps its own line");
    check(outcome.ok && outcome.slides[0].label == "1:1-1:2", "the label names the covered range");
    checkIdentity(outcome, verses, "one slide preserves the passage exactly");
    check(outcome.reservedHeight == 10.0, "the reference reserved its measured row");
}

// --- narrow vs. wide, big vs. small: more slides, never smaller words -------

void testBoxAndSizeShape() {
    // 100 three-letter words: predictable rows in the model, and word
    // boundaries available for any split it forces.
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += (i ? " " : "") + std::string("xxx");
    }
    const std::vector<PaginationVerse> verses{{"1:1", text}};

    MeasureScript wideScript;
    const PaginationOutcome wide = paginateScripture(input(verses, style(1600, 100)), wideScript);
    check(wide.ok && wide.slides.size() == 1, "a wide box holds the verse without a split");

    MeasureScript narrowScript;
    const PaginationOutcome narrow = paginateScripture(input(verses, style(100, 100)), narrowScript);
    check(narrow.ok && narrow.slides.size() > wide.slides.size(),
          "the same text in a narrow box becomes more slides, not smaller words");
    checkIdentity(narrow, verses, "the narrow split preserves the passage exactly");

    MeasureScript bigScript;
    const PaginationOutcome big =
        paginateScripture(input(verses, style(1600, 100, /*size=*/80.0)), bigScript);
    check(big.ok && big.slides.size() > wide.slides.size(),
          "a larger chosen size means more slides, never a silent shrink");
    checkIdentity(big, verses, "the larger size still preserves the passage exactly");
    for (const double size : bigScript.state->sizes) {
        if (size != 80.0) {
            check(false, "every measurement asks at the CHOSEN size — nothing shrinks");
            return;
        }
    }
    check(!bigScript.state->sizes.empty(), "measurement really happened at the chosen size");
}

// --- the renderer default (size 0) is forwarded, never invented ----------

void testRendererDefaultSizePassesThrough() {
    // The renderer's own default size lives in the worker alone (it is the
    // answer to fontSize 0). The paginator must forward 0 unchanged — any
    // number it invented here would be a second answerer, and the page
    // would be packing against one engine while the audience saw another.
    // std::function copies the callable, so the sizes land in a shared
    // vector the recorder and this function both see.
    auto sizes = std::make_shared<std::vector<double>>();
    struct Recorder {
        std::shared_ptr<std::vector<double>> sizes;
        bool operator()(const std::vector<MeasureRequest>& requests,
                        std::vector<MeasureReply>* replies, std::string*) {
            for (const MeasureRequest& request : requests) {
                sizes->push_back(request.fontSize);
                MeasureReply reply;
                reply.key = request.key;
                reply.height = 24.0;  // whatever the engine says the default is
                reply.lineCount = 1;
                replies->push_back(reply);
            }
            return true;
        }
    } recorder{sizes};
    const std::vector<PaginationVerse> verses{{"1:1", "First words."}, {"1:2", "Second words."}};
    const PaginationOutcome outcome =
        paginateScripture(input(verses, style(800, 90, /*size=*/0.0), "Mark 1:1-2"), recorder);
    check(outcome.ok && outcome.slides.size() == 1,
          "a theme with no size of its own still paginates (the renderer's default measures)");
    check(!sizes->empty(), "measurement happened at the default too");
    for (const double size : *sizes) {
        if (size != 0.0) {
            check(false, "size 0 reaches the engine unchanged — the default is the worker's, not ours");
            return;
        }
    }
    check(true, "size 0 reaches the engine unchanged — the default is the worker's, not ours");
}

// --- splits: sentences first, then words, identity preserved ----------------

void testSentenceSplit() {
    const std::vector<PaginationVerse> verses{{
        "1:1",
        "Alpha one. Beta two. Gamma three. Delta four.",
    }};
    // Model: row 10 px, 20 codepoints per row at width 100. Available
    // 20 px = 2 rows: the fragment "Alpha one. Beta two. Gamma three."
    // (33 cp -> 2 rows) fits; adding "Delta four." (45 cp -> 3 rows) does
    // not.
    MeasureScript script;
    const PaginationOutcome outcome = paginateScripture(input(verses, style(100, 20)), script);
    check(outcome.ok && outcome.slides.size() == 2,
          "the long verse splits across a continuation slide at a sentence boundary");
    if (outcome.slides.size() == 2) {
        check(outcome.slides[0].lines.at(0) == "Alpha one. Beta two. Gamma three.",
              "the first slide keeps the largest sentence prefix that fits");
        check(outcome.slides[1].lines.at(0) == "Delta four.", "the remainder stands alone after");
        check(outcome.slides[0].label == "1:1 (continues)",
              "the split slide says the verse continues");
        check(outcome.slides[1].label == "(cont.) 1:1",
              "the remainder slide says the verse continued here");
    }
    checkIdentity(outcome, verses, "the sentence split preserves the passage exactly");
}

void testWordSplit() {
    // One long run with no terminator at all: only word boundaries.
    const std::vector<PaginationVerse> verses{{"1:1", "aaaa bbbb cccc dddd eeee ffff gggg hhhh"}};
    // Available 10 px = 1 row = 20 codepoints: "aaaa bbbb cccc dddd" is 19
    // and fits; adding "eeee" (24) does not.
    MeasureScript script;
    const PaginationOutcome outcome = paginateScripture(input(verses, style(100, 10)), script);
    check(outcome.ok && outcome.slides.size() >= 2,
          "a terminator-free verse splits at word boundaries");
    if (!outcome.slides.empty()) {
        check(outcome.slides[0].lines.at(0) == "aaaa bbbb cccc dddd",
              "the word prefix is the longest that fits the row");
    }
    checkIdentity(outcome, verses, "the word split preserves the passage exactly");
    for (const GeneratedSlide& slide : outcome.slides) {
        check(slide.label.find("1:1") != std::string::npos,
              "every continuation slide still names its verse");
    }
}

// --- Unicode: never split inside a character --------------------------------

void testUnicode() {
    // Greek, an em dash, combining accents, Devanagari and CJK — split at
    // spaces, measured by codepoints.
    const std::vector<PaginationVerse> verses{
        {"1:1", "Ωμέγα — words with combining áccents and CJK 中文 text keep ᬵtheir shape"},
    };
    MeasureScript script;
    const PaginationOutcome outcome = paginateScripture(input(verses, style(60, 20)), script);
    check(outcome.ok, "the Unicode passage paginates");
    checkIdentity(outcome, verses, "the Unicode passage survives the split exactly");
    auto validUtf8 = [](const std::string& s) {
        std::size_t i = 0;
        while (i < s.size()) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const std::size_t len = c < 0x80 ? 1
                                      : (c >= 0xC2 && c <= 0xDF) ? 2
                                      : (c >= 0xE0 && c <= 0xEF) ? 3
                                      : (c >= 0xF0 && c <= 0xF4) ? 4
                                                                 : 0;
            if (len == 0 || i + len > s.size()) {
                return false;
            }
            for (std::size_t k = 1; k < len; ++k) {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                    return false;
                }
            }
            i += len;
        }
        return true;
    };
    bool allValid = true;
    bool splitHappened = false;
    for (const GeneratedSlide& slide : outcome.slides) {
        splitHappened = splitHappened || outcome.slides.size() > 1;
        for (const std::string& line : slide.lines) {
            allValid = allValid && validUtf8(line);
        }
    }
    check(splitHappened, "the narrow box really split the Unicode passage");
    check(allValid, "no split ever lands inside a UTF-8 character");
}

void testSplitHelpers() {
    const std::vector<std::string> english = splitSentences("Hello world. Next one! Why? End");
    check(english.size() == 4 && english[0] == "Hello world." && english[1] == "Next one!" &&
              english[2] == "Why?" && english[3] == "End",
          "sentences split at . ! ? followed by space, punctuation riding along");
    const std::vector<std::string> danda = splitSentences("प्रथम। द्वितीय");
    check(danda.size() == 2 && danda[1] == "द्वितीय", "a Devanagari danda ends a sentence too");
    const std::vector<std::string> dots = splitSentences("a...b continues");
    check(dots.size() == 1, "an ellipsis mid-clause does not split");
    const std::vector<std::string> words = splitWords("  a  b\tc\n ");
    check(words.size() == 3 && words[2] == "c", "words split on whitespace runs");
    const std::string uni = "\xC3\xA9\xC3\xA0 word \xE4\xB8\xAD";
    const std::vector<std::string> uniWords = splitWords(uni);
    check(uniWords.size() == 3 && uniWords[0] == std::string("\xC3\xA9\xC3\xA0"),
          "a Unicode word stays whole across the word splitter");
}

// --- the reservation: reference and attribution get measured room -----------

void testReservation() {
    std::vector<PaginationVerse> verses;
    for (int i = 1; i <= 8; ++i) {
        verses.push_back({"1:" + std::to_string(i), std::string(15, 'w')});
    }
    const PaginationStyle box = style(100, 60);  // 6 rows of 10 px
    MeasureScript bare;
    const PaginationOutcome without = paginateScripture(input(verses, box), bare);
    MeasureScript titled;
    const PaginationOutcome withReference =
        paginateScripture(input(verses, box, "Genesis chapter one"), titled);
    check(without.ok && withReference.ok, "both reservations run");
    check(withReference.reservedHeight == 10.0,
          "the reference reserved exactly its measured row at the chosen size");
    check(withReference.availableHeight == 50.0, "the words area shrank by the reservation");
    check(without.ok && withReference.ok && withReference.slides.size() >= without.slides.size(),
          "reserved room leaves the words no more space than they had");
    checkIdentity(withReference, verses, "the reference is not slide text — identity holds");
}

void testReservationBlocks() {
    const std::vector<PaginationVerse> verses{{"1:1", "Short words."}};
    // The attribution alone: 100 cp at width 100 = 5 rows = 50 px in a
    // 50 px box — nothing is left for the words, so Add must block with
    // its own sentence, not clip.
    MeasureScript script;
    const PaginationOutcome blocked =
        paginateScripture(input(verses, style(100, 50), "Ref", std::string(100, 'c')), script);
    check(!blocked.ok && blocked.slides.empty() &&
              blocked.error.find("leave no room") != std::string::npos,
          "an attribution that eats the box blocks Add with its own sentence");
}

// --- honest refusals ---------------------------------------------------------

void testRefusals() {
    const std::vector<PaginationVerse> verses{{"1:1", "word"}};

    {  // Nothing selected.
        MeasureScript script;
        const PaginationOutcome outcome = paginateScripture(input({}, style(100, 50)), script);
        check(!outcome.ok && outcome.slides.empty() &&
                  outcome.error.find("no verses") != std::string::npos,
              "an empty selection refuses with its own sentence");
    }
    {  // An unusable theme area is refused, not guessed around.
        MeasureScript script;
        const PaginationOutcome outcome = paginateScripture(input(verses, style(0, 50)), script);
        check(!outcome.ok && outcome.error.find("measured") != std::string::npos,
              "a zero-width box is refused as unmeasurable");
        MeasureScript nanScript;
        const PaginationOutcome nan =
            paginateScripture(input(verses, style(100, std::nan(""), 10)), nanScript);
        check(!nan.ok && nan.slides.empty(), "a NaN box height is refused too");
    }
    {  // A single word that will not fit at the chosen size blocks Add.
        // 20 cp at width 30 = 4 rows = 40 px; the box has 10.
        MeasureScript script;
        const PaginationOutcome outcome = paginateScripture(
            input({{"1:1", "supercalifragilistic"}}, style(30, 10)), script);
        check(!outcome.ok && outcome.slides.empty() &&
                  outcome.error.find("single word") != std::string::npos &&
                  outcome.error.find("1:1") != std::string::npos,
              "a word wider than the box at the chosen size blocks Add, naming its verse");
    }
    {  // A dead engine fails the run — and is not re-asked per verse.
        MeasureScript script;
        script.failFromCall = 0;
        const PaginationOutcome outcome =
            paginateScripture(input(verses, style(100, 50), "Ref"), script);
        check(!outcome.ok && outcome.slides.empty() &&
                  outcome.error.find("the worker said no") != std::string::npos,
              "a failed measurement refuses with the engine's own sentence");
        check(script.state->calls <= 1, "a dead engine is latched, not re-asked per verse");
    }
    {  // A shuffled answer is a failure, not a height.
        MeasureScript script;
        script.shuffleKeys = true;
        const PaginationOutcome outcome =
            paginateScripture(input(verses, style(100, 50), "Ref"), script);
        check(!outcome.ok && outcome.slides.empty(),
              "an answer whose keys do not match the ask fails the run");
    }
    {  // A short answer is a failure too.
        MeasureScript script;
        script.dropReply = true;
        const PaginationOutcome outcome =
            paginateScripture(input(verses, style(100, 50), "A longish ref", "a second reserve"),
                              script);
        check(!outcome.ok && outcome.slides.empty(),
              "an answer with a reply missing fails the run");
    }
}

// --- the port's discipline: every block once --------------------------------

void testMeasureDiscipline() {
    std::vector<PaginationVerse> verses;
    for (int i = 1; i <= 12; ++i) {
        verses.push_back({"1:" + std::to_string(i),
                          "Verse number " + std::to_string(i) +
                              " carries enough words to need several slides in a narrow box"});
    }
    MeasureScript script;
    const PaginationOutcome outcome =
        paginateScripture(input(verses, style(120, 40), "Zechariah 1"), script);
    check(outcome.ok && outcome.slides.size() >= 3, "the twelve verses needed continuation");
    // No block content is ever asked of the engine twice in one run — the
    // second sight of identical words (a tail that matches another verse's
    // tail) is a cache hit, not a round trip.
    const std::set<std::string> unique{script.state->asked.begin(), script.state->asked.end()};
    check(unique.size() == script.state->asked.size(),
          "no block is asked of the measuring engine twice in one run");
}

}  // namespace

int main() {
    testFitsOneSlide();
    testBoxAndSizeShape();
    testRendererDefaultSizePassesThrough();
    testSentenceSplit();
    testWordSplit();
    testUnicode();
    testSplitHelpers();
    testReservation();
    testReservationBlocks();
    testRefusals();
    testMeasureDiscipline();
    if (gFailed == 0) {
        std::printf("scripture_pagination_tests: all fixtures passed\n");
        return 0;
    }
    std::printf("scripture_pagination_tests: %d FAILURES\n", gFailed);
    return 1;
}
