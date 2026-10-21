/*
 * Checks the shared protocol offer round-trips through Boost.JSON, which is
 * the library both the frontend and the Qt-free worker encode with.
 */

#include <boost/json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

#include "openshow/protocol/protocol.h"

namespace {

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
    return condition;
}

// Boost.JSON keeps small integers in its signed 64-bit kind, so a wire number
// can arrive as either kind depending on how it was produced.
std::uint64_t asInteger(const boost::json::value& value) {
    if (value.is_uint64()) {
        return value.as_uint64();
    }
    return static_cast<std::uint64_t>(value.as_int64());
}

}  // namespace

int main() {
    const std::string payload = openshow::protocol::offerPayload();

    boost::json::value parsed;
    try {
        parsed = boost::json::parse(payload);
    } catch (const boost::system::system_error& error) {
        std::fprintf(stderr, "FAIL: offer is not valid JSON: %s (%s)\n",
                     error.what(), payload.c_str());
        return 1;
    }

    bool ok = check(parsed.is_object(), "offer is a JSON object");
    if (!ok) {
        return 1;
    }

    const boost::json::object& offer = parsed.as_object();
    ok = check(offer.contains("application"), "offer has an application member") && ok;
    ok = check(offer.contains("protocol"), "offer has a protocol member") && ok;
    if (!ok) {
        return 1;
    }

    ok = check(offer.at("application").is_string() &&
                   offer.at("application").as_string() == "openshow-worker",
               "application names the worker") &&
         ok;

    const boost::json::array& versions = offer.at("protocol").as_array();
    ok = check(versions.size() == 2, "protocol holds a major and minor pair") && ok;
    if (versions.size() == 2) {
        ok = check(asInteger(versions[0]) == openshow::protocol::kProtocolMajor,
                   "protocol major matches kProtocolMajor") && ok;
        ok = check(asInteger(versions[1]) == openshow::protocol::kProtocolMinor,
                   "protocol minor matches kProtocolMinor") && ok;
    }

    if (!ok) {
        return 1;
    }
    std::printf("ok: offer \"%s\"\n", payload.c_str());
    return 0;
}
