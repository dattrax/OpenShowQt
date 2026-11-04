// The generic measured text-flow engine (milestone TF).
//
// "How does this run of text sit in this box at this typography?" is one
// question with one answerer: the worker's own text engine, handed in as
// the MeasureFn port below (the frontend implements it over the
// text.measure RPC; the tests script it). The packing shape belongs to no
// content kind — scripture feeds it verses, the future ASCII song import
// will feed it stanzas — so it lives here, out of the scripture paginator,
// unchanged in every particular BI proved: greedy whole-block packing
// first, sentence split before word split, continuation carried with its
// block's identity, nothing clipped, nothing estimated, no size
// shrinking — the size is the size and the WORDS move.
//
// What the input does NOT carry: any line count. How many lines a slide
// holds is exactly what the measurement fits in the box at the given
// typography. A theme that wants "a reading slide" says so with a box and
// a reading size; the engine measures out the lines. There is no number
// here to set to six.
//
// A block is the unit that stays whole if it can: a verse, a stanza, a
// paragraph. When the head block alone will not fit the box, the engine
// splits it — sentences first, words as the fallback — and the remainder
// carries the SAME block id and knows it starts mid-block, so the labeler
// can say "(cont.)" honestly. A whitespace split can never cut a Unicode
// character: UTF-8 continuation bytes are >= 0x80 and every considered
// whitespace is ASCII.
//
// Qt-free on purpose, like the rest of openshow::importer: the engine is
// a pure function over its input and the measuring port, so the
// ordering/no-repeats/no-loss properties are testable with no network, no
// worker and no display.
#ifndef OPENSHOW_IMPORTER_TEXTFLOW_H
#define OPENSHOW_IMPORTER_TEXTFLOW_H

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openshow/domain/documents.h"

