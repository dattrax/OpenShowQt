/*
 * MediaProbe: container and stream inspection for the Qt-free worker
 * (task 1.5a).
 *
 * Opens a media file with libavformat, reads the stream headers and reports
 * what the linked FFmpeg can actually decode, so the frontend can say
 * "unsupported format/codec" (PLAN.md acceptance) without the worker ever
 * guessing. The public types carry no FFmpeg headers: this is the module's
 * Qt-free, FFmpeg-free face.
 */

#pragma once

#include <string>
#include <vector>

namespace openshow::media {

// One video stream as the container and codec headers describe it.
struct VideoStreamInfo {
    int index = -1;              // index into the container, what decoding selects
    std::string codec;           // decoder name source, e.g. "h264"
    std::string profile;         // best effort, empty when unknown
    int width = 0;
    int height = 0;
    std::string pixelFormat;     // e.g. "yuv420p"; decoding converts to I420 as needed
    double frameRate = 0.0;      // guessed fps; 0 when variable or unknown
    bool frameRateConstant = false;  // false: guessed from container hints
    std::string colorSpace;      // e.g. "bt709", "bt601", "unspecified"
    std::string colorRange;      // e.g. "mpeg" (limited), "jpeg" (full), "unspecified"
    bool decoderAvailable = false;
};

struct AudioStreamInfo {
    int index = -1;
    std::string codec;           // e.g. "aac", "mp3", "pcm_s16le"
    int sampleRate = 0;
    int channels = 0;
    std::string channelLayout;   // e.g. "stereo", "mono", "5.1"
    std::string sampleFormat;    // e.g. "fltp", "s16"
    bool decoderAvailable = false;
};

struct MediaInfo {
    std::string path;
    std::string formatName;      // demuxer, e.g. "mov,mp4,m4a,3gp,3g2,mj2"
    double durationSeconds = 0.0;  // 0 when the container does not say
    std::vector<VideoStreamInfo> videoStreams;
    std::vector<AudioStreamInfo> audioStreams;
};

enum class ProbeStatus {
    kOk,             // opened; streams reported (a stream may still be
                     // decoderAvailable == false — the caller decides)
    kOpenFailed,     // missing file, unreadable, or not a recognised container
    kNoUsableStreams,  // parsed, but no video and no audio we could decode
};

// Reads container and stream headers. Fills *out on kOk; on failure fills
// *error (when given) with a message naming the path and the FFmpeg reason.
ProbeStatus probeMedia(const std::string& path, MediaInfo* out, std::string* error);

}  // namespace openshow::media
