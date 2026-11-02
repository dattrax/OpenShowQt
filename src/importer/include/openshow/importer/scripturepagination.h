// Measured scripture pagination (milestone BI, task BI.4).
//
// "How do the selected verses sit in this theme's box at the chosen size?"
// is answered ONLY by the worker's own text engine, handed in as the port
// below (the frontend implements it over the text.measure RPC; the tests
// script it). There is no character-count estimate here, no second
// renderer, and no shrinking — the size is chosen and the WORDS move,
// either onto further slides or, for a verse too long for any box, across
// continuation slides split at a sentence boundary where one is available
// and at a word boundary otherwise. A whitespace split can never cut a
// Unicode character: UTF-8 continuation bytes are ≥ 0x80 and every
// considered whitespace is ASCII.
//
// The words are the whole of a generated slide's lines: the reference and
// the attribution are NOT slide text (the schedule item carries them as
// provenance and the slide labels ride in the slide notes), but both are
// measured in the same box at the chosen typography and their height is
// RESERVED out of it — the plan's "reserving room for references and
// attribution" and "references fit at the chosen size", made numeric. When
// the reservation leaves no room for the words, or a single word will not
// fit, this says so with its own sentence and generates nothing: Add is
// blocked by explanation, never by silent clipping.
//
// Qt-free on purpose: the pagination is a pure function over its input and
// the measuring port, so the ordering/no-repeats/no-loss properties are
// testable with no network, no worker and no display.
#ifndef OPENSHOW_IMPORTER_SCRIPTUREPAGINATION_H
#define OPENSHOW_IMPORTER_SCRIPTUREPAGINATION_H

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace openshow::importer {

// One block of words put to the measuring engine: exactly the knobs the
// worker's layout reads (see protocol.h's text.measure contract). `key`
// correlates the answer; the paginator keys its cache on the CONTENT, so
// an identical block is measured at most once per run.
struct MeasureRequest {
    std::string key;
    std::vector<std::string> lines;
    double fontSize = 0.0;
    double width = 0.0;
    double lineSpacing = 1.25;
    std::string fontFamily;
};

struct MeasureReply {
    std::string key;
    double height = 0.0;
    int lineCount = 0;
};

// The measuring port. Returns true only when EVERY request was answered,
// in order, with matching keys — a partial or shuffled answer is a failure,
// because half a page's heights is exactly the guess this task exists to
// forbid. The frontend's adapter implements this over
// WorkerController::measureText (whose answers come from the worker's
// SkParagraph/ICU layout).
using MeasureFn = std::function<bool(const std::vector<MeasureRequest>& requests,
                                     std::vector<MeasureReply>* replies,
                                     std::string* error)>;

// One selected verse, in the order the passage gave them.
struct PaginationVerse {
    std::string reference;  // the page's own "chapter:verse", e.g. "1:2"
    std::string text;       // plain text, whitespace already collapsed
};

// The resolved theme's primary text box and the CHOSEN typography, in
// output-canvas pixels. Nothing here is a suggestion the paginator may
// scale: it measures at exactly these numbers or the ask fails.
struct PaginationStyle {
    double boxWidth = 0.0;
    double boxHeight = 0.0;
    double fontSize = 0.0;
    double lineSpacing = 1.25;
    std::string fontFamily;
};

struct PaginationInput {
    std::vector<PaginationVerse> verses;  // the selection, in order
    std::string reference;   // what the operator looked up; reserves room
    std::string attribution; // the licence text; reserves room, may be empty
    PaginationStyle style;
};

// One generated slide. `lines` is the slide's ONLY text and holds ONLY
// passage words — concatenating every slide's lines in order preserves the
// selected passage exactly (after whitespace normalisation): no word is
// dropped, added or repeated. `label` names the verses the slide covers
// ("1:1-1:3", with "(cont.)" / "(continues)" marks when the slide starts
// or ends mid-verse) and belongs in the slide's NOTES, not on it.
struct GeneratedSlide {
    std::vector<std::string> lines;
    std::string label;
};

struct PaginationOutcome {
    bool ok = false;
    std::string error;  // the operator's own sentence; empty when ok
    std::vector<GeneratedSlide> slides;
    // What the reservation cost, for the UI to say so honestly ("words
    // area after reference/attribution: N px"). Only meaningful when ok.
    double reservedHeight = 0.0;
    double availableHeight = 0.0;
};

// Paginate. The measure port is called only with whole batches of blocks
// this run has not measured yet, at most kMaxMeasureChunk per call (the
// wire's batch cap, mirrored so a legal run never meets a refusal).
PaginationOutcome paginateScripture(const PaginationInput& input, const MeasureFn& measure);

// The batch cap one MeasureFn call never exceeds (mirrors the protocol's
// kMaxMeasureBatch; kept here so this Qt-free library stands alone).
inline constexpr int kMaxMeasureChunk = 32;

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

#endif  // OPENSHOW_IMPORTER_SCRIPTUREPAGINATION_H
