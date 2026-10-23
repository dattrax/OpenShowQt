/*
 * VideoFrameSource: demux + software decode on a dedicated thread (task 1.5b).
 *
 * Owns one container, one decoder and one decode thread. Video packets go
 * through libavcodec; frames leave as I420 (converted only when the stream
 * is not yuv420p) into a FrameRing. The thread never blocks on anything
 * consumer-owned beyond the bounded ring, and requestStop() breaks even a
 * full-ring wait, so shutdown never waits on a stalled consumer.
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
    void* fSwsContext = nullptr;      // SwsContext*, only when conversion needed
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
