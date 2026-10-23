#include "media/FrameRing.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "media/FFmpeg.h"

namespace openshow::media {

namespace {
enum class SlotState : uint8_t {
    kFree,    // claimable
    kClaimed, // producer writing
    kReady,   // visible, waiting for poll()
    kOwned,   // consumer holds it
};
}  // namespace

struct FrameRing::Impl {
    struct Slot {
        // Single av_image_alloc block per frame; data[1..3] point inside it.
        uint8_t* data[4] = {};
        int linesize[4] = {};
        SlotState state = SlotState::kFree;
        double ptsSeconds = 0.0;
        uint64_t sequence = 0;
    };

    std::vector<Slot> slots;
    std::deque<uint32_t> ready;  // kReady slot indices, production order
    std::mutex mutex;
    std::condition_variable freeCv;
    std::condition_variable readyCv;
    uint64_t sequenceCounter = 0;
    bool stopped = false;
    unsigned slotCount = 0;
    int width = 0;
    int height = 0;
    FrameRing::Stats stats;
};

FrameRing::FrameRing() = default;
FrameRing::~FrameRing() { destroy(); }

bool FrameRing::create(unsigned slotCount, int width, int height, std::string* error) {
    // Re-create is allowed: drop any previous buffers first.
    destroy();
    auto fail = [&](const std::string& message) {
        if (error) {
            *error = message;
        }
        destroy();
        return false;
    };
    if (slotCount == 0) {
        return fail("FrameRing needs at least one slot");
    }
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        return fail("FrameRing needs positive even dimensions for I420");
    }

    fImpl = std::make_unique<Impl>();
    fImpl->slotCount = slotCount;
    fImpl->width = width;
    fImpl->height = height;
    fImpl->slots.resize(slotCount);
    for (unsigned i = 0; i < slotCount; ++i) {
        Impl::Slot& slot = fImpl->slots[i];
        if (av_image_alloc(slot.data, slot.linesize, width, height, AV_PIX_FMT_YUV420P, 32) < 0) {
            return fail("FrameRing could not allocate frame buffers");
        }
    }
    return true;
}

void FrameRing::destroy() {
    if (!fImpl) {
        return;
    }
    for (Impl::Slot& slot : fImpl->slots) {
        // av_image_alloc packs every plane into one block; the first
        // pointer owns it. Works from any slot state — consumers must not
        // touch frames after destroy(), the same contract as the worker's
        // shm segment disappearing.
        av_freep(&slot.data[0]);
    }
    fImpl.reset();
}

unsigned FrameRing::slotCount() const { return fImpl ? fImpl->slotCount : 0; }
int FrameRing::width() const { return fImpl ? fImpl->width : 0; }
int FrameRing::height() const { return fImpl ? fImpl->height : 0; }

bool FrameRing::claim(VideoFrame* out) {
    std::unique_lock lock(fImpl->mutex);
    fImpl->freeCv.wait(lock, [&] {
        return fImpl->stopped ||
               std::any_of(fImpl->slots.begin(), fImpl->slots.end(),
                           [](const Impl::Slot& slot) { return slot.state == SlotState::kFree; });
    });
    // stop() wins even when a slot is free: the producer must unwind.
    if (fImpl->stopped) {
        return false;
    }
    for (size_t i = 0; i < fImpl->slots.size(); ++i) {
        Impl::Slot& slot = fImpl->slots[i];
        if (slot.state != SlotState::kFree) {
            continue;
        }
        slot.state = SlotState::kClaimed;
        out->slot = static_cast<uint32_t>(i);
        out->width = fImpl->width;
        out->height = fImpl->height;
        std::copy(std::begin(slot.data), std::end(slot.data), out->data);
        std::copy(std::begin(slot.linesize), std::end(slot.linesize), out->linesize);
        out->ptsSeconds = 0.0;
        out->sequence = 0;
        return true;
    }
    return false;  // unreachable: the predicate just found a free slot
}

void FrameRing::publish(const VideoFrame& frame) {
    {
        std::lock_guard lock(fImpl->mutex);
        Impl::Slot& slot = fImpl->slots[frame.slot];
        slot.state = SlotState::kReady;
        slot.ptsSeconds = frame.ptsSeconds;
        slot.sequence = ++fImpl->sequenceCounter;
        fImpl->ready.push_back(frame.slot);
        fImpl->stats.published++;
        fImpl->stats.peakFilled =
            std::max(fImpl->stats.peakFilled, static_cast<uint64_t>(fImpl->ready.size()));
    }
    fImpl->readyCv.notify_one();
}

void FrameRing::abortClaim(uint32_t slot) {
    {
        std::lock_guard lock(fImpl->mutex);
        Impl::Slot& target = fImpl->slots[slot];
        if (target.state != SlotState::kClaimed) {
            return;
        }
        target.state = SlotState::kFree;
    }
    fImpl->freeCv.notify_one();
}

bool FrameRing::poll(VideoFrame* out, std::chrono::milliseconds timeout) {
    std::unique_lock lock(fImpl->mutex);
    fImpl->readyCv.wait_for(lock, timeout,
                            [&] { return fImpl->stopped || !fImpl->ready.empty(); });
    // Frames already visible still drain after stop(); once they are gone,
    // a stopped ring never hands anything out.
    if (fImpl->ready.empty()) {
        return false;
    }
    const uint32_t index = fImpl->ready.front();
    fImpl->ready.pop_front();
    Impl::Slot& slot = fImpl->slots[index];
    slot.state = SlotState::kOwned;
    out->slot = index;
    out->width = fImpl->width;
    out->height = fImpl->height;
    std::copy(std::begin(slot.data), std::end(slot.data), out->data);
    std::copy(std::begin(slot.linesize), std::end(slot.linesize), out->linesize);
    out->ptsSeconds = slot.ptsSeconds;
    out->sequence = slot.sequence;
    fImpl->stats.consumed++;
    return true;
}

void FrameRing::release(uint32_t slot) {
    {
        std::lock_guard lock(fImpl->mutex);
        Impl::Slot& target = fImpl->slots[slot];
        if (target.state != SlotState::kOwned) {
            return;
        }
        target.state = SlotState::kFree;
    }
    fImpl->freeCv.notify_one();
}

void FrameRing::stop() {
    {
        std::lock_guard lock(fImpl->mutex);
        fImpl->stopped = true;
    }
    fImpl->freeCv.notify_all();
    fImpl->readyCv.notify_all();
}

FrameRing::Stats FrameRing::stats() const {
    std::lock_guard lock(fImpl->mutex);
    return fImpl->stats;
}

}  // namespace openshow::media
