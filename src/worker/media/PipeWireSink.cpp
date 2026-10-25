#include "media/PipeWireSink.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

// Unlike FFmpeg, the PipeWire and SPA headers carry their own extern "C"
// guards, so they are included bare.
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>

namespace openshow::media {

namespace {

// Callback-arrival histogram: 2ms buckets up to one second. With a full
// AudioRing the server pulls one slice per slice-consumption, so the honest
// cadence here is slice-size/rate (hundreds of ms), not the graph quantum;
// anything slower lands in the last bucket, keeping p99 a lower bound under
// pathological scheduling.
constexpr uint64_t kBucketMicros = 2000;
constexpr size_t kHistogramBuckets = 500;

int64_t monotonicNs() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

std::string pwErrorText(int error) {
    // PipeWire errors are negative errno.
    return std::string(std::strerror(-error));
}

}  // namespace

struct PipeWireSink::Impl {
    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    struct spa_hook streamListener = {};
    std::thread loopThread;

    AudioRing ring;
    AudioClock clock;

    std::mutex stateMutex;
    std::condition_variable stateCv;
    enum class Wait { kNothing, kStreaming, kFailed } waitResult = Wait::kNothing;
    std::string failure;

    // The loop thread tears down the pw objects it created; stop() waits on
    // this before destroying the context itself.
    std::mutex teardownMutex;
    std::condition_variable teardownCv;
    bool tornDown = false;

    std::atomic<uint32_t> negotiatedRate{0};

    // Transport (task 5.4). The stream starts inactive; resume()/pause()
    // toggle server consumption (and the clock's frozen state together
    // with it). The flush request is answered by the process callback,
    // which owns the ring's consumer end.
    std::atomic<bool> activeFlag{false};
    std::atomic<bool> pausedFlag{false};
    std::atomic<bool> flushRequested{false};
    std::atomic<uint64_t> flushes{0};

    // Written only by the PipeWire data thread; relaxed loads elsewhere.
    std::atomic<uint64_t> callbacks{0};
    std::atomic<uint64_t> underrunEvents{0};
    std::atomic<uint64_t> underrunFrames{0};
    std::atomic<int64_t> lastProcessNs{-1};
    std::array<std::atomic<uint32_t>, kHistogramBuckets> intervals{};

    bool start(uint32_t preferredRate, std::string* error, size_t capacityFrames);
    void stop();
    void process();
    void stateChanged(enum pw_stream_state state);
    void paramChanged(uint32_t id, const spa_pod* param);
    // Activation is a control operation: PipeWire refuses it from foreign
    // threads ("called from wrong context"), so it runs under the loop's
    // control lock — the same discipline pw_thread_loop users get from
    // pw_thread_loop_lock(). The lock is re-entrant for the owner, so the
    // loop thread itself (or a stream callback) may call this too.
    int setStreamActive(bool active);

    double p99Ms() const {
        uint64_t total = 0;
        for (const auto& bucket : intervals) {
            total += bucket.load(std::memory_order_relaxed);
        }
        if (total == 0) {
            return 0.0;
        }
        uint64_t seen = 0;
        for (size_t i = 0; i < intervals.size(); ++i) {
            seen += intervals[i].load(std::memory_order_relaxed);
            if (seen * 100 >= total * 99) {
                return static_cast<double>(i + 1) * kBucketMicros / 1000.0;
            }
        }
        return static_cast<double>(kHistogramBuckets) * kBucketMicros / 1000.0;
    }
};

bool PipeWireSink::Impl::start(uint32_t preferredRate, std::string* error,
                               size_t capacityFrames) {
    // pw_init is process-wide; nothing de-inits it, same as every long-lived
    // client application does.
    static std::once_flag pwOnce;
    std::call_once(pwOnce, [] { pw_init(nullptr, nullptr); });

    if (!ring.create(capacityFrames)) {
        if (error) {
            *error = "could not allocate the audio ring";
        }
        return false;
    }

    loop = pw_main_loop_new(nullptr);
    if (!loop) {
        if (error) {
            *error = "pw_main_loop_new failed";
        }
        return false;
    }
    context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    if (!context) {
        if (error) {
            *error = "pw_context_new failed";
        }
        stop();
        return false;
    }
    // Connecting the context to the daemon is where "no PipeWire" surfaces.
    core = pw_context_connect(context, nullptr, 0);
    if (!core) {
        if (error) {
            *error = std::string("pw_context_connect: ") + std::strerror(errno);
        }
        stop();
        return false;
    }
    // Ask for a 1024-frame quantum: without a latency property the server
    // hands the whole ring per callback and the callback cadence goes to
    // ~4 Hz, which would make the timing histogram meaningless.
    pw_properties* properties = pw_properties_new(PW_KEY_NODE_LATENCY, "1024/48000", nullptr);
    stream = pw_stream_new(core, "openshow-bench", properties);
    if (!stream) {
        if (error) {
            *error = "pw_stream_new failed";
        }
        stop();
        return false;
    }

    static const struct pw_stream_events kEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed =
            [](void* data, enum pw_stream_state, enum pw_stream_state state, const char*) {
                static_cast<Impl*>(data)->stateChanged(state);
            },
        .param_changed =
            [](void* data, uint32_t id, const struct spa_pod* param) {
                static_cast<Impl*>(data)->paramChanged(id, param);
            },
        .process =
            [](void* data) { static_cast<Impl*>(data)->process(); },
    };
    pw_stream_add_listener(stream, &streamListener, &kEvents, this);

