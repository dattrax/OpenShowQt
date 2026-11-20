#include "openshow/domain/jsoncanonical.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "openshow/domain/thumbnails.h"

namespace openshow::domain {

namespace {

void append(const boost::json::value& value, std::string* out) {
    if (value.is_object()) {
        std::vector<std::pair<std::string, const boost::json::value*>> members;
        members.reserve(value.as_object().size());
        for (const auto& member : value.as_object()) {
            members.emplace_back(std::string(member.key()), &member.value());
        }
        std::sort(members.begin(), members.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        out->push_back('{');
        bool first = true;
        for (const auto& member : members) {
            if (!first) {
                out->push_back(',');
            }
            first = false;
            out->append(boost::json::serialize(boost::json::value(member.first)));
            out->push_back(':');
            append(*member.second, out);
        }
        out->push_back('}');
        return;
    }
    if (value.is_array()) {
        out->push_back('[');
        bool first = true;
        for (const boost::json::value& element : value.as_array()) {
            if (!first) {
                out->push_back(',');
            }
            first = false;
            append(element, out);
        }
        out->push_back(']');
        return;
    }
    out->append(boost::json::serialize(value));
}

}  // namespace

std::string canonicalJsonText(const boost::json::value& value) {
    std::string out;
    append(value, &out);
    return out;
}

std::string canonicalJsonDigest(const boost::json::value& value) {
    return sha256Hex(canonicalJsonText(value));
}

}  // namespace openshow::domain
