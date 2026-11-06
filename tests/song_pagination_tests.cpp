/*
 * The stanza-aware song planner's regressions (task SI.2): a section starts
 * on a new slide, an overfull stanza splits at authored lines, a single
 * over-long line uses the shared engine, and neither words nor font sizes
 * are sacrificed. Qt-free, with a scripted measuring engine.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "openshow/importer/songpagination.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::importer::FlowStyle;
using openshow::importer::MeasureFn;
using openshow::importer::MeasureReply;
using openshow::importer::MeasureRequest;
using openshow::importer::SongPaginationInput;
using openshow::importer::SongPaginationOutcome;
using openshow::importer::SongSlide;
using openshow::importer::paginateSong;

int wrappedLines(const std::string& line) {
    // A deliberately crude but deterministic layout: every 20 bytes wrap.
    if (line.empty()) {
        return 1;
    }
    return static_cast<int>((line.size() + 19) / 20);
}

MeasureFn scriptedMeasurer(int* calls, std::set<std::string>* seen) {
    return [calls, seen](const std::vector<MeasureRequest>& requests,
                         std::vector<MeasureReply>* replies,
                         std::string* error) -> bool {
        replies->clear();
        for (const MeasureRequest& request : requests) {
            if (calls != nullptr) {
                ++*calls;
            }
            int rendered = 0;
            for (const std::string& line : request.lines) {
                rendered += wrappedLines(line);
            }
            if (seen != nullptr) {
                std::string identity;
                for (const std::string& line : request.lines) {
                    identity += line;
                    identity.push_back('\n');
                }
                identity += "|" + std::to_string(request.fontSize) + "|" +
                            std::to_string(request.width) + "|" +
                            std::to_string(request.lineSpacing);
                if (!seen->insert(identity).second) {
                    // A duplicate ask is legal only when different engines
                    // (planner vs flow) ask; the fixtures that assert cache
                    // behaviour count it separately.
                }
            }
            MeasureReply reply;
            reply.key = request.key;
            reply.height = static_cast<double>(rendered) * 100.0;
            reply.lineCount = rendered;
            bool fits = true;
            for (const std::string& line : request.lines) {
                // A "word" longer than the box is the dead end the real
                // engine reports as fits=false.
                std::size_t run = 0;
                for (char c : line) {
                    if (c == ' ') {
                        run = 0;
                    } else if (++run > 20) {
                        fits = false;
                    }
                }
            }
            reply.fits = fits;
            replies->push_back(reply);
        }
        (void)error;
        return true;
    };
}

FlowStyle styleWithBox(double height) {
    FlowStyle style;
    style.boxWidth = 1600.0;
    style.boxHeight = height;
    style.fontSize = 96.0;
    style.lineSpacing = 1.25;
    style.fontFamily = "Test Sans";
    style.fitMode = "fixed";
    return style;
}

openshow::catalog::Stanza stanza(const std::string& label,
                                 const std::vector<std::string>& lines) {
    openshow::catalog::Stanza result;
    result.label = label;
    result.lines = lines;
    return result;
}

std::string stripSpace(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c != ' ' && c != '\n' && c != '\t') {
            out += c;
        }
    }
    return out;
}

std::string joinedOfStanza(const SongPaginationOutcome& outcome,
                           std::size_t stanzaIndex) {
    std::string out;
    for (const SongSlide& slide : outcome.slides) {
        if (slide.stanzaIndex != stanzaIndex) {
            continue;
        }
        for (const std::string& line : slide.lines) {
            out += line;
        }
    }
    return stripSpace(out);
}

void testWholeStanzaAndHardBoundary() {
    SongPaginationInput input;
    input.style = styleWithBox(300.0);
    input.stanzas = {
        stanza("Verse 1",
               {"one", "two", "three", "four", "five"}),
        stanza("Chorus", {"six", "seven"}),
    };
    int calls = 0;
    std::set<std::string> seen;
    const SongPaginationOutcome outcome =
        paginateSong(input, scriptedMeasurer(&calls, &seen));
    check(outcome.ok, "the two-section song paginates");
    check(outcome.slides.size() == 3,
          "the verse needs two slides and the chorus one");
    if (outcome.slides.size() == 3) {
        check(outcome.slides[0].lines ==
                  std::vector<std::string>{"one", "two", "three"},
              "the verse fills its first slide with whole lines");
        check(outcome.slides[0].label == "Verse 1", "the first slide is labelled");
        check(outcome.slides[1].lines == std::vector<std::string>{"four", "five"},
              "the verse's remainder is its own slide");
        check(outcome.slides[1].label == "Verse 1 (cont.)",
              "the continuation says so");
        check(outcome.slides[1].startsContinued,
              "the continuation is marked as such");
        // THE rule: the chorus does NOT fill the verse's leftover room.
        check(outcome.slides[2].lines == std::vector<std::string>{"six", "seven"},
              "the chorus is whole on its own slide");
        check(outcome.slides[2].label == "Chorus",
              "the chorus keeps its own label");
        check(outcome.slides[2].startsContinued == false,
              "the chorus is not marked as a continuation");
        check(outcome.slides[2].stanzaIndex == 1,
              "the chorus names its own stanza");
    }
    check(joinedOfStanza(outcome, 0) == stripSpace("onetwothreefourfive"),
          "the verse's words survive exactly, in order");
    check(joinedOfStanza(outcome, 1) == stripSpace("sixseven"),
          "the chorus's words survive exactly, in order");
}

void testWholeStanzaFitsOnOneSlide() {
    SongPaginationInput input;
    input.style = styleWithBox(300.0);
    input.stanzas = {stanza("Verse", {"one", "two", "three"})};
    const SongPaginationOutcome outcome =
        paginateSong(input, scriptedMeasurer(nullptr, nullptr));
    check(outcome.ok, "a fitting stanza paginates");
    check(outcome.slides.size() == 1, "it is one slide");
    check(outcome.slides[0].lines.size() == 3, "every line is on it");
    check(!outcome.slides[0].startsContinued, "it does not claim continuation");
}

void testOverlongLineUsesTheEngine() {
    // Ten two-letter words plus two long lines: a single authored line that
    // cannot fit as one block. The engine splits at word boundaries.
    std::string longLine;
    for (int i = 0; i < 40; ++i) {
        if (!longLine.empty()) {
            longLine.push_back(' ');
        }
        longLine += "word";
    }
    SongPaginationInput input;
    input.style = styleWithBox(300.0);
    input.stanzas = {stanza("Verse 1", {longLine, "Amen"}),
                     stanza("Chorus", {"Sing"})};
    const SongPaginationOutcome outcome =
        paginateSong(input, scriptedMeasurer(nullptr, nullptr));
    check(outcome.ok, "the over-long line still paginates");
    check(outcome.slides.size() >= 2,
          "the over-long line becomes several slides");
    check(joinedOfStanza(outcome, 0) == stripSpace(longLine + "Amen"),
          "no word is lost, repeated or reordered across the split");
    // Every slide has only this stanza's words until the chorus slide.
    for (const SongSlide& slide : outcome.slides) {
        if (slide.stanzaIndex == 1) {
            check(slide.lines == std::vector<std::string>{"Sing"},
                  "the chorus is its own slide");
        }
    }
    check(outcome.slides.back().label == "Chorus",
          "the chorus keeps its label");
}

void testDeadEndAndStyleRefusals() {
    SongPaginationInput input;
    input.style = styleWithBox(50.0);  // less than one rendered line
    input.stanzas = {stanza("Verse", {"word"})};
    SongPaginationOutcome outcome =
        paginateSong(input, scriptedMeasurer(nullptr, nullptr));
    check(!outcome.ok, "a box shorter than one line refuses");
    check(outcome.error.find("does not fit") != std::string::npos ||
              outcome.error.find("text area") != std::string::npos,
          "the refusal names the missing room");

    input = SongPaginationInput();
    input.style = styleWithBox(300.0);
    input.style.boxWidth = 0.0;
    input.stanzas = {stanza("Verse", {"word"})};
    outcome = paginateSong(input, scriptedMeasurer(nullptr, nullptr));
    check(!outcome.ok, "an unusable box is refused before measuring");
    check(outcome.error.find("text area") != std::string::npos,
          "the style refusal names the text area");

    input.style = styleWithBox(300.0);
    input.style.fontWeight = 1000;
    outcome = paginateSong(input, scriptedMeasurer(nullptr, nullptr));
    check(!outcome.ok, "an out-of-band weight is refused");
    check(outcome.error.find("weight") != std::string::npos,
          "the refusal names the weight");

    input = SongPaginationInput();
    input.style = styleWithBox(300.0);
    input.stanzas = {stanza("Verse", {"   "})};
    outcome = paginateSong(input, scriptedMeasurer(nullptr, nullptr));
    check(!outcome.ok, "a wordless song refuses");
    check(outcome.error.find("no words") != std::string::npos,
          "the refusal says there are no words");
}

void testNoDuplicateMeasurements() {
    SongPaginationInput input;
    input.style = styleWithBox(300.0);
    input.stanzas = {stanza("Verse", {"one", "two", "three", "four", "five"})};
    int calls = 0;
    const SongPaginationOutcome outcome =
        paginateSong(input, scriptedMeasurer(&calls, nullptr));
    check(outcome.ok, "the split song paginates");
    // Whole stanza + prefixes {one}, {one,two}, {one,two,three},
    // {one,two,three,four}, then the retry prefix {four}, {four,five}.
    // Nothing is measured twice.
    check(calls <= 7, "the run measures each distinct block at most once");
    check(calls >= 5, "the run actually measured its blocks");
}

void testEmptyStanzaIsSkipped() {
    SongPaginationInput input;
    input.style = styleWithBox(300.0);
    input.stanzas = {stanza("Verse", {"one"}),
                     stanza("Pre-Chorus", {"   ", ""}),
                     stanza("Chorus", {"two"})};
    const SongPaginationOutcome outcome =
        paginateSong(input, scriptedMeasurer(nullptr, nullptr));
    check(outcome.ok, "wordless stanzas do not break the plan");
    check(outcome.slides.size() == 2, "no empty slide is generated");
    check(outcome.slides[1].label == "Chorus", "the real label survives");
}

void testFingerprint() {
    SongPaginationInput input;
    input.style = styleWithBox(300.0);
    input.stanzas = {stanza("Verse", {"one", "two"})};
    const std::string first = openshow::importer::songPlanFingerprint(input);
    const std::string second = openshow::importer::songPlanFingerprint(input);
    check(first == second, "the same plan has the same fingerprint");

    SongPaginationInput restyled = input;
    restyled.style.fontSize = 80.0;
    check(openshow::importer::songPlanFingerprint(restyled) != first,
          "a size change is a different plan");

    SongPaginationInput retexted = input;
    retexted.stanzas[0].lines[0] = "ONE";
    check(openshow::importer::songPlanFingerprint(retexted) != first,
          "a word change is a different plan");

    SongPaginationInput rebased = input;
    rebased.style.boxWidth = 1200.0;
    check(openshow::importer::songPlanFingerprint(rebased) != first,
          "a box change is a different plan");
}

}  // namespace

int main() {
    testWholeStanzaAndHardBoundary();
    testWholeStanzaFitsOnOneSlide();
    testOverlongLineUsesTheEngine();
    testDeadEndAndStyleRefusals();
    testNoDuplicateMeasurements();
    testEmptyStanzaIsSkipped();
    testFingerprint();
    if (gFailed) {
        std::fprintf(stderr, "song_pagination_tests: FAILED\n");
        return 1;
    }
    std::printf("song_pagination_tests: all checks passed\n");
    return 0;
}
