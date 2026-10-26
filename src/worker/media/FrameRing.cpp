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
        // Ring-owned frames are one av_image_alloc block (data[1..3] point
        // inside it); backing-owned frames are plane pointers handed over
        // once at create() and freed by the backing.
        uint8_t* data[4] = {};
        int linesize[4] = {};
        SlotState state = SlotState::kFree;
        double ptsSeconds = 0.0;
        uint64_t sequence = 0;
    };

    std::vector<Slot> slots;
    std::deque<uint32_t> ready;  // kReady slot indices, production order
    // The round-robin cursor for claim(): scanning always resumes after the
    // last claimed slot, so a consumer that frees one slot late still leaves
    // the producer's writes spread across every slot. A lowest-index-first
    // scan degenerates to the first slot in the pattern "consumer holds one
    // frame while the producer keeps cycling" (task 6.6a) and collapses the
    // decode cushion to a single slot.
    uint32_t claimCursor = 0;
    std::mutex mutex;
    std::condition_variable freeCv;
    std::condition_variable readyCv;
    uint64_t sequenceCounter = 0;
    bool stopped = false;
    unsigned slotCount = 0;
    int width = 0;
    int height = 0;
    FrameFormat format = FrameFormat::kNV12;
    FrameBacking* backing = nullptr;  // nullptr: the ring owns the pixels
    bool direct = false;
    FrameRing::Stats stats;
};

FrameRing::FrameRing() = default;
FrameRing::~FrameRing() { destroy(); }

bool FrameRing::create(unsigned slotCount, int width, int height, std::string* error) {
    return create(slotCount, width, height, FrameFormat::kNV12, nullptr, error);
}

bool FrameRing::create(unsigned slotCount, int width, int height, FrameFormat format,
                       FrameBacking* backing, std::string* error, bool direct) {
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
        return fail("FrameRing needs positive even dimensions for 4:2:0");
    }

    fImpl = std::make_unique<Impl>();
    fImpl->slotCount = slotCount;
    fImpl->width = width;
    fImpl->height = height;
    fImpl->format = format;
    fImpl->backing = backing;
    fImpl->direct = direct;
    if (direct && !backing) return fail("direct ring needs a frame backing");
    fImpl->slots.resize(slotCount);
    const AVPixelFormat pixFmt = format == FrameFormat::kYUV420P ? AV_PIX_FMT_YUV420P
        : format == FrameFormat::kP010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    for (unsigned i = 0; i < slotCount; ++i) {
        Impl::Slot& slot = fImpl->slots[i];
        if (direct) continue; // only presentation tokens; decoder owns the pixels
        if (backing) {
            if (!backing->slotPlanes(i, slot.data, slot.linesize)) {
                return fail("FrameRing slot backing refused a slot");
            }
            continue;
        }
        if (av_image_alloc(slot.data, slot.linesize, width, height, pixFmt, 32) < 0) {
            return fail("FrameRing could not allocate frame buffers");
        }
    }
    return true;
}

void FrameRing::destroy() {
    if (!fImpl) {
        return;
    }
    if (!fImpl->backing) {
        for (Impl::Slot& slot : fImpl->slots) {
            // av_image_alloc packs every plane into one block; the first
            // pointer owns it. Works from any slot state — consumers must
            // not touch frames after destroy(), the same contract as the
            // worker's shm segment disappearing.
            av_freep(&slot.data[0]);
        }
    }
    fImpl.reset();
}

unsigned FrameRing::slotCount() const { return fImpl ? fImpl->slotCount : 0; }
int FrameRing::width() const { return fImpl ? fImpl->width : 0; }
int FrameRing::height() const { return fImpl ? fImpl->height : 0; }
FrameFormat FrameRing::format() const { return fImpl ? fImpl->format : FrameFormat::kNV12; }

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
    // Round-robin from the cursor (see Impl::claimCursor): wrap the scan so
    // every slot gets its turn even when several are free at once.
    const size_t count = fImpl->slots.size();
    for (size_t step = 0; step < count; ++step) {
        const size_t i = (fImpl->claimCursor + step) % count;
        Impl::Slot& slot = fImpl->slots[i];
        if (slot.state != SlotState::kFree) {
            continue;
        }
        slot.state = SlotState::kClaimed;
        fImpl->claimCursor = static_cast<uint32_t>((i + 1) % count);
        out->slot = static_cast<uint32_t>(i);
        out->width = fImpl->width;
        out->height = fImpl->height;
        std::copy(std::begin(slot.data), std::end(slot.data), out->data);
        std::copy(std::begin(slot.linesize), std::end(slot.linesize), out->linesize);
        out->ptsSeconds = 0.0;
        out->sequence = 0;
        out->storage.reset();
        return true;
    }
    return false;  // unreachable: the predicate just found a free slot
}

bool FrameRing::publish(const VideoFrame& frame) {
    // Commit outside the lock: with a GPU backing this blocks until the
    // slot's previous frame has been released (and its copy retired),
    // which is exactly the back-pressure the producer thread should feel —
    // nothing else may hold the ring mutex while it waits.
    if (fImpl->backing && !(fImpl->direct
            ? fImpl->backing->commitFrame(frame.slot, frame.storage)
            : fImpl->backing->commit(frame.slot))) {
        abortClaim(frame.slot);
        return false;
    }
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
    return true;
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

unsigned FrameRing::discardReady() {
    unsigned dropped = 0;
    std::vector<uint32_t> purged;
    {
        std::lock_guard lock(fImpl->mutex);
        purged.reserve(fImpl->ready.size());
        for (const uint32_t index : fImpl->ready) {
            fImpl->slots[index].state = SlotState::kFree;
            purged.push_back(index);
            ++dropped;
        }
        fImpl->ready.clear();
        fImpl->stats.discarded += dropped;
    }
    // A purged frame was already committed to the backing and will never
    // reach a consumer, so nothing else hands its slot back (task 6.7d).
    // Without this the GPU-side slot stays held for the life of the
    // transport — and since the ring is exactly as deep as the backing is
    // wide, the frames a seek flushes would in four steps park the producer
    // inside publish(). Released before the wake below, which may hand a
    // producer one of these very slots.
    if (fImpl->backing) {
        for (const uint32_t index : purged) {
            fImpl->backing->discardCommitted(index);
        }
    }
    if (dropped != 0) {
        fImpl->freeCv.notify_all();  // a parked producer can fill again
    }
    return dropped;
}

void FrameRing::stop() {
    if (!fImpl) return;
    {
        std::lock_guard lock(fImpl->mutex);
        fImpl->stopped = true;
    }
    fImpl->freeCv.notify_all();
    fImpl->readyCv.notify_all();
}

bool FrameRing::stopped() const {
    if (!fImpl) {
        return true;  // a ring without storage can never take a frame anyway
    }
    std::lock_guard lock(fImpl->mutex);
    return fImpl->stopped;
}

FrameRing::Stats FrameRing::stats() const {
    if (!fImpl) return {};
    std::lock_guard lock(fImpl->mutex);
    return fImpl->stats;
}

}  // namespace openshow::media