    uint8_t stack[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(stack, sizeof(stack));
    struct spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = preferredRate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const struct spa_pod* param =
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    const int rc = pw_stream_connect(
        stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                          PW_STREAM_FLAG_INACTIVE),
        &param, 1);
    if (rc < 0) {
        if (error) {
            *error = "pw_stream_connect: " + pwErrorText(rc);
        }
        stop();
        return false;
    }

    loopThread = std::thread([this] {
        pw_main_loop_run(loop);
        // run() returned because stop() quit the loop: this thread owns the
        // context, so the objects created on it die here (PipeWire rejects
        // calls from foreign threads while the loop is live).
        if (stream) {
            pw_stream_disconnect(stream);
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (core) {
            pw_core_disconnect(core);
            core = nullptr;
        }
        {
            std::lock_guard lock(teardownMutex);
            tornDown = true;
        }
        teardownCv.notify_all();
    });

    std::unique_lock lock(stateMutex);
    const bool settled = stateCv.wait_for(lock, std::chrono::milliseconds(1500), [&] {
        return waitResult != Wait::kNothing;
    });
    if (!settled) {
        if (error) {
            *error = "no STREAMING state within 1.5s (PipeWire daemon unavailable?)";
        }
        lock.unlock();
        stop();
        return false;
    }
    if (waitResult == Wait::kFailed) {
        if (error) {
            *error = failure.empty() ? "stream failed to start" : failure;
        }
        lock.unlock();
        stop();
        return false;
    }

    // Negotiation finished before STREAMING; anchor the clock exactly here.
    clock.setRate(negotiatedRate.load(std::memory_order_relaxed));
    clock.anchor();
    return true;
}

int PipeWireSink::Impl::setStreamActive(bool active) {
    if (!stream) {
        return -EINVAL;
    }
    struct pw_loop* control = loop ? pw_main_loop_get_loop(loop) : nullptr;
    if (control) {
        pw_loop_lock(control);  // blocks until the loop thread yields control
    }
    const int res = pw_stream_set_active(stream, active);
    if (control) {
        pw_loop_unlock(control);
    }
    return res;
}

void PipeWireSink::Impl::stop() {
    if (loopThread.joinable()) {
        pw_main_loop_quit(loop);  // thread-safe by contract
        std::unique_lock lock(teardownMutex);
        teardownCv.wait(lock, [&] { return tornDown; });
        lock.unlock();
        loopThread.join();
    } else {
        // The loop thread never ran (connection failed before it started):
        // nothing contends, drop the objects directly.
        if (stream) {
            pw_stream_disconnect(stream);
            pw_stream_destroy(stream);
            stream = nullptr;
        }
        if (core) {
            pw_core_disconnect(core);
            core = nullptr;
        }
    }
    if (context) {
        pw_context_destroy(context);
        context = nullptr;
    }
    if (loop) {
        pw_main_loop_destroy(loop);
        loop = nullptr;
    }
}

void PipeWireSink::Impl::process() {
    // A transport flush belongs to this thread: it owns the ring's
    // consumer end, so discarding queued-but-unheard audio is safe here.
    if (flushRequested.exchange(false, std::memory_order_relaxed)) {
        ring.dropAll();
        flushes.fetch_add(1, std::memory_order_relaxed);
    }
    struct pw_buffer* buffer = pw_stream_dequeue_buffer(stream);
    if (!buffer) {
        // The server needed a buffer we could not be given: that is its own
        // kind of gap on the timeline.
        underrunEvents.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    struct spa_buffer* data = buffer->buffer;
    auto* destination = static_cast<float*>(data->datas[0].data);
    // datas[0].maxsize is bytes; one frame is two F32 samples.
    const size_t frames = data->datas[0].maxsize / (2 * sizeof(float));

    size_t filled = 0;
    if (destination && frames > 0) {
        filled = ring.read(destination, frames);
        if (filled < frames) {
            std::memset(destination + filled * 2, 0, (frames - filled) * 2 * sizeof(float));
            underrunEvents.fetch_add(1, std::memory_order_relaxed);
            underrunFrames.fetch_add(frames - filled, std::memory_order_relaxed);
        }
        data->datas[0].chunk->offset = 0;
        data->datas[0].chunk->stride = static_cast<int32_t>(2 * sizeof(float));
        data->datas[0].chunk->size = static_cast<int32_t>(frames * 2 * sizeof(float));
    }
    pw_stream_queue_buffer(stream, buffer);

    // The delivered frames are the clock; the padded silence played too.
    clock.addDelivered(frames);
    callbacks.fetch_add(1, std::memory_order_relaxed);

    // Hand the server's authoritative playback position to the clock
    // (position ticks at now, advancing at rate.denom/rate.num fps).
    struct pw_time streamTime = {};
    if (pw_stream_get_time(stream, &streamTime) == 0 && streamTime.rate.denom > 0 &&
        streamTime.rate.num > 0) {
        clock.setStreamTime(streamTime.ticks, streamTime.now,
                            static_cast<double>(streamTime.rate.denom) /
                                static_cast<double>(streamTime.rate.num));
    }

    const int64_t now = monotonicNs();
    const int64_t previous = lastProcessNs.exchange(now, std::memory_order_relaxed);
    if (previous >= 0 && now >= previous) {
        const uint64_t micros = static_cast<uint64_t>(now - previous) / 1000;
        size_t bucket = micros / kBucketMicros;
        if (bucket >= kHistogramBuckets) {
            bucket = kHistogramBuckets - 1;
        }
        intervals[bucket].fetch_add(1, std::memory_order_relaxed);
    }
}

void PipeWireSink::Impl::stateChanged(enum pw_stream_state state) {
    std::lock_guard lock(stateMutex);
    switch (state) {
        case PW_STREAM_STATE_STREAMING:
            waitResult = Wait::kStreaming;
            break;
        case PW_STREAM_STATE_PAUSED:
            // The stream connects INACTIVE (task 5.4): PAUSED is where a
            // connected, not-yet-resumed stream settles, so readiness —
            // and negotiation — completes here.
            if (waitResult == Wait::kNothing) {
                waitResult = Wait::kStreaming;
            }
            break;
        case PW_STREAM_STATE_ERROR:
            waitResult = Wait::kFailed;
            failure = "PipeWire stream error";
            break;
        case PW_STREAM_STATE_UNCONNECTED:
            if (waitResult == Wait::kNothing) {
                waitResult = Wait::kFailed;
                failure = "PipeWire stream unconnected (is the daemon running?)";
            }
            break;
        default:
            break;
    }
    stateCv.notify_all();
}

void PipeWireSink::Impl::paramChanged(uint32_t id, const spa_pod* param) {
    if (id != SPA_PARAM_Format || param == nullptr) {
        return;
    }
    struct spa_audio_info_raw negotiated = {};
    // Observed (PipeWire 1.6): success returns a positive parsed-size,
    // failure a negative errno.
    if (spa_format_audio_raw_parse(param, &negotiated) >= 0 && negotiated.rate > 0) {
        negotiatedRate.store(negotiated.rate, std::memory_order_relaxed);
    }
}

PipeWireSink::PipeWireSink() : fImpl(std::make_unique<Impl>()) {}

PipeWireSink::~PipeWireSink() {
    fImpl->stop();
}

bool PipeWireSink::start(uint32_t preferredRate, std::string* error, size_t capacityFrames) {
    return fImpl->start(preferredRate, error, capacityFrames);
}

void PipeWireSink::stop() {
    fImpl->stop();
}

size_t PipeWireSink::feed(const float* interleavedStereo, size_t frames) {
    return fImpl->ring.write(interleavedStereo, frames);
}

uint32_t PipeWireSink::negotiatedRate() const {
    return fImpl->negotiatedRate.load(std::memory_order_relaxed);
}

const AudioClock& PipeWireSink::clock() const {
    return fImpl->clock;
}

void PipeWireSink::resume() {
    Impl& impl = *fImpl;
    if (!impl.stream) {
        return;
    }
    const bool wasPaused = impl.pausedFlag.exchange(false, std::memory_order_acq_rel);
    const bool wasActive = impl.activeFlag.load(std::memory_order_relaxed);
    if (wasPaused) {
        // Continuing from pause(): fold the stale-extrapolation overshoot
        // into the clock offset so the position continues from the hold.
        impl.clock.resume();
    } else if (!wasActive) {
        // First activation (the stream connected INACTIVE): the media
        // clock starts exactly now, with the ring pre-filled by the
        // owner so the first frame the server pulls is real content.
        impl.clock.anchor();
    }
    const int res = impl.setStreamActive(true);
    if (res < 0) {
        // Do not claim an activation the server refused (silent failure
        // here was the original 5.4 bug): rewind the transport flags.
        std::fprintf(stderr, "openshow-worker: sink: activation failed (%s)\n",
                     pwErrorText(res).c_str());
        impl.pausedFlag.store(wasPaused, std::memory_order_release);
        if (wasPaused) {
            impl.clock.pause();  // re-latches essentially the same hold
        }
        return;
    }
    impl.activeFlag.store(true, std::memory_order_release);
}

void PipeWireSink::pause() {
    Impl& impl = *fImpl;
    if (!impl.stream) {
        return;
    }
    // Freeze the clock before deactivating: stream time stops advancing
    // once the server stops the stream, but the clock's extrapolation
    // would not, so the hold must latch the current position first.
    impl.clock.pause();
    impl.pausedFlag.store(true, std::memory_order_release);
    const int res = impl.setStreamActive(false);
    if (res < 0) {
        std::fprintf(stderr, "openshow-worker: sink: deactivation failed (%s)\n",
                     pwErrorText(res).c_str());
        impl.pausedFlag.store(false, std::memory_order_release);
        impl.clock.resume();
        return;
    }
    impl.activeFlag.store(false, std::memory_order_release);
}

bool PipeWireSink::active() const {
    return fImpl->activeFlag.load(std::memory_order_acquire);
}

void PipeWireSink::requestFlush() {
    fImpl->flushRequested.store(true, std::memory_order_release);
}

void PipeWireSink::rebaseClock(double mediaSeconds) {
    fImpl->clock.rebase(mediaSeconds);
}

PipeWireSink::Stats PipeWireSink::stats() const {
    Stats out;
    out.negotiatedRate = fImpl->negotiatedRate.load(std::memory_order_relaxed);
    out.deliveredFrames = fImpl->clock.deliveredFrames();
    out.callbacks = fImpl->callbacks.load(std::memory_order_relaxed);
    out.underrunEvents = fImpl->underrunEvents.load(std::memory_order_relaxed);
    out.underrunFrames = fImpl->underrunFrames.load(std::memory_order_relaxed);
    out.flushes = fImpl->flushes.load(std::memory_order_relaxed);
    out.active = fImpl->activeFlag.load(std::memory_order_relaxed);
    out.callbackP99Ms = fImpl->p99Ms();
    out.clockSeconds = fImpl->clock.clockSeconds();
    out.wallSeconds = fImpl->clock.wallElapsedSeconds();
    return out;
}

}  // namespace openshow::media
