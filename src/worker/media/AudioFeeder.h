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

    struct Stats {
        uint64_t framesFed = 0;       // frames the callback accepted
        uint64_t writeRetries = 0;    // full-ring retries (pacing, not errors)
        uint64_t decodeErrors = 0;
        uint64_t loops = 0;
        bool eof = false;             // thread ended at EOF (loop was off)
    };
    Stats stats() const;

private:
    void run();
    bool rewindForLoop();

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
    mutable std::mutex fStatsMutex;
    Stats fStats;
};

}  // namespace openshow::media
