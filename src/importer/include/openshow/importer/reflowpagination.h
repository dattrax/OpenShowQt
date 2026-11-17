/*
 * Measured re-pagination of words a service ALREADY holds (task PT.5).
 *
 * Reflow is not an import: the words and their authored blocks come from a
 * stored `domain::ReflowSource` (or, for a legacy/edited item, from the
 * conservative current-word source), and only the BOX and typography of the
 * chosen future appearance decide where the slides break. Nothing here can
 * fetch a stanza, a verse or a provider: the input is a bounded value and
 * the measuring port.
 *
 * Two policies, because the content kinds have two:
 *
 *   - A song section never shares a slide with another section
 *     (`paginateSong`'s rule, unchanged).
 *   - Scripture blocks pack greedily, exactly as the import packs verses,
 *     with a caption reservation measured in caption type when the source
 *     carries a reference/attribution.
 *
 * Neither policy clips, drops, repeats or font-shrinks a word: the size is
 * the size and the WORDS move. `reflowWordsPreserved` is the arithmetic
 * check a caller runs before publishing a plan.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "openshow/domain/reflowsource.h"
#include "openshow/importer/textflow.h"

namespace openshow::importer {

enum class ReflowPolicy {
    // Songs: a section starts on a fresh slide and is packed inside itself.
    kSectionStartsNewSlide,
    // Scripture: blocks fill a slide greedily and only split when a block
    // alone will not fit.
    kGreedyBlocks,
};

struct ReflowPaginationInput {
    std::vector<openshow::domain::ReflowSourceBlock> blocks;
    // The future appearance: the box and typography the new slides wear.
    FlowStyle style;
    ReflowPolicy policy = ReflowPolicy::kSectionStartsNewSlide;
    // Caption reservation (scripture only): measured in caption type in the
    // same box, exactly as the import reserved it. Empty strings reserve
    // nothing.
    std::string reference;
    std::string attribution;
    // Labels a block that has none (a legacy passage's whole reference).
    std::string fallbackLabel;
};

struct ReflowSlide {
    std::vector<std::string> lines;  // this slide's ONLY text
    std::string label;               // goes in notes, never on the slide
    std::size_t blockIndex = 0;      // the authored block it starts in
    bool startsContinued = false;    // opens on a split block's tail
};

struct ReflowPaginationOutcome {
    bool ok = false;
    std::string error;  // the operator's sentence; empty when ok
    std::vector<ReflowSlide> slides;
    double reservedHeight = 0.0;
};

// Re-paginate. Pure over its input and the measuring port, so the
// no-loss/no-repeat properties are testable with a scripted measurer.
ReflowPaginationOutcome paginateReflow(const ReflowPaginationInput& input,
                                       const MeasureFn& measure);

// True when concatenating the plan's words, in order, restores the source
// words exactly (whitespace runs collapsed). This is the check every caller
// runs before publishing: a plan that lost or duplicated a word is refused,
// never shown.
bool reflowWordsPreserved(const std::vector<openshow::domain::ReflowSourceBlock>& blocks,
                          const std::vector<ReflowSlide>& slides);

}  // namespace openshow::importer
