/*
 * Stanza-aware song pagination (task SI.2).
 *
 * The question — "how many slides does this stanza need in THIS service's
 * Songs theme?" — is answered by the worker's own text engine, through the
 * same MeasureFn port and the same openshow::importer::textflow packing the
 * Bible importer uses. This file adds the one rule songs need that verses
 * do not:
 *
 *   A SECTION NEVER SHARES A SLIDE WITH ANOTHER SECTION.
 *
 * Each stanza starts on a fresh slide and is packed (and, only if it
 * overflows, split) entirely within itself. If a verse's last slide has
 * room for two more lines, the chorus still begins on the next slide. The
 * Bible's greedy whole-block packing is untouched: songs are the adapter
 * that enforces the boundary, not a change to the engine.
 *
 * Words are never clipped, dropped, repeated or font-shrunk. A stanza is
 * first tried whole; when it does not fit, whole authored LINES are packed
 * greedily, and only a single line taller than the box is handed to the
 * engine's sentence-then-word split. The dead end — one word that cannot
 * fit at the chosen size — blocks with the engine's own sentence.
 *
 * The input carries no line count: the resolved theme's box and typography
 * decide. The planner is Qt-free and pure over its input/port, so its
 * properties are testable with a scripted measurer.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "openshow/catalog/catalog.h"
#include "openshow/importer/textflow.h"

namespace openshow::importer {

// One generated slide. `lines` is the slide's ONLY text and holds only the
// stanza's own words, in order. `label` is the section label plus a
// "(cont.)" mark on a continuation; it belongs in the slide's NOTES, never
// on the words. `stanzaIndex` names which stanza produced the slide so the
// UI can group cards and the service bridge can set groups.
struct SongSlide {
    std::vector<std::string> lines;
    std::string label;
    std::size_t stanzaIndex = 0;
    bool startsContinued = false;
};

struct SongPaginationInput {
    // Canonical stanzas from the catalog (or a parsed draft), authored
    // order. Empty stanzas are skipped, never turned into blank slides.
    std::vector<openshow::catalog::Stanza> stanzas;
    // The current service's resolved Songs view: box, size (0 = renderer
    // default), family, spacing, weight/style/fit and effects.
    FlowStyle style;
};

struct SongPaginationOutcome {
    bool ok = false;
    std::string error;  // the operator's sentence; empty when ok
    std::vector<SongSlide> slides;
};

// Paginate. Each stanza is measured whole first, then packed by authored
// line, then (only for one over-long line) handed to runTextFlow. The
// measure port is called at most once per distinct measured block per run.
SongPaginationOutcome paginateSong(const SongPaginationInput& input,
                                   const MeasureFn& measure);

// A complete fingerprint of everything a plan depends on: the stanza text
// and labels plus the full box/typography/effect record. Equal fingerprints
// mean an earlier plan is still valid; any change means it is stale and the
// session must re-measure rather than reuse a hopeful picture. Deliberately
// a readable string (small songs) rather than a hash, so a diagnostic can
// say what changed.
std::string songPlanFingerprint(const SongPaginationInput& input);

}  // namespace openshow::importer
