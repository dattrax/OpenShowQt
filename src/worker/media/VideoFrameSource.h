/*
 * Dedicated demux/decode thread. Deferred open defaults to direct software
 * decode: get_buffer2 allocates native YUV planes in the GPU backing's mapped
 * linear images. createRing primes and validates the first frame before the
 * render loop starts. The bounded ring then carries presentation tokens;
 * publishing copies the retained decoder image to an optimal image on the GPU.
 *
 * setDirectDecodeEnabled(false) explicitly selects the copying compatibility
 * path (VA-API download or software plane copy/conversion). The CPU-only
 * open(path, depth) convenience overload also uses that path.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    // the ring with ringDepth slots (plain memory) at the visible coded
    // size. Fills streamInfo() from MediaProbe. No thread runs yet.
    bool open(const std::string& path, unsigned ringDepth, std::string* error);

    // Deferred open: GPU-backed, direct software decode by default. Create
    // the uploader from the exposed dimensions/native format, then createRing.
    bool open(const std::string& path, std::string* error);
    bool createRing(unsigned ringDepth, FrameBacking* backing, std::string* error);
    FrameFormat ringFormat() const { return fRingFormat; }
    int frameWidth() const { return fFrameWidth; }
    int frameHeight() const { return fFrameHeight; }

    void close();
    void setDirectDecodeEnabled(bool enabled) { fDirectDecode = enabled; }
    bool directDecodeEnabled() const { return fDirectDecode; }

    // Private FFmpeg callback bridge; arguments remain opaque in public headers.
    int allocateDecoderBuffer(void* codec, void* frame);

    // Applies only to the explicitly selected compatibility path.
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
        uint64_t directFrames = 0;
        uint64_t downloadedFrames = 0;
        uint64_t copiedFrames = 0;
        uint64_t convertedFrames = 0;
        std::string fatalError;
        bool eof = false;  // the thread ended at EOF (loop was off)
    };
    Stats stats() const;

private:
    void run();
    bool primeDirect(std::string* error);
    std::shared_ptr<FrameStorage> decoderStorage(void* frame) const;
    void failDecode(const std::string& error);
    bool openInternal(const std::string& path, std::string* error);

    bool fDirectDecode = true;
    FrameBacking* fBacking = nullptr;
    void* fPrimedFrame = nullptr; // AVFrame*, validated before starting the thread
    int fNativePixelFormat = -1;
    std::vector<std::weak_ptr<FrameStorage>> fDecodeAllocations;
    std::string fAllocationError; // guarded by fStatsMutex (codec callbacks)
    std::string fPath;
    VideoStreamInfo fStreamInfo;
    FrameRing fRing;
    // Coded size and ring slot format, fixed at open(); createRing() and a
    // GPU backing read these to agree on the slot layout before frames flow.
    int fFrameWidth = 0;
    int fFrameHeight = 0;
    FrameFormat fRingFormat = FrameFormat::kNV12;

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
