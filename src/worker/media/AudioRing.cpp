#include "media/AudioRing.h"

#include <algorithm>
#include <cstring>

namespace openshow::media {

bool AudioRing::create(size_t capacityFrames) {
    if (capacityFrames == 0) {
        return false;
    }
    fSamples.assign(capacityFrames * 2, 0.0f);
    fCapacity = capacityFrames;
    fHead.store(0);
    fTail.store(0);
    return true;
}

void AudioRing::reset() {
    fHead.store(0);
    fTail.store(0);
}

size_t AudioRing::available() const {
    return fHead.load(std::memory_order_acquire) - fTail.load(std::memory_order_acquire);
}

size_t AudioRing::free() const {
    return fCapacity - (fHead.load(std::memory_order_relaxed) -
                        fTail.load(std::memory_order_acquire));
}

size_t AudioRing::write(const float* interleavedStereo, size_t frames) {
    if (!fCapacity) {
        return 0;
    }
    const size_t head = fHead.load(std::memory_order_relaxed);
    const size_t used = head - fTail.load(std::memory_order_acquire);
    const size_t n = std::min(frames, fCapacity - used);
    if (n == 0) {
        return 0;
    }
    const size_t start = head % fCapacity;
    const size_t first = std::min(n, fCapacity - start);
    std::memcpy(&fSamples[start * 2], interleavedStereo, first * 2 * sizeof(float));
    if (n > first) {
        std::memcpy(fSamples.data(), interleavedStereo + first * 2,
                    (n - first) * 2 * sizeof(float));
    }
    fHead.store(head + n, std::memory_order_release);
    return n;
}

size_t AudioRing::read(float* interleavedStereo, size_t frames) {
    if (!fCapacity) {
        return 0;
    }
    const size_t tail = fTail.load(std::memory_order_relaxed);
    const size_t available = fHead.load(std::memory_order_acquire) - tail;
    const size_t n = std::min(frames, available);
    if (n == 0) {
        return 0;
    }
    const size_t start = tail % fCapacity;
    const size_t first = std::min(n, fCapacity - start);
    std::memcpy(interleavedStereo, &fSamples[start * 2], first * 2 * sizeof(float));
    if (n > first) {
        std::memcpy(interleavedStereo + first * 2, fSamples.data(),
                    (n - first) * 2 * sizeof(float));
    }
    fTail.store(tail + n, std::memory_order_release);
    return n;
}

}  // namespace openshow::media
