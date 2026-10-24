#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace openshow::media {

enum class FrameFormat { kNV12, kP010, kYUV420P };

// A decoder allocation, independent of presentation slot indices. The owner
// keeps its mapping alive; FFmpeg and the GPU copy each retain a reference.
struct FrameStorage {
    virtual ~FrameStorage() = default;
    FrameFormat format = FrameFormat::kNV12;
    int width = 0;   // allocated (padded) dimensions
    int height = 0;
    uint8_t* base = nullptr;
    size_t size = 0;
    uint8_t* data[4] = {};
    int linesize[4] = {};
};

struct FrameAllocation {
    FrameFormat format = FrameFormat::kNV12;
    int width = 0;
    int height = 0;
    // avcodec_align_dimensions2 writes AV_NUM_DATA_POINTERS entries (8),
    // even though the supported formats use at most three planes.
    int alignment[8] = {};
};

}  // namespace openshow::media
