/*
 * Display inventory (stage delivery, tasks FO.5a.1/FO.5a.2).
 *
 * GLFW's monitor INDEX is an enumeration position, not an identity: a hotplug
 * reorders it, and two panels can carry the same model name. The stage's
 * destination is therefore remembered as a CONNECTION-SCOPED identity — the
 * name plus the desktop position and the current mode, with an ordinal only
 * when two attached panels are indistinguishable — and every accepted
 * configuration names the inventory GENERATION it was validated against.
 *
 * Indices stay on the wire for compatibility with the audience's existing
 * `output.select`; the identity is what the stage validates before it maps a
 * window. A saved index is a preference, never a proof.
 *
 * Pure functions: no GLFW, no Vulkan, no session state. OutputSession builds
 * the rows, RpcMethods encodes them, and the policy tests exercise the same
 * rules the worker applies.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openshow::worker {

struct DisplayInfo {
    int index = 0;  // the value output.select takes; -1 (windowed) is not listed
    std::string name;
    int width = 0;
    int height = 0;
    int refreshHz = 0;
    int x = 0;  // desktop position, from glfwGetMonitorPos
    int y = 0;
    // Connection-scoped identity: name | x,y | WxH | #ordinal. Stable across
    // re-enumeration of the same attached set; changes when the panel moves
    // or is replaced. Never a Vulkan/GLFW numeric index.
    std::string identity;
    // The inventory generation in which this identity first appeared. A
    // consumer can tell "the same panel, still attached" from "a new panel
    // reusing this name and position" by comparing both fields.
    std::uint64_t connectionGeneration = 0;
};

// Builds the identity rows for a snapshot. `ordinal` disambiguates identical
// (name, position, mode) triples by their order in the enumeration; the first
// occurrence keeps ordinal 0 and therefore reads without a suffix.
std::string displayIdentity(const std::string& name, int x, int y, int width, int height,
                            int ordinal = 0);

// Fills `identity` and `connectionGeneration` on each row. `generation` is the
// generation the caller is about to publish for this snapshot; an identity
// already present in `previous` keeps ITS first-appearance generation, so a
// re-enumeration of the same attached set is not a new connection.
void stampDisplayIdentities(std::vector<DisplayInfo>* displays,
                            const std::vector<DisplayInfo>& previous, std::uint64_t generation);

// True when the two snapshots describe the same physical connected set, in
// the same order. A re-enumeration that returns identical rows does not bump
// the inventory generation; any add/remove/reorder/mode change does.
bool sameDisplayInventory(const std::vector<DisplayInfo>& a,
                          const std::vector<DisplayInfo>& b);

// Index of the row carrying `identity`, or -1. Duplicate identities cannot be
// stored because stampDisplayIdentities gives them ordinals.
int displayIndexForIdentity(const std::vector<DisplayInfo>& displays,
                            const std::string& identity);

// True when `identity` names a row in `displays`. Empty is never present: a
// stage that was not chosen cannot validate.
bool displayIdentityPresent(const std::vector<DisplayInfo>& displays,
                            const std::string& identity);

}  // namespace openshow::worker