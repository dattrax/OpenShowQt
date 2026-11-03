/*
 * Task TF.2 — the generic text-flow engine's properties, on the same kind
 * of scripted measuring engine BI.4 proved the scripture paginator on
 * (uniform glyph rows: monotone, honest, no knowledge of verses). The
 * point of these fixtures is that the properties are the ENGINE's, not
 * scripture's: stanzas with a stanza labeler pack, split and label by the
 * same rules, the packing is by measurement with no line count anywhere,
 * a block that fits alone is never split, and the dead end refuses
 * naming the block. That the real engine is the worker's SkParagraph is
 * pinned by text_measure_tests; that scripture still paginates exactly as
 * BI proved is pinned by scripture_pagination_tests running on this
 * engine unchanged.
 */
#include "openshow/importer/textflow.h"

#include <cmath>
#include <cstdio>
#include <memory>
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

size_t codepoints(const std::string& s) {
    size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

// The scripted engine, same model as the scripture fixture's: charWidth =
// size/2, every line at least one row, row height = size × lineSpacing.
struct MeasureScript {
    struct State {
        std::vector<std::string> asked;  // every block asked about, serialized
        std::vector<double> sizes;       // every fontSize the port was asked with
        int calls = 0;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    bool operator()(const std::vector<MeasureRequest>& requests,
                    std::vector<MeasureReply>* replies, std::string* error) {
        ++state->calls;
        for (const MeasureRequest& request : requests) {
            std::string line;
            for (const std::string& text : request.lines) {
                line += text + "\u0001";
            }
            state->asked.push_back(std::move(line));
            state->sizes.push_back(request.fontSize);
        }
        replies->clear();
        for (const MeasureRequest& request : requests) {
            double rows = 0.0;
            const double charWidth = request.fontSize / 2.0;
            for (const std::string& text : request.lines) {
                rows += text.empty() ? 1.0
                                     : std::ceil(codepoints(text) * charWidth / request.width);
            }
            MeasureReply reply;
            reply.key = request.key;
            reply.height = rows * request.fontSize * request.lineSpacing;
            reply.lineCount = static_cast<int>(rows);
            replies->push_back(reply);
        }
        return true;
    }
};

// The model's ruler: at size 10, spacing 1, a row is 10 px and a 100-px
// line holds 20 codepoints.
FlowStyle style(double width, double height, double size = 10.0) {
    FlowStyle s;
    s.boxWidth = width;
    s.boxHeight = height;
    s.fontSize = size;
    s.lineSpacing = 1.0;
    return s;
}

std::string stanzaLine(char tag) {
    // Exactly one 100-px row at size 10: 20 codepoints.
    return std::string(19, 'a' + (tag % 26)) + "!";
}

FlowBlock stanza(const std::string& id, int lines, char tag) {
    FlowBlock block;
    block.id = id;
    for (int i = 0; i < lines; ++i) {
        block.lines.push_back(stanzaLine(static_cast<char>(tag + i)));
    }
    return block;
}

// A stanza labeler: "2" / "2-3", with the continuation marks the caller
// of the engine owns — the engine only says which blocks and which edges.
std::string stanzaLabel(const std::vector<std::string>& ids, bool startsContinued,
                        bool continues) {
    if (ids.empty()) {
        return {};
    }
    std::string label = startsContinued ? "(cont.) " : "";
    label += ids.front() == ids.back() ? ids.front() : ids.front() + "-" + ids.back();
    if (continues) {
        label += " (continues)";
    }
    return label;
}

// The words an input's blocks hold, in order, whitespace stripped.
std::string wordsOf(const std::vector<FlowBlock>& blocks) {
    std::string out;
    for (const FlowBlock& block : blocks) {
        for (const std::string& line : block.lines) {
            out += line;
        }
    }
    return out;
}

std::string stripped(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

void testStanzaPackingWithStanzaLabels() {
    // Four two-line stanzas, a box that measures out exactly four rows:
    // two per slide, whole lines preserved, the labeler asked per slide
    // with the right ids and edges.
    MeasureScript script;
    FlowInput input;
    input.style = style(100.0, 40.0);
    input.subject = "the lyrics";
    input.labeler = stanzaLabel;
    input.blocks = {stanza("1", 2, 'a'), stanza("2", 2, 'c'), stanza("3", 2, 'e'),
                    stanza("4", 2, 'g')};
    const FlowOutcome out = runTextFlow(input, script);
    check(out.ok, "four stanzas pack under the stanza labeler");
    check(out.slides.size() == 2,
          "the box measured out two stanzas per slide — no line count said so");
    if (out.slides.size() == 2) {
        check(out.slides[0].blockIds == std::vector<std::string>({"1", "2"}) &&
                  out.slides[1].blockIds == std::vector<std::string>({"3", "4"}),
              "each slide names the stanzas its words belong to");
        check(out.slides[0].label == "1-2" && out.slides[1].label == "3-4",
              "the stanza labeler composes the ranges");
        check(out.slides[0].lines.size() == 4 && out.slides[1].lines.size() == 4,
              "a whole multi-line block keeps its line structure on the slide");
        check(!out.slides[0].startsContinued && !out.slides[0].continues &&
                  !out.slides[1].startsContinued && !out.slides[1].continues,
              "no slide is a continuation: nothing was split");
    }
    std::string packed;
    for (const FlowSlide& slide : out.slides) {
        for (const std::string& line : slide.lines) {
            packed += line;
        }
    }
    check(stripped(packed) == stripped(wordsOf(input.blocks)),
          "the slides hold the stanzas' words exactly — nothing dropped, added or repeated");
    const std::set<std::string> askedOnce(script.state->asked.begin(), script.state->asked.end());
    check(askedOnce.size() == script.state->asked.size(),
          "no block is measured twice in one run at one size");
}

void testTheLineCountIsWhatFits() {
    // Same blocks, three box heights: the slide count follows the
    // measurement, because the input carries no number to override it.
    MeasureScript script;
    FlowInput input;
    input.subject = "the lyrics";
    input.blocks = {stanza("1", 2, 'a'), stanza("2", 2, 'c'), stanza("3", 2, 'e'),
                    stanza("4", 2, 'g')};
    std::vector<std::size_t> counts;
    for (const double height : {20.0, 40.0, 80.0}) {
        input.style = style(100.0, height);
        const FlowOutcome out = runTextFlow(input, script);
        counts.push_back(out.ok ? out.slides.size() : 0);
    }
    check(counts == std::vector<std::size_t>({4, 2, 1}),
          "20/40/80 px of box measure out 4/2/1 slides of the same stanzas");
}

void testOneWholeBlockFitsAloneIsNeverSplit() {
    // Exactly the box height: one block, one slide, no continuation edge,
    // and no split-probe ever asked the port for a fragment of it.
    MeasureScript script;
    FlowInput input;
    input.style = style(100.0, 20.0);
    input.subject = "the lyrics";
    input.labeler = stanzaLabel;
    input.blocks = {stanza("V", 2, 'm')};
    const FlowOutcome out = runTextFlow(input, script);
    check(out.ok && out.slides.size() == 1, "a block that fits alone becomes one slide");
    if (!out.slides.empty()) {
        check(!out.slides[0].continues && !out.slides[0].startsContinued,
              "and it is not a continuation of anything");
        check(out.slides[0].lines.size() == 2, "its two lines arrive as two lines");
        check(out.slides[0].label == "V", "the labeler sees one whole stanza");
    }
    bool askedFragment = false;
    for (const std::string& asked : script.state->asked) {
        // Only whole-block stacks may be asked for a fitting block: never
        // a single-sentence or single-word fragment of it.
        if (asked == stanzaLine('m') + "\u0001") {
            askedFragment = true;
        }
    }
    check(!askedFragment, "no fragment of a fitting block was ever put to the port");
}

void testSplitCarriesIdentityAndContinuation() {
    // One block too long for the box: split at sentence boundaries, both
    // slides naming the SAME block id, the second knowing it continues.
    // The box measures out exactly one sentence per slide (the first
    // sentence is 5 rows of this script's 10 px rows, the second 6): the
    // whole block needs 11, so it splits — and at 60 px each half fits.
    MeasureScript script;
    FlowInput input;
    input.style = style(100.0, 60.0);
    input.subject = "the lyrics";
    input.labeler = stanzaLabel;
    FlowBlock longOne;
    longOne.id = "3";
    longOne.lines = {"one two three four five six seven eight nine ten eleven twelve thirteen "
                     "fourteen fifteen!",
                     "sixteen seventeen eighteen nineteen twenty twenty-one twenty-two twenty-three "
                     "twenty-four twenty-five!"};
    input.blocks = {longOne};
    const FlowOutcome out = runTextFlow(input, script);
    check(out.ok && out.slides.size() == 2,
          "a block longer than the box splits across two slides");
    if (out.slides.size() == 2) {
        check(out.slides[0].blockIds == std::vector<std::string>({"3"}) &&
                  out.slides[1].blockIds == std::vector<std::string>({"3"}),
              "both continuations name the same block");
        check(!out.slides[0].startsContinued && out.slides[0].continues,
              "the first slide hands off");
        check(out.slides[1].startsContinued && !out.slides[1].continues,
              "the second slide receives");
        check(out.slides[0].label == "3 (continues)" && out.slides[1].label == "(cont.) 3",
              "the stanza labeler says hand-off and receipt in the label");
    }
    std::string packed;
    for (const FlowSlide& slide : out.slides) {
        for (const std::string& line : slide.lines) {
            packed += line + " ";
        }
    }
    check(stripped(packed) == stripped(wordsOf(input.blocks)),
          "the split loses nothing: the words reassemble exactly");
}

void testNoFitRefusalNamesItself() {
    // A single word wider than the box: the honest dead end, named — with
    // the block's id when it has one, with the subject when it has none.
    MeasureScript script;
    FlowInput input;
    input.style = style(50.0, 20.0);  // a row holds 10 codepoints; this word holds 40
    input.subject = "the lyrics";
    FlowBlock wide;
    wide.id = "Chorus 2";
    wide.lines = {std::string(40, 'w')};
    input.blocks = {wide};
    const FlowOutcome out = runTextFlow(input, script);
    check(!out.ok, "a word wider than the box is refused, not clipped");
    check(out.error.find("single word") != std::string::npos &&
              out.error.find("Chorus 2") != std::string::npos,
          "the refusal names the block it cannot place");

    FlowInput anonymous = input;
    anonymous.blocks = {FlowBlock{{}, wide.lines}};
    const FlowOutcome out2 = runTextFlow(anonymous, script);
    check(!out2.ok && out2.error.find("the lyrics") != std::string::npos,
          "an anonymous block is refused naming the subject instead");
}

void testReservationIsHonouredAndRefused() {
    // The reservation stands above the words: it is counted out of the
    // box, and when it eats the box the run refuses with its own sentence.
    MeasureScript script;
    FlowInput input;
    input.style = style(100.0, 40.0);
    input.subject = "the lyrics";
    input.blocks = {stanza("1", 2, 'a'), stanza("2", 2, 'c')};
    input.reservedHeight = 20.0;  // one stanza's worth left for two
    const FlowOutcome tight = runTextFlow(input, script);
    check(tight.ok && tight.slides.size() == 2 && tight.availableHeight == 20.0,
          "the reserved room comes off the box before the words are packed");

    FlowInput eaten = input;
    eaten.reservedHeight = 40.0;
    const FlowOutcome blocked = runTextFlow(eaten, script);
    check(!blocked.ok && blocked.error.find("no room") != std::string::npos,
          "a reservation that eats the box refuses with its own sentence");
}

void testMeasureReservationSumsAtTheAskedSize() {
    MeasureScript script;
    FlowStyle s = style(100.0, 100.0);
    double height = 0.0;
    std::string error;
    const bool ok = measureReservation({{"0123456789"}, {"01234567890123456789"}}, s, 10.0, script,
                                       &height, &error);
    // 10 codepoints and 20 codepoints at size 10 in a 100-px box: one row
    // each, 10 px per row.
    check(ok && height == 20.0, "the reservation is the summed measured height");
    check(script.state->sizes.size() == 2 && script.state->sizes[0] == 10.0,
          "and it was asked at exactly the size the caller named");
}

void testEmptyAndUnusableInputsRefuseThemselves() {
    MeasureScript script;
    FlowInput input;
    input.style = style(100.0, 40.0);
    check(!runTextFlow(input, script).ok, "no blocks is refused");

    FlowInput blank = input;
    blank.blocks = {FlowBlock{{}, {"   "}}, FlowBlock{{}, {}}};
    check(!runTextFlow(blank, script).ok, "blocks of nothing are refused too");

    FlowInput broken = input;
    broken.blocks = {stanza("1", 1, 'a')};
    broken.style.boxWidth = 0.0;
    const FlowOutcome out = runTextFlow(broken, script);
    check(!out.ok && out.error.find("measured") != std::string::npos,
          "a box with no width is refused with the measurement sentence");
    check(script.state->calls == 0, "and the refusal asked the port nothing");
}

void testAnonymousSlidesCarryNoLabelsButCarryIds() {
    // No labeler at all: slides still carry their words and their block
    // ids, and empty ids stay empty rather than inventing names.
    MeasureScript script;
    FlowInput input;
    input.style = style(100.0, 20.0);
    input.blocks = {stanza("", 2, 'p')};
    const FlowOutcome out = runTextFlow(input, script);
    check(out.ok && out.slides.size() == 1 && out.slides[0].label.empty() &&
              out.slides[0].blockIds.size() == 1 && out.slides[0].blockIds[0].empty(),
          "without a labeler the slides are words and ids only");
}

}  // namespace

int main() {
    testStanzaPackingWithStanzaLabels();
    testTheLineCountIsWhatFits();
    testOneWholeBlockFitsAloneIsNeverSplit();
    testSplitCarriesIdentityAndContinuation();
    testNoFitRefusalNamesItself();
    testReservationIsHonouredAndRefused();
    testMeasureReservationSumsAtTheAskedSize();
    testEmptyAndUnusableInputsRefuseThemselves();
    testAnonymousSlidesCarryNoLabelsButCarryIds();

    if (gFailed > 0) {
        std::printf("textflow_tests: %d FAILURES\n", gFailed);
        return 1;
    }
    std::printf("textflow_tests: the generic engine packs, splits and refuses on measurement\n");
    return 0;
}
