/*
 * A portable remembered look (task PT.1).
 *
 * A Library song can remember the look the operator gave it, and that look
 * must keep meaning the same thing in a later service and on another day.
 * A service theme id cannot do that — ids belong to one service package, and
 * the theme behind an id can be edited or deleted — so the look travels as a
 * VALUE: the complete effective appearance, resolved, with no service ids,
 * no asset paths and no theme background media.
 *
 * The snapshot is not a second style vocabulary. Its fields are the domain's
 * own theme/context records (weight, slant, fit, effects, layout pair,
 * placement, transition, named contexts), captured CONCRETE: every silent
 * dependency has already been resolved against the chosen theme's normal
 * Default floor at capture time, exactly as the Theme Editor's fixture does,
 * with the unrelated service current-theme override excluded. Re-applying it
 * is therefore deterministic, and a later edit to a theme elsewhere cannot
 * change what the Library remembers.
 *
 * Background colour/gradient are deliberately absent: applying a look to an
 * item replaces text formatting and placement and preserves authored
 * backdrops (PT.3). Font family names are remembered as names; bundling
 * fonts or promising fallback faces on another machine is out of scope.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "openshow/domain/documents.h"

namespace openshow::domain {

struct ThemeSnapshot {
    // Display name of the remembered look ("Brun", "Easter", …). A
    // materialised service theme uses it; it is never an identity.
    std::string name;
    // The complete resolved text appearance.
    std::string textColor;
    std::string fontFamily;
    double fontSize = 0.0;                 // 0 = renderer default
    std::string align = "center";
    double lineSpacing = kDefaultLineSpacing;
    int fontWeight = kDefaultFontWeight;
    std::string fontStyle = "normal";
    bool fontUnderline = false;
    std::string fitMode = "shrink";
    double minFontSize = kDefaultMinFontSize;
    std::string verticalAlign = kDefaultVerticalAlign;
    bool allCaps = false;
    TextStroke stroke;
    TextShadow shadow;
    // The primary lyric box, absent when no layer placed one. The authored
    // slide rectangle survives separately; this is only the look's opinion.
    std::optional<BoxRect> placement;
    // The resolved movement answer (theme → Default → service default at
    // capture time), concrete like every other field here.
    TransitionOptions transition = TransitionOptions::defaults();
    // Named contexts with their effective values. The name is retained even
    // when this build does not know it, exactly as the theme record keeps
    // unknown names for the importers still to come.
    std::vector<TextContext> contexts;
    // Output profiles (milestone FO): the theme's authored stage/broadcast
    // overrides, copied verbatim. They are sparse by design — resolution
    // against the snapshot's concrete audience look happens on the
    // recipient's own chain, so a materialised look still gives the stage
    // its role box and inherits everything the profile is silent on.
    std::vector<OutputProfile> profiles;

    bool operator==(const ThemeSnapshot&) const = default;
};

// Bounded limits for the snapshot codec. They are the domain's existing
// budgets expressed for this record: a catalog item is small application
// data, not a document store.
inline constexpr std::size_t kMaxThemeSnapshotContexts = 64;
inline constexpr std::size_t kMaxThemeSnapshotNameChars = 256;
inline constexpr std::size_t kMaxThemeSnapshotFamilyChars = 512;

// Deterministic JSON writer for the snapshot. Key order is fixed, so two
// equal snapshots serialise byte-equal (the catalog's round-trip rule).
boost::json::object themeSnapshotToJson(const ThemeSnapshot& snapshot);

// Strict reader for the above. Fails naming the offending key or rule on:
// unknown keys, missing required members, wrong value kinds, off-list
// words, out-of-band numbers, out-of-range effects, a non-placeable box,
// duplicate context names and the snapshot budgets above. Leaves `out`
// untouched on refusal.
bool themeSnapshotFromJson(const boost::json::value& value, ThemeSnapshot* out,
                           std::string* error);

}  // namespace openshow::domain
