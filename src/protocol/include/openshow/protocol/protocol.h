/*
 * Wire types shared by the Qt frontend and the Qt-free worker.
 *
 * Task 0.4 establishes Boost.JSON as the single JSON library for these
 * payloads. Framing, the handshake and the JSON-RPC envelope are task 1.2.
 */

#pragma once

#include <cstdint>
#include <string>

namespace openshow::protocol {

// Wire format version. Frontend and worker must agree on the major value
// before any other message is exchanged.
inline constexpr std::uint32_t kProtocolMajor = 0;
inline constexpr std::uint32_t kProtocolMinor = 1;

// The offer the worker would send during a handshake, for example
// {"application":"openshow-worker","protocol":[0,1]}.
std::string offerPayload();

}  // namespace openshow::protocol
