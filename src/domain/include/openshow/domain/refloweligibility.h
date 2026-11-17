/*
 * Whether a whole item can be reflowed at all (task PT.5).
 *
 * Reflow is deliberately bounded. It may replace a run of simple
 * text slides, but it may never flatten, duplicate or discard authored
 * media to make an item qualify. This header is the ONE answer to "may this
 * item be offered Reflow, and why not?" — the dialog, the plan and the
 * commit all quote it, so they cannot disagree about an item that carries a
 * second text box or a picture.
 *
 * The conservative current-word source (PT.4) is not ineligibility: a
 * legacy or edited item is offered Reflow with its current words. Only the
 * STRUCTURE of the item — extra boxes, foreground pictures, wordless cues
 * inside the words, differing media — makes Reflow unavailable; Apply only
 * remains available for every item.
 */

#pragma once

#include <cstddef>
#include <string>

#include "openshow/domain/documents.h"
#include "openshow/domain/reflowsource.h"

namespace openshow::domain {

struct ReflowEligibility {
    bool eligible = false;
    // The operator's sentence when not eligible; empty when it is.
    std::string reason;
    // The source the plan would read. `usesStoredSource` is false for a
    // legacy/edited item, where the conservative current-word source is
    // used and the UI should say so honestly.
    ReflowSource source;
    bool usesStoredSource = false;
    // Songs force a fresh slide per section; scripture packs greedily.
    bool sectionStartsNewSlide = true;
    // The optional leading cue that must survive the reflow untouched.
    bool hasLeadingBlankCue = false;
    // The common media identity across the text run ("" = no background).
    std::string backgroundAssetId;
    BackgroundFit backgroundFit = BackgroundFit::Cover;
    // The text context every content slide names ("" = none).
    std::string textContext;
    // How many slides currently carry words (the count the plan replaces).
    std::size_t contentSlideCount = 0;
};

// The single eligibility sentence. Unknown show, a picture-only deck, a
// second text box, a foreground picture on a word slide, an internal/
// trailing wordless cue, more than one leading blank, or differing
// background/fit/context across the words each refuse with a specific
// reason.
ReflowEligibility reflowEligibility(const Documents& documents,
                                    const std::string& showId);

}  // namespace openshow::domain