namespace openshow::importer {

// One thing put to the measuring engine: exactly the knobs the worker's
// layout reads (see protocol.h's text.measure contract). `key` correlates
// the answer; the engine keys its cache on the COMPLETE typography of a
// question (measureCacheKey), so an identical block at identical type is
// measured at most once per run — and the same block at a different
// weight, face or effect is a DIFFERENT question, never a cache hit.
struct MeasureRequest {
    std::string key;
    std::vector<std::string> lines;
    double fontSize = 0.0;
    double width = 0.0;
    double lineSpacing = 1.25;
    std::string fontFamily;
    // Task TS.3: the resolved style block — the same scene fields the
    // item the audience sees will wear. A packer that measured bold italic
    // words with default metrics packed a page the styled slide overflows.
    int fontWeight = openshow::domain::kDefaultFontWeight;
    std::string fontStyle = "normal";
    std::string fitMode = "fixed";  // measurement never shrinks whatever says
    double minFontSize = openshow::domain::kDefaultMinFontSize;
    openshow::domain::TextStroke stroke;
    openshow::domain::TextShadow shadow;
};

struct MeasureReply {
    std::string key;
    double height = 0.0;
    int lineCount = 0;
    // Task TS.3: the worker's verdict on whether the block's FILL — the
    // laid-out lines themselves, effects not folded in — stays inside the
    // measured width (and height, if the ask carried one). Effects belong
    // to the reply's bounds at the RPC seam, not to this verdict: a
    // shadow's spill is the theme author's choice and no repagination
    // shortens it, so folding it in would refuse every page of a shadowed
    // theme. Absent (a pre-16 worker) means unknown, never "yes": the
    // packer then judges with the height criterion alone, exactly as it
    // did before the verdict existed — the honest floor, not a claim.
    std::optional<bool> fits;
};

// The measuring port (the engine's AwaitingMeasurement edge: a run that
// meets an unanswered batch stops with its own sentence and suspends no
// assumption). Returns true only when EVERY request was answered, in
// order, with matching keys — a partial or shuffled answer is a failure,
// because half a page's heights is exactly the guess this engine exists
// to forbid. The frontend's adapter implements this over
// WorkerController::measureText (whose answers come from the worker's
// SkParagraph/ICU layout).
using MeasureFn = std::function<bool(const std::vector<MeasureRequest>& requests,
                                     std::vector<MeasureReply>* replies,
                                     std::string* error)>;

// The batch cap one MeasureFn call never exceeds (mirrors the protocol's
// kMaxMeasureBatch; kept here so this Qt-free library stands alone).
inline constexpr int kMaxMeasureChunk = 32;

// Task TS.3: the canonical string form of a question's COMPLETE
// typography — content, box and style. Two requests with equal keys are
// answered by equal layouts, and only the wire's correlation `key` is
// left out (no answer depends on it). The engine's per-run cache and the
// frontend's across-run cache share this one serializer on purpose: a
// second hand-rolled key is exactly how a stale measurement — the same
// words answered at a theme the operator has since changed — slips back
// in through the door the key was supposed to shut.
std::string measureCacheKey(const MeasureRequest& request);

// The ceiling on a measurement cache (per-run here, across-run in the
// frontend). A whole-Bible import probes tens of thousands of distinct
// fragments, and an unbounded map of them is a cost with no operator
// in front of it to justify it; past the cap a run keeps its hits and
// stops keeping new answers (measuring fresh is correct, just not cheap).
inline constexpr std::size_t kMaxMeasureCacheEntries = 4096;

// The box and the CHOSEN typography, in output-canvas pixels. Nothing
// here is a suggestion the engine may scale: it measures at exactly these
// numbers or the ask fails. `fontSize` follows the scene's own contract:
// 0 is not "no size" but the RENDERER's default — the number lives in the
// worker alone, and the engine forwards 0 to the measuring port
// unchanged, which measures it at exactly what the drawing will use.
struct FlowStyle {
    double boxWidth = 0.0;
    double boxHeight = 0.0;
    double fontSize = 0.0;
    double lineSpacing = 1.25;
    std::string fontFamily;
    // Task TS.3: the resolved style the packed slides will wear, on the
    // same scene-field terms MeasureRequest carries (the packer forwards
    // it verbatim), so the box is packed with the metrics of the type
    // the audience gets. validateFlowStyle vettes the bands.
    int fontWeight = openshow::domain::kDefaultFontWeight;
    std::string fontStyle = "normal";
    std::string fitMode = "fixed";  // the flow's own premise: the size is the size
    double minFontSize = openshow::domain::kDefaultMinFontSize;
    openshow::domain::TextStroke stroke;
    openshow::domain::TextShadow shadow;
};

// One ordered unit of words. `id` is whatever the caller needs its slides
// labelled by — a verse reference, a stanza number, "" for anonymous
// runs — and it survives a split: every continuation names the same id.
struct FlowBlock {
    std::string id;
    std::vector<std::string> lines;
};

// One generated slide's label, composed by the caller. `blockIds` are the
// blocks whose words appear on the slide, in order; `startsContinued`
// says the slide's first words are the tail of a block an earlier slide
// split; `continues` says the slide's last block is not finished — its
// remainder opens the next slide.
using LabelFn = std::function<std::string(const std::vector<std::string>& blockIds,
                                          bool startsContinued, bool continues)>;

struct FlowInput {
    std::vector<FlowBlock> blocks;
    FlowStyle style;
    // Height already measured and taken out of the box before the words
    // (a reference line, an attribution — the adapter measures its own,
    // at whatever size suits it; the engine only honours and reports it).
    double reservedHeight = 0.0;
    // The noun refusals speak of ("the passage", "the lyrics"); empty is
    // "the text". Refusals name themselves with this and the block id.
    std::string subject;
    LabelFn labeler;  // optional; empty labels every slide ""
};

struct FlowSlide {
    std::vector<std::string> blockIds;  // whose words these are, in order
    std::vector<std::string> lines;     // the slide's ONLY text
    std::string label;                  // the labeler's answer for this slide
    bool startsContinued = false;       // opens on a split block's tail
    bool continues = false;             // its last block keeps going
};

struct FlowOutcome {
    bool ok = false;
    std::string error;  // the operator's own sentence; empty when ok
    std::vector<FlowSlide> slides;
    // What the box had left for the words after the reservation. Only
    // meaningful when ok.
    double availableHeight = 0.0;
};

// Vet the box and typography the same way the packer does before any
// measurement — an adapter that wants to refuse an unusable box before it
// even reserves gets the engine's own sentence rather than a copy. Task
// TS.3 vets the style block in the same pass: the scene bands for weight,
// slant, fit mode and floor, and the effect records' own ranges, refusal
// naming the field. A half-checked style is how a NaN sigma reaches the
// wire and the whole batch answers with nothing.
bool validateFlowStyle(const FlowStyle& style, std::string* error);

// Measure a block's height through the port at a given size and sum it —
// the one-shot question an adapter asks to reserve room (a reference
// line, an attribution), so a reservation stays arithmetic over
// measurement, never arithmetic over guesses. Batch discipline and answer
// vetting are the engine's own.
bool measureReservation(const std::vector<std::vector<std::string>>& blocks,
                        const FlowStyle& style, double fontSize, const MeasureFn& measure,
                        double* height, std::string* error);

// Pack. Greedy whole blocks first; the head block splits (sentences, then
// words) only when it alone will not fit; a single first word that will
// not fit ANY box is the honest dead end and refuses by name — clipping
// and shrinking are both forbidden, so the caller blocks with this
// sentence. The measure port is called only with whole batches of blocks
// this run has not measured yet, at most kMaxMeasureChunk per call.
// Concatenating every slide's lines in order, then stripping whitespace,
// restores the blocks' words exactly: nothing dropped, added or repeated.
FlowOutcome runTextFlow(const FlowInput& input, const MeasureFn& measure);

// UTF-8-safe split helpers, exposed so the "never split inside a Unicode
// character" claim is testable directly. Sentences end at '.' '!' '?'
// U+0964 U+3002 U+FF01 U+FF1F followed by whitespace or the end of the
// text; the punctuation rides with the sentence that owns it. Words split
// at runs of ASCII whitespace. Both drop the separator whitespace; joining
// the parts of a passage back and stripping whitespace restores it
// exactly, which is the property the acceptance asserts.
std::vector<std::string> splitSentences(std::string_view text);
std::vector<std::string> splitWords(std::string_view text);

}  // namespace openshow::importer

#endif  // OPENSHOW_IMPORTER_TEXTFLOW_H
