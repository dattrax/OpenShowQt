/*
 * AudioFileFeeder: decode a file's audio stream into a sink-shaped write
 * callback (task 1.5e).
 *
 * Same isolation rule as VideoFrameSource: the feeder owns a second
 * container handle rather than sharing a demuxer across threads. Packets
 * decode to PCM, libswresample converts to interleaved stereo F32 at the
 * target rate (the PipeWire sink's negotiated rate in the bench), and the
 * frames go out through a write callback. The callback is expected to be
 * non-blocking (PipeWireSink::feed returns a short count when its ring is
 * full); the feeder then sleeps a couple of milliseconds and retries —
 * it never blocks on consumer-owned state, and requestStop() cuts even
 * those sleeps short.
 *
 * With loop set, EOF rewinds and feeding continues; the point is a bench
 * that keeps clock and ring busy until told to stop.
 *
 * Transport (task 5.4): requestSeek(targetSeconds) is answered by the
 * decode thread between packets (the 5.2 video pattern: backward seek,
 * decoder flush, resampler re-init, then frames entirely before the
 * target are resampled into silence, counted, not emitted — never
 * dropped, dropping would shorten the stream and break the heard-audio
 * position). After the jump the feeder is quiet, so it calls the seeked
 * callback (setSeekedCallback, default none) from the decode thread;
 * owners use it to flush the input's mixer ring then — flushing while
 * the feeder still emits would drop post-seek content instead.
 * waitSeekApplied() lets transport callers order work after the jump.
 * setLoopEnabled() toggles EOF rewind live.
 *
 * Why no clock pacing is needed: the sink's clock is the SERVER's
 * consumption position, and this feeder's content maps ring-index to
 * media-position continuously (pre-fill, pauses and queue depth only
 * add latency, never heard-vs-clock skew — the samples sit in the queue
 * in exact media order). Only a flush re-anchors that mapping, so only
 * a seek flushes; a paused feeder pile-up against the bounded rings
 * resumes as seamless, correctly positioned audio.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace openshow::media {

class AudioFileFeeder {
public:
    // Consumes interleaved stereo F32 frames, returns how many were taken.
    using WriteFn = std::function<size_t(const float*, size_t)>;

    AudioFileFeeder() = default;
    ~AudioFileFeeder();

    AudioFileFeeder(const AudioFileFeeder&) = delete;
    AudioFileFeeder& operator=(const AudioFileFeeder&) = delete;

    // Opens the container and the best audio stream's decoder. targetRate
    // is the rate to resample towards (start the sink first and pass its
    // negotiatedRate()). No thread runs yet.
    bool open(const std::string& path, uint32_t targetRate, std::string* error);
    void close();

    // Starts the feed thread; exactly one start per open().
    bool start(WriteFn write, bool loop, std::string* error);

    // Unwinds the thread (retried writes and sleeps abort); both are
    // idempotent and safe before start.
    void requestStop();
    void join();

    // --- transport (task 5.4) ---
    // Asynchronous: the decode thread applies the seek between packets.
    // Consecutive requests coalesce (the latest target wins).
    void requestSeek(double targetSeconds);
    // Blocks until the newest request has been applied (or timeoutMs
    // passes); returns whether it was applied.
    bool waitSeekApplied(int timeoutMs);
    double lastSeekSeconds() const { return fLastSeekSeconds.load(std::memory_order_relaxed); }
    // Runs on the decode thread right after a seek is applied, while the
    // feeder is quiet — the race-free moment to flush downstream stages.
    void setSeekedCallback(std::function<void(double)> callback) {
        fSeekedCallback = std::move(callback);
    }
    // Live EOF-rewind toggle; takes effect at the next end of stream.
    void setLoopEnabled(bool loop) { fLoop = loop; }
    bool loopEnabled() const { return fLoop.load(std::memory_order_relaxed); }

    struct Stats {
        uint64_t framesFed = 0;       // frames the callback accepted
        uint64_t writeRetries = 0;    // full-ring retries (pacing, not errors)
        uint64_t decodeErrors = 0;
        uint64_t loops = 0;
        uint64_t seeks = 0;           // seeks applied by the decode thread
        uint64_t seekDiscards = 0;    // pre-target frames silenced at a seek
        bool eof = false;             // thread ended at EOF (loop was off)
    };
    Stats stats() const;

private:
    void run();
    bool rewindForLoop();
    // Applies the newest pending seek; false means the container seek
    // failed (still counted as applied — the transport must not hang).
    bool applyPendingSeek(double targetSeconds);

    std::string fPath;
    uint32_t fTargetRate = 0;

    // FFmpeg handles kept opaque so this header stays FFmpeg-free.
    void* fFormatContext = nullptr;  // AVFormatContext*
    void* fCodecContext = nullptr;   // AVCodecContext*
    int fAudioStreamIndex = -1;

    WriteFn fWrite;
    std::thread fThread;
    std::atomic<bool> fLoop{false};
    std::atomic<bool> fStopRequested{false};
    std::atomic<bool> fThreadWanted{false};

    // Seek transport: one writer (the transport caller), the decode
    // thread consumes; generations pair request with application.
    std::atomic<double> fSeekTargetSeconds{0.0};
    std::atomic<uint64_t> fSeekPendingGeneration{0};
    std::atomic<uint64_t> fSeekAppliedGeneration{0};
    std::atomic<double> fLastSeekSeconds{0.0};
    std::function<void(double)> fSeekedCallback;
    // Skip-before-emit window: frames ending at or before this media
    // position are resampled into silence after a seek (never dropped —
    // dropping would shorten the stream against the clock). Decode-thread
    // state only: fSkipUntilSeconds is written by run(), read there and
    // zeroed by applyPendingSeek before the skip window opens.
    std::atomic<double> fSkipUntilSeconds{0.0};
    // SwrContext* while the decode thread runs (applyPendingSeek rebuilds
    // its delay window); nullptr otherwise.
    void* fSwrContext = nullptr;

    mutable std::mutex fStatsMutex;
    Stats fStats;
};

}  // namespace openshow::media
