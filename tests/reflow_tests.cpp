/*
 * Task PT.5: re-pagination of words a service already holds. Qt-free and
 * scripted: the measuring port answers a deterministic line height, so the
 * section policy, greedy passage packing, splitting/continuation labels,
 * caption reservation and the no-loss arithmetic are pinned with no worker
 * and no display.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "openshow/importer/reflowpagination.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::domain::ReflowSourceBlock;
using openshow::importer::MeasureReply;
using openshow::importer::MeasureRequest;
using openshow::importer::ReflowPaginationInput;
using openshow::importer::ReflowPaginationOutcome;
using openshow::importer::ReflowPolicy;
using openshow::importer::ReflowSlide;
using openshow::importer::reflowWordsPreserved;

// A deterministic measurer: one line of type is fontSize * lineSpacing tall
// (fontSize 0 asks at 40, the renderer's own assumed default), and a block
// is its line count. No width wrapping, so the tests are about policy.
openshow::importer::MeasureFn measurer() {
    return [](const std::vector<MeasureRequest>& requests,
              std::vector<MeasureReply>* replies, std::string* error) {
        for (const MeasureRequest& request : requests) {
            MeasureReply reply;
            reply.key = request.key;
            const double size = request.fontSize > 0.0 ? request.fontSize : 40.0;
            // A crude width model: half an em per byte at the chosen size.
            // Without it a long line would "fit" any narrow box and the
            // split paths would never be exercised.
            const double charWidth = size * 0.5;
            const int perLine = std::max(1, static_cast<int>(request.width / charWidth));
            std::size_t layoutLines = 0;
            for (const std::string& line : request.lines) {
                const std::size_t chars = line.empty() ? 1 : line.size();
                layoutLines += (chars + static_cast<std::size_t>(perLine) - 1) /
                               static_cast<std::size_t>(perLine);
            }
            reply.height = static_cast<double>(layoutLines) * size * request.lineSpacing;
            reply.lineCount = static_cast<int>(layoutLines);
            replies->push_back(std::move(reply));
        }
        (void)error;
        return true;
    };
}

openshow::importer::FlowStyle styleFor(double fontSize, double lineSpacing,
                                       double boxHeight) {
    openshow::importer::FlowStyle style;
    style.boxWidth = 1000.0;
    style.boxHeight = boxHeight;
    style.fontSize = fontSize;
    style.lineSpacing = lineSpacing;
    style.fontFamily = "Test Sans";
    return style;
}

std::vector<std::string> allLines(const std::vector<ReflowSlide>& slides) {
    std::vector<std::string> lines;
    for (const ReflowSlide& slide : slides) {
        for (const std::string& line : slide.lines) {
            lines.push_back(line);
        }
    }
    return lines;
}

void testSectionPolicy() {
    ReflowPaginationInput input;
    input.style = styleFor(40.0, 1.25, 1000.0);  // plenty of room for both
    input.policy = ReflowPolicy::kSectionStartsNewSlide;
    input.blocks = {ReflowSourceBlock{"Verse 1", {"First", "Second"}},
                    ReflowSourceBlock{"Chorus", {"Sing"}}};
    const ReflowPaginationOutcome packed =
        openshow::importer::paginateReflow(input, measurer());
    check(packed.ok, "a song reflow plans");
    check(packed.slides.size() == 2,
          "a section never shares a slide, even with room to spare");
    check(packed.slides[0].lines == std::vector<std::string>{"First", "Second"} &&
              packed.slides[0].label == "Verse 1" &&
              packed.slides[0].blockIndex == 0 &&
              packed.slides[1].lines == std::vector<std::string>{"Sing"} &&
              packed.slides[1].label == "Chorus" &&
              packed.slides[1].blockIndex == 1,
          "each slide keeps its own section's words and identity");
    check(reflowWordsPreserved(input.blocks, packed.slides),
          "the song plan preserves every word in order");
}

void testGreedyPassagePolicy() {
    ReflowPaginationInput input;
    input.style = styleFor(40.0, 1.25, 1000.0);
    input.policy = ReflowPolicy::kGreedyBlocks;
    input.blocks = {ReflowSourceBlock{"1 Corinthians 1:1", {"Paul, called"}},
                    ReflowSourceBlock{"1 Corinthians 1:3", {"Grace and peace"}}};
    const ReflowPaginationOutcome packed =
        openshow::importer::paginateReflow(input, measurer());
    check(packed.ok && packed.slides.size() == 1,
          "a passage packs whole verse blocks greedily");
    check(packed.slides[0].lines ==
              std::vector<std::string>{"Paul, called", "Grace and peace"},
          "both verses share the slide in authored order");
    check(packed.slides[0].label == "1 Corinthians 1:1-1 Corinthians 1:3",
          "the label names the whole span the slide covers");
    check(reflowWordsPreserved(input.blocks, packed.slides),
          "the passage plan preserves every word in order");
}

void testSongSectionsReassembleBeforeFitting() {
    ReflowPaginationInput input;
    input.policy = ReflowPolicy::kSectionStartsNewSlide;
    input.style = styleFor(40.0, 1.25, 1000.0);
    input.blocks = {{"Verse 1", {"First", "Second"}},
                    {"Verse 1 (cont.)", {"Third"}},
                    {"Chorus", {"Sing"}}, {"Chorus (cont.)", {"Again"}},
                    {"Verse 2", {"New words"}},
                    {"Chorus", {"Sing"}}, {"Chorus", {"Again"}}};
    const auto roomy = openshow::importer::paginateReflow(input, measurer());
    check(roomy.ok && roomy.slides.size() == 4,
          "old slide fragments recombine into one roomy slide per section occurrence");
    if (!roomy.ok || roomy.slides.size() != 4) return;
    check(roomy.slides[0].lines == std::vector<std::string>{"First", "Second", "Third"} &&
              roomy.slides[0].label == "Verse 1" && roomy.slides[0].blockIndex == 0 &&
              roomy.slides[1].lines == std::vector<std::string>{"Sing", "Again"} &&
              roomy.slides[1].blockIndex == 2 && roomy.slides[2].label == "Verse 2" &&
              roomy.slides[3].lines == std::vector<std::string>{"Sing", "Again"} &&
              roomy.slides[3].blockIndex == 5,
          "joining fragments preserves section identity, repeated choruses and running order");
    input.style.boxHeight = 100.0;
    const auto tight = openshow::importer::paginateReflow(input, measurer());
    check(tight.ok && tight.slides.size() == 5 &&
              tight.slides[0].lines == std::vector<std::string>{"First", "Second"} &&
              tight.slides[1].lines == std::vector<std::string>{"Third"} &&
              tight.slides[1].label == "Verse 1 (cont.)" &&
              tight.slides[1].startsContinued,
          "the complete reconstructed verse is split again using the new fitting");
    check(reflowWordsPreserved(input.blocks, roomy.slides) &&
              reflowWordsPreserved(input.blocks, tight.slides),
          "section reconstruction never loses or duplicates words");
}

void testSplittingAndContinuation() {
    ReflowPaginationInput input;
    // Two lines fit (2 * 40 * 1.25 = 100 <= 100).
    input.style = styleFor(40.0, 1.25, 100.0);
    input.policy = ReflowPolicy::kSectionStartsNewSlide;
    input.blocks = {ReflowSourceBlock{"Verse 1", {"One", "Two", "Three"}}};
    const ReflowPaginationOutcome packed =
        openshow::importer::paginateReflow(input, measurer());
    check(packed.ok && packed.slides.size() == 2,
          "an overflowing stanza splits at authored lines");
    check(packed.slides[0].lines == std::vector<std::string>{"One", "Two"} &&
              packed.slides[1].lines == std::vector<std::string>{"Three"},
          "the split keeps authored order and the whole lines");
    check(packed.slides[1].startsContinued &&
              packed.slides[1].label.find("(cont.)") != std::string::npos,
          "the continuation is marked and carries the section identity");

    // A passage block that does not fit splits by sentence/word, and every
    // continuation still names the whole passage rather than a guessed verse.
    ReflowPaginationInput passage;
    passage.style = styleFor(40.0, 1.25, 50.0);  // one line tall...
    passage.style.boxWidth = 200.0;               // ...and ten characters wide
    passage.policy = ReflowPolicy::kGreedyBlocks;
    passage.fallbackLabel = "The passage";
    passage.blocks =
        {ReflowSourceBlock{"", {"alpha beta gamma delta epsilon"}}};
    const ReflowPaginationOutcome flow =
        openshow::importer::paginateReflow(passage, measurer());
    check(flow.ok && flow.slides.size() >= 2,
          "an over-long passage block splits across slides");
    check(flow.slides[0].label.rfind("The passage", 0) == 0,
          "the first slide names the whole known passage");
    bool laterContinuations = true;
    for (std::size_t i = 1; i < flow.slides.size(); ++i) {
        laterContinuations = laterContinuations &&
                             flow.slides[i].label.rfind("(cont.) The passage", 0) == 0;
    }
    check(laterContinuations,
          "every continuation marks itself and keeps the passage identity");
    check(reflowWordsPreserved(passage.blocks, flow.slides),
          "the split passage keeps every word");
}

void testCaptionReservation() {
    ReflowPaginationInput without;
    without.style = styleFor(40.0, 1.25, 200.0);
    without.policy = ReflowPolicy::kGreedyBlocks;
    without.blocks = {ReflowSourceBlock{"1:1", {"One", "Two", "Three", "Four"}}};
    const ReflowPaginationOutcome plain =
        openshow::importer::paginateReflow(without, measurer());
    check(plain.ok && plain.reservedHeight == 0.0,
          "nothing is reserved for a passage without provenance");

    ReflowPaginationInput with = without;
    with.reference = "1 Corinthians 1:1-6";
    with.attribution = "Scripture quotations taken from NIV";
    const ReflowPaginationOutcome reserved =
        openshow::importer::paginateReflow(with, measurer());
    check(reserved.ok && reserved.reservedHeight > 0.0,
          "a passage reserves measured caption room");
    check(reserved.slides.size() >= plain.slides.size(),
          "the reservation can only add slides, never fewer");
    check(reflowWordsPreserved(with.blocks, reserved.slides),
          "the reservation changes the packing, never the words");
}

void testRefusalsAndNoLoss() {
    ReflowPaginationInput empty;
    empty.style = styleFor(40.0, 1.25, 500.0);
    const ReflowPaginationOutcome none =
        openshow::importer::paginateReflow(empty, measurer());
    check(!none.ok && none.error.find("no words") != std::string::npos,
          "an item with no words is refused by name");

    ReflowPaginationInput bad;
    bad.style = styleFor(40.0, 1.25, 0.0);
    bad.blocks = {ReflowSourceBlock{"Verse", {"One"}}};
    const ReflowPaginationOutcome refused =
        openshow::importer::paginateReflow(bad, measurer());
    check(!refused.ok, "an unusable box is refused before measuring");

    // Unicode and blank lines survive the word arithmetic.
    ReflowPaginationInput text;
    text.style = styleFor(40.0, 1.25, 1000.0);
    text.policy = ReflowPolicy::kGreedyBlocks;
    text.blocks = {ReflowSourceBlock{"", {"Grüße, Welt", "", "  two   spaces  "}}};
    const ReflowPaginationOutcome unicode =
        openshow::importer::paginateReflow(text, measurer());
    check(unicode.ok && reflowWordsPreserved(text.blocks, unicode.slides),
          "Unicode words and authored spacing survive the plan");

    std::vector<ReflowSlide> tampered = unicode.slides;
    tampered.back().lines.push_back("extra");
    check(!reflowWordsPreserved(text.blocks, tampered),
          "a plan that gained a word is detected");
    tampered = unicode.slides;
    tampered.front().lines.clear();
    check(!reflowWordsPreserved(text.blocks, tampered),
          "a plan that lost its words is detected");
}

}  // namespace

int main() {
    testSectionPolicy();
    testGreedyPassagePolicy();
    testSongSectionsReassembleBeforeFitting();
    testSplittingAndContinuation();
    testCaptionReservation();
    testRefusalsAndNoLoss();
    if (gFailed) {
        std::fprintf(stderr, "reflow_tests: FAILED\n");
        return 1;
    }
    std::printf("reflow_tests: all checks passed\n");
    return 0;
}
