// Measured scripture pagination — the TF.2 adapter. Verses become blocks,
// the reference/attribution reservation is measured in caption type, the
// labels are composed from the block ids the engine reports, and the
// packing itself is one call to the generic engine (see textflow.h). The
// sentences this file still owns are the scripture ones the tests and the
// page quote: the empty selection, and the reservation eating the box.
#include "openshow/importer/scripturepagination.h"

#include <utility>

namespace openshow::importer {

namespace {

// The verses' labeler: a range of references, with a "(cont.)" mark when
// the slide opens mid-verse and a "(continues)" mark when the slide's last
// verse is not finished. Both marks are the reader's guarantee that the
// verse boundaries on the screen are the passage's own.
std::string verseLabel(const std::vector<std::string>& ids, bool startsContinued,
                       bool continues) {
    if (ids.empty()) {
        return {};
    }
    std::string label = startsContinued ? "(cont.) " : "";
    const std::string& first = ids.front();
    const std::string& last = ids.back();
    label += first == last ? first : first + "-" + last;
    if (continues) {
        label += " (continues)";
    }
    return label;
}

}  // namespace

PaginationOutcome paginateScripture(const PaginationInput& input, const MeasureFn& measure) {
    PaginationOutcome outcome;
    // The engine's own gate first (the order BI.4's tests quote: an
    // unmeasurable box is refused before anything else is asked), then the
    // selection, then the reservation.
    if (!validateFlowStyle(input.style, &outcome.error)) {
        return outcome;
    }
    FlowInput flow;
    flow.subject = "the passage";
    flow.style = input.style;
    flow.labeler = verseLabel;
    for (const PaginationVerse& verse : input.verses) {
        if (verse.text.empty()) {
            continue;  // a verse with no words carries nothing to place
        }
        flow.blocks.push_back(FlowBlock{verse.reference, {verse.text}});
    }
    if (flow.blocks.empty()) {
        outcome.error = "no verses are selected to add";
        return outcome;
    }

    // The reservation: reference and attribution, each measured in this
    // very box in CAPTION type (BIF.1: setting provenance small print in
    // the words' display face let a live licence paragraph reserve most of
    // the box it never shares with the words on screen). The height is
    // still the worker's measured answer at exactly the caption size —
    // arithmetic never stands in for a glyph run. (An empty string
    // reserves nothing.)
    std::vector<std::vector<std::string>> reserveBlocks;
    if (!input.reference.empty()) {
        reserveBlocks.push_back({input.reference});
    }
    if (!input.attribution.empty()) {
        reserveBlocks.push_back({input.attribution});
    }
    double reserved = 0.0;
    if (!reserveBlocks.empty()) {
        std::string measureError;
        if (!measureReservation(reserveBlocks, input.style, reservationFontSize(input.style),
                                measure, &reserved, &measureError)) {
            outcome.error = "the passage could not be measured: " + measureError;
            return outcome;
        }
    }
    outcome.reservedHeight = reserved;
    flow.reservedHeight = reserved;

    const FlowOutcome packed = runTextFlow(flow, measure);
    outcome.availableHeight = packed.availableHeight;
    if (!packed.ok) {
        // The engine's sentences are already the operator's; the one it
        // speaks about a reservation that ate the box gets the scripture
        // telling, naming the reference and attribution by what they are.
        outcome.error = packed.error;
        if (packed.error.find("what stands above the words") != std::string::npos) {
            outcome.error =
                "even set as small print, the reference and attribution leave no room for the "
                "passage in this theme's text area — choose a larger box or a smaller text size";
        }
        return outcome;
    }
    for (const FlowSlide& slide : packed.slides) {
        GeneratedSlide generated;
        generated.lines = slide.lines;
        generated.label = slide.label;
        outcome.slides.push_back(std::move(generated));
    }
    outcome.ok = true;
    return outcome;
}

}  // namespace openshow::importer
