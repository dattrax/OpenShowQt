/*
 * VideoFrameSource: demux + decode on a dedicated thread (task 1.5b).
 *
 * Owns one container, one decoder and one decode thread. Video packets go
 * through libavcodec; frames leave as I420 (converted only when the stream
 * is not yuv420p) into a FrameRing. The thread never blocks on anything
 * consumer-owned beyond the bounded ring, and requestStop() breaks even a
 * full-ring wait, so shutdown never waits on a stalled consumer.
 *
 * Since 1.5h open() prefers hardware decode: a VA-API device attached to
 * the native decoder as hw_device_ctx, plus a get_format hook that picks
 * AV_PIX_FMT_VAAPI when the decoder offers it (FFmpeg's *_vaapi codecs
 * are encode-only — VA-API decode is an hwaccel, not a decoder — and
 * external wrapper decoders like libdav1d ignore hwaccels, hence the
 * native-decoder preference). Frames then arrive as GPU surfaces that
 * the decode thread copies back to system memory (NV12/P010) and convert
 * to I420 before the ring — the win is the decode work itself leaving
 * the CPU, not the frame path. Anything that cannot accelerate decodes
 * in software exactly as before, and decodeDescription() says which path
 * was taken and why.
 *
 * Audio packets are skipped here on purpose: 1.5c runs its own feed from a
 * second file handle rather than sharing a demuxer, keeping every thread's
 * blocking behaviour simple (PLAN.md: never mix decode on the render or RPC
 * threads — and here, never share one demux context across threads).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "media/FrameRing.h"
#include "media/MediaProbe.h"

namespace openshow::media {

class VideoFrameSource {
public:
    VideoFrameSource() = default;
    ~VideoFrameSource();

    VideoFrameSource(const VideoFrameSource&) = delete;
    VideoFrameSource& operator=(const VideoFrameSource&) = delete;

    // Opens the container and the best video stream's decoder, and creates
    // the ring with ringDepth I420 slots at the visible coded size. Fills
    // streamInfo() from MediaProbe. No thread runs yet.
    bool open(const std::string& path, unsigned ringDepth, std::string* error);
    void close();

    // Hardware decode is attempted at open() unless switched off beforehand
    // (--no-hw-decode exists to A/B the bench against software decoding).
    void setHardwareDecodeEnabled(bool enabled) { fHardwareDecodeEnabled = enabled; }

    // Human-readable record of what decode ended up with, e.g.
    // "h264 (VA-API hardware decode)" or "h264 (software decode; VA-API
    // device unavailable: ...)". Valid after a successful open(); when
    // hardware was configured it is settled by the first decoded frame,
    // which is when the hwaccel negotiates its pixel format.
    const std::string& decodeDescription() const { return fDecodeDescription; }

    // Called from the FFmpeg get_format hook (see negotiatePixelFormat in
    // the .cpp) to report whether a VAAPI format was negotiated. Not for
    // ordinary callers.
    void noteFormatNegotiated(const char* decoderName, bool hardware);

    const VideoStreamInfo& streamInfo() const { return fStreamInfo; }
    FrameRing& ring() { return fRing; }

    // Starts the decode thread. With loop set, EOF rewinds to the start and
    // decoding continues; sequence numbers in the ring stay monotonic.
    // Exactly one start per open().
    bool startDecode(bool loop, std::string* error);

    // Asks the thread to unwind (wakes a ring-blocked producer) and joins
    // it. Both are idempotent; safe before startDecode.
    void requestStop();
    void join();

    struct Stats {
        uint64_t packetsRead = 0;
        uint64_t framesProduced = 0;
        uint64_t decodeErrors = 0;
        uint64_t loops = 0;
        bool eof = false;  // the thread ended at EOF (loop was off)
    };
    Stats stats() const;

private:
    void run();

    std::string fPath;
    VideoStreamInfo fStreamInfo;
    FrameRing fRing;

    // FFmpeg handles kept opaque so this header stays FFmpeg-free.
    void* fFormatContext = nullptr;   // AVFormatContext*
    void* fCodecContext = nullptr;    // AVCodecContext*
    void* fHwDeviceCtx = nullptr;     // AVBufferRef*, held while decode may use VA-API
    bool fHardwareDecodeEnabled = true;
    std::atomic<bool> fNegotiatedLogged{false};  // one-shot get_format outcome
    std::string fDecodeDescription;
    int fVideoStreamIndex = -1;
    double fTimeBaseSeconds = 0.0;
    double fSyntheticPtsSeconds = 0.0;  // carries pts when a frame has none
    double fSyntheticStepSeconds = 0.04;

    std::thread fThread;
    std::atomic<bool> fLoop{false};
    std::atomic<bool> fThreadWanted{false};
    mutable std::mutex fStatsMutex;
    Stats fStats;
};

}  // namespace openshow::media
