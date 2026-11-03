// Measured scripture pagination (milestone BI, task BI.4) — as of task
// TF.2 this is an ADAPTER over the generic text-flow engine
// (openshow/importer/textflow.h), and thin by construction: it turns the
// selected verses into blocks (whole verses in canonical order, each one
// line, referenced by its "chapter:verse"), measures the reference and
// attribution reservation in caption type, composes the verse-range
// labels, and hands the packing to the engine. Everything BI proved about
// the packing — greedy whole verses, sentence then word split,
// continuation labels, no clipping, no repeats, no shrinking, the worker
// as the only measurer — is the engine's, unchanged, because the future
// ASCII song import will call that same engine with stanzas. The engine
// input carries no line count anywhere: how many verses fit is what the
// measurement answers for the resolved (context-aware) box and size.
//
// Qt-free on purpose: the pagination is a pure function over its input and
// the measuring port, so the ordering/no-repeats/no-loss properties are
// testable with no network, no worker and no display.
#ifndef OPENSHOW_IMPORTER_SCRIPTUREPAGINATION_H
#define OPENSHOW_IMPORTER_SCRIPTUREPAGINATION_H

#include <string>
#include <vector>

#include "openshow/importer/textflow.h"

namespace openshow::importer {

// The measuring port, the batch cap and the split helpers come from the
// engine header (MeasureRequest, MeasureReply, MeasureFn,
// kMaxMeasureChunk, splitSentences, splitWords, measureReservation) — the
// paginator and every other adapter share them, the frontend included.

// One selected verse, in the order the passage gave them.
struct PaginationVerse {
    std::string reference;  // the page's own "chapter:verse", e.g. "1:2"
    std::string text;       // plain text, whitespace already collapsed
};

// The resolved theme's primary text box and the CHOSEN typography, in
// output-canvas pixels — the engine's box-and-typography itself (see
// textflow.h for the contract: nothing is a suggestion, and fontSize 0 is
// the RENDERER's default).
using PaginationStyle = FlowStyle;

struct PaginationInput {
    std::vector<PaginationVerse> verses;  // the selection, in order
    std::string reference;   // what the operator looked up; reserves room
    std::string attribution; // the licence text; reserves room, may be empty
    PaginationStyle style;
};

// The caption the reservation is set in (BIF.1). Its size is four percent
// of the box's height — a provenance line is small print wherever it
// lives — clamped to [16, 56] canvas pixels so it stays readable in a
// small theme box and stays small in a great one, and never larger than
// the words themselves. The HEIGHT it reserves is still the worker's
// measured answer at exactly this size; no arithmetic stands in for a
// glyph run.
inline double reservationFontSize(const PaginationStyle& style) {
    double caption = style.boxHeight * 0.04;
    if (caption < 16.0) {
        caption = 16.0;
    }
    if (caption > 56.0) {
        caption = 56.0;
    }
    if (style.fontSize > 0.0 && style.fontSize < caption) {
        caption = style.fontSize;  // the reservation never out-typesets the words
    }
    return caption;
}

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

// Paginate. Blocks are the selected verses; the labeler composes
// "1:2", "1:2-1:4", "(cont.) 1:5" and "1:6 (continues)" from the block
// ids the engine reports; the reference and attribution reserve measured
// room in caption type. All packing happens in the engine — the measure
// port is called only with whole batches, at most kMaxMeasureChunk per
// call.
PaginationOutcome paginateScripture(const PaginationInput& input, const MeasureFn& measure);

}  // namespace openshow::importer

#endif  // OPENSHOW_IMPORTER_SCRIPTUREPAGINATION_H
