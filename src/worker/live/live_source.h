/*
 * Live source values shared by the worker's live-source backends and the
 * background transport that consumes them (ND.3). A frame is plain BGRA (or
 * RGBA/UYVY) pixels plus the metadata the helper published; the backend copies
 * the newest frame out of shared memory, so these values own their bytes and
 * never point into a mapping.
 *
 * Newest wins: a consumer passes the last sequence it drew and gets either that
 * newer frame, NoFrame while nothing new has arrived, or Error. There is no
 * queue and no per-frame socket traffic.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "openshow/live/frame_ring.h"

namespace openshow::live {

struct SourceInfo {
    std::string name;
    std::string url;
};

struct OpenInfo {
    std::string shmName;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = 0;
    std::uint64_t bytes = 0;
    double fps = 0.0;
};

struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = 0;
    std::uint32_t flags = 0;
    std::uint64_t sequence = 0;
    std::uint64_t timestampNs = 0;

    // Reuses the caller's buffer unless the frame size changed.
    bool ensure(std::size_t bytes) {
        if (pixels.size() != bytes) {
            pixels.resize(bytes);
        }
        return !pixels.empty();
    }

    bool alpha() const { return (flags & kFrameFlagAlpha) != 0; }
    bool sourceLost() const { return (flags & kFrameFlagSourceLost) != 0; }
};

enum class FrameStatus {
    Frame,   // a newer frame was copied into the caller's Frame
    NoFrame, // nothing new (or nothing published yet)
    Error,   // the mapping/header is unusable; `error` says why
};

// An asynchronous helper event. `kind` is "source-lost" or "receiver-error";
// ND.8 turns these into the typed fault the operator retries.
struct Event {
    std::string kind;
    std::string source;
    std::string message;
};

}  // namespace openshow::live
