/*
 * Stage layout (stage delivery, tasks FO.5b.1/FO.5b.2).
 *
 * Stage v1 puts the CURRENT cue in the stage profile's own box (the ordinary
 * `compileSlideOn(..., Stage, ...)` answer) and the ACKNOWLEDGED NEXT cue in a
 * separate slot below it. The next cue is not authored content of its own: it
 * is the next cue's resolved Stage appearance, scaled to 55% and moved into
 * the slot. These are v1 layout defaults, not persisted theme fields, so no
 * service format or scene format change is involved.
 *
 * The worker draws the next cue as an overlay: a second scene whose items are
 * already in the next slot, drawn without a background and without
 * advancing the audience's crossfade. This header is the one spelling of
 * those numbers so the compiler, the stage compositor and the tests cannot
 * disagree about where "next" lives.
 *
 * Nothing here reads a document, a file or a clock: it transforms an
 * already-compiled Scene. The frontend compiles the next cue; the worker
 * draws the result.
 */

#pragma once

#include "openshow/scene/scene.h"

namespace openshow::scene {

// The v1 next-cue slot on the 1920x1080 authored canvas, below the stage's
// default current box (160,120,1600,560).
inline constexpr double kStageNextBoxX = 160.0;
inline constexpr double kStageNextBoxY = 740.0;
inline constexpr double kStageNextBoxWidth = 1600.0;
inline constexpr double kStageNextBoxHeight = 260.0;
// The next cue's starting size relative to its own resolved Stage style.
// The worker's ordinary fit/shrink rules still apply, with a minimum scaled
// by the same factor.
inline constexpr double kStageNextFontScale = 0.55;

// The stage's next-cue overlay: the PRIMARY text item of `nextCue` (the first
// non-image item with words), moved to the next slot and scaled to 55% of its
// resolved size. Pictures, audio and the background are deliberately not
// carried: the next region is a words preview, and an image-only cue has no
// next text rather than invented words. Returns a cleared scene when the next
// cue has no text.
Scene stageNextOverlay(const Scene& nextCue);

// True when any CURRENT text item intersects the next-cue slot. The stage
// reports this rather than silently drawing one over the other; v1 does not
// move the operator's authored box.
bool stageNextSlotOverlaps(const Scene& current);

}  // namespace openshow::scene