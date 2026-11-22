#include "DisplayInventory.h"

#include <algorithm>
#include <unordered_map>

namespace openshow::worker {

std::string displayIdentity(const std::string& name, int x, int y, int width, int height,
                            int ordinal) {
    std::string identity = name;
    identity += '|';
    identity += std::to_string(x);
    identity += ',';
    identity += std::to_string(y);
    identity += '|';
    identity += std::to_string(width);
    identity += 'x';
    identity += std::to_string(height);
    if (ordinal > 0) {
        identity += '#';
        identity += std::to_string(ordinal);
    }
    return identity;
}

void stampDisplayIdentities(std::vector<DisplayInfo>* displays,
                            const std::vector<DisplayInfo>& previous, std::uint64_t generation) {
    if (displays == nullptr) {
        return;
    }
    // First-appearance generations by identity, so a panel attached before
    // this snapshot keeps the generation it was first seen in. The map is
    // rebuilt from `previous` each time rather than cached: the inventory is
    // a handful of rows, and a stale cache is how a replacement panel would
    // inherit its predecessor's identity.
    std::unordered_map<std::string, std::uint64_t> firstSeen;
    for (const DisplayInfo& row : previous) {
        if (!row.identity.empty() && firstSeen.find(row.identity) == firstSeen.end()) {
            firstSeen.emplace(row.identity, row.connectionGeneration);
        }
    }
    std::unordered_map<std::string, int> ordinals;
    for (DisplayInfo& row : *displays) {
        const std::string base =
            displayIdentity(row.name, row.x, row.y, row.width, row.height, 0);
        const int ordinal = ordinals[base]++;
        row.identity =
            displayIdentity(row.name, row.x, row.y, row.width, row.height, ordinal);
        const auto found = firstSeen.find(row.identity);
        row.connectionGeneration =
            found != firstSeen.end() ? found->second : generation;
        // Keep the identity map truthful within this snapshot too: if two
        // identical rows somehow carry the same identity (ordinals ensure
        // they cannot), the later one would otherwise inherit the earlier's
        // generation unnoticed.
        firstSeen[row.identity] = row.connectionGeneration;
    }
}

bool sameDisplayInventory(const std::vector<DisplayInfo>& a,
                          const std::vector<DisplayInfo>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        // Compare the PHYSICAL description, not the derived identity: the
        // caller stamps identities after this question, so comparing the
        // stamped string with an unstamped snapshot would report a change on
        // every refresh and invalidate a healthy stage selection.
        if (a[i].name != b[i].name || a[i].x != b[i].x || a[i].y != b[i].y ||
            a[i].width != b[i].width || a[i].height != b[i].height ||
            a[i].refreshHz != b[i].refreshHz) {
            return false;
        }
    }
    return true;
}

int displayIndexForIdentity(const std::vector<DisplayInfo>& displays,
                            const std::string& identity) {
    if (identity.empty()) {
        return -1;
    }
    for (const DisplayInfo& row : displays) {
        if (row.identity == identity) {
            return row.index;
        }
    }
    return -1;
}

bool displayIdentityPresent(const std::vector<DisplayInfo>& displays,
                            const std::string& identity) {
    return displayIndexForIdentity(displays, identity) >= 0;
}

}  // namespace openshow::worker