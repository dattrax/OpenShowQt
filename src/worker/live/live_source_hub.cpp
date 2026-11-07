#include "live/live_source_hub.h"

#include <cmath>

namespace openshow::live {

namespace {

// Tear down the backend without touching the uploader. Used by a start thread
// that found a cancel after opening a receiver nobody will read.
void closeBackend(std::unique_ptr<HelperBackend>* backend) {
    if (*backend) {
        (*backend)->close(nullptr);
        (*backend)->stop();
        backend->reset();
    }
}

}  // namespace

LiveSourceHub::~LiveSourceHub() {
    fCancelStart.store(true, std::memory_order_release);
    if (fStartThread.joinable()) {
        if (fStartThread.get_id() == std::this_thread::get_id()) {
            // The thread is releasing the last reference to this hub. It is
            // finishing right now, so detach and let the lambda's own teardown
            // run after this destructor returns.
            fStartThread.detach();
            return;
        }
        fStartThread.join();
    }
    teardown();
}

void LiveSourceHub::teardown() {
    fActive.store(false, std::memory_order_release);
    closeBackend(&fBackend);
    fUploader.releaseAll();
    fUploader.destroy();
    fFrame = Frame{};
    fNv12 = Nv12Frame{};
    fInfo = OpenInfo{};
    fSource.clear();
    fLastSequence = 0;
    fDrawnSlot = -1;
    fPendingSlot = -1;
}

bool LiveSourceHub::startImpl(const std::string& source, const Gpu& gpu,
                              const HelperOptions& options, std::string* error) {
    fSource = source;
    fBackend = std::make_unique<HelperBackend>(options);
    if (!fBackend->start(error)) {
        closeBackend(&fBackend);
        fSource.clear();
        return false;
    }
    if (fCancelStart.load(std::memory_order_acquire)) {
        closeBackend(&fBackend);
        fSource.clear();
        return false;
    }
    // The blocking step: the helper answers `open` only after the first frame
    // arrives, which is why this runs on the hub's own thread.
    if (!fBackend->open(source, &fInfo, error)) {
        closeBackend(&fBackend);
        fSource.clear();
        return false;
    }
    if (fInfo.format != static_cast<std::uint32_t>(FrameFormat::UYVY)) {
        if (error) {
            *error = "camera source delivered " +
                     std::string(frameFormatName(fInfo.format)) +
                     ", but the live layer expects UYVY";
        }
        closeBackend(&fBackend);
        fSource.clear();
        return false;
    }
    if (fCancelStart.load(std::memory_order_acquire)) {
        closeBackend(&fBackend);
        fSource.clear();
        return false;
    }
    const SkYUVColorSpace space =
        fInfo.height >= 720 ? kRec709_Limited_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
    // create() only allocates images, memory and fences — it never submits to
    // the upload queue — so it is safe on this thread even while the render
    // thread keeps uploading through the same uploader-adjacent queue.
    if (!fUploader.create(gpu.device, gpu.physicalDevice, gpu.graphicsQueueIndex,
                          gpu.uploadQueueIndex, gpu.uploadQueue, gpu.context,
                          static_cast<int>(fInfo.width), static_cast<int>(fInfo.height),
                          openshow::media::FrameFormat::kNV12, space, error)) {
        closeBackend(&fBackend);
        fSource.clear();
        return false;
    }
    fActive.store(true, std::memory_order_release);
    return true;
}

bool LiveSourceHub::beginStart(const std::string& source, const Gpu& gpu,
                               HelperOptions options) {
    if (fStartState.load(std::memory_order_acquire) == StartState::kStarting) {
        return false;
    }
    stop();  // retire anything already running; a hub is single-use anyway
    fCancelStart.store(false, std::memory_order_release);
    fStartState.store(StartState::kStarting, std::memory_order_release);
    auto self = shared_from_this();
    fStartThread = std::thread(
        [self, source, gpu, options = std::move(options)]() mutable {
            std::string error;
            const bool ok = self->startImpl(source, gpu, options, &error);
            {
                std::lock_guard<std::mutex> lock(self->fStartMutex);
                self->fStartError = ok ? std::string() : error;
            }
            self->fStartState.store(ok ? StartState::kReady : StartState::kFailed,
                                    std::memory_order_release);
        });
    return true;
}

LiveSourceHub::StartState LiveSourceHub::pollStart(std::string* error) {
    const StartState state = fStartState.load(std::memory_order_acquire);
    if (state == StartState::kFailed) {
        std::lock_guard<std::mutex> lock(fStartMutex);
        if (error) {
            *error = fStartError;
        }
        fStartError.clear();
        fStartState.store(StartState::kIdle, std::memory_order_release);
        return StartState::kFailed;
    }
    return state;
}

bool LiveSourceHub::start(const std::string& source, const Gpu& gpu, std::string* error,
                          HelperOptions options) {
    stop();
    // stop() raised the cancel flag for whatever was running before; this new
    // start is the caller's intent, so clear it before startImpl checks it.
    fCancelStart.store(false, std::memory_order_release);
    return startImpl(source, gpu, options, error);
}

void LiveSourceHub::cancel() {
    fCancelStart.store(true, std::memory_order_release);
    // A hub with no start thread owns nothing in flight: tear down now. One
    // with a thread finishes in its own time (the thread owns a reference, so
    // this object stays alive until it does) and tears itself down then.
    if (!fStartThread.joinable()) {
        teardown();
    }
}

void LiveSourceHub::stop() {
    fCancelStart.store(true, std::memory_order_release);
    if (fStartThread.joinable()) {
        if (fStartThread.get_id() == std::this_thread::get_id()) {
            // Called from the start thread's own lambda; nothing to join.
            fStartThread.detach();
        } else {
            // Blocking: shutdown may wait for the helper's open timeout, and
            // that is correct here — the Vulkan device this uploader lives on
            // is about to be destroyed.
            fStartThread.join();
        }
    }
    teardown();
}

int LiveSourceHub::pump(std::string* error) {
    if (!fActive.load(std::memory_order_acquire)) {
        return -1;
    }
    Event event;
    if (fBackend->takeEvent(&event)) {
        // A helper that exited closed its ring with it: there is no frame to
        // read and no receiver to revive. Treat it as the fatal case the
        // source-lost path already is, with its own reason in the message.
        if (event.kind == "source-lost" || event.kind == "receiver-error" ||
            event.kind == "helper-exit") {
            fActive.store(false, std::memory_order_release);
            if (error) {
                *error = event.message.empty()
                             ? std::string("camera ") + event.kind
                             : event.message;
            }
            return -1;
        }
    }
    const FrameStatus status = fBackend->readFrame(fLastSequence, &fFrame, error);
    if (status == FrameStatus::Error) {
        fActive.store(false, std::memory_order_release);
        return -1;
    }
    if (status == FrameStatus::Frame) {
        if (!convertUyvyToNv12(fFrame, &fNv12, error)) {
            fActive.store(false, std::memory_order_release);
            return -1;
        }
        const double pts =
            static_cast<double>(fFrame.timestampNs) / 1000000000.0;
        unsigned slot = 0;
        if (fUploader.upload(fNv12.y.data(), fNv12.yRowBytes(), fNv12.uv.data(),
                             fNv12.uvRowBytes(), fFrame.sequence, pts, &slot)) {
            // The previously drawn frame has had its sampling registered by
            // presented(); the next commit waits on those semaphores, so the
            // lease may end here and the slot become available again.
            if (fDrawnSlot >= 0) {
                fUploader.release(static_cast<unsigned>(fDrawnSlot));
                fDrawnSlot = -1;
            }
            fPendingSlot = static_cast<int>(slot);
            fLastSequence = fFrame.sequence;
        }
    }
    return fPendingSlot >= 0 ? fPendingSlot : fDrawnSlot;
}

SkImage* LiveSourceHub::image(int slot, std::string* error) {
    if (slot < 0) {
        return nullptr;
    }
    return fUploader.imageForSlot(static_cast<unsigned>(slot), error).get();
}

void LiveSourceHub::presented(int slot, VkSemaphore done) {
    if (slot < 0) {
        return;
    }
    if (done != VK_NULL_HANDLE) {
        fUploader.addSamplingDone(static_cast<unsigned>(slot), done);
    }
    fDrawnSlot = slot;
    if (fPendingSlot == slot) {
        fPendingSlot = -1;
    }
}

}  // namespace openshow::live
