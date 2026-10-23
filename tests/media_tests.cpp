/*
 * Task 1.5 media foundation: MediaProbe (1.5a), FrameRing + decode thread
 * (1.5b), all against generated fixtures.
 *
 * Fixtures come from the ffmpeg CLI so the probe sees real container and
 * codec headers and the decoder sees real H.264. When the CLI is
 * unavailable the test exits 77, which ctest reports as skipped.
 */

#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "media/FrameRing.h"
#include "media/MediaProbe.h"
#include "media/VideoFrameSource.h"

namespace {

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
    return condition;
}

bool generate(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

using openshow::media::FrameRing;
using openshow::media::VideoFrame;
using openshow::media::VideoFrameSource;

// 1.5b: the ring's own contract, with no FFmpeg and no threads.
bool exerciseRing() {
    bool ok = true;
    FrameRing ring;
    ok = check(ring.create(2, 16, 16, nullptr), "ring creates");
    ok = check(!ring.create(0, 16, 16, nullptr) && !ring.create(2, 15, 16, nullptr),
               "ring rejects empty slot lists and odd sizes") && ok;
    ok = check(ring.create(2, 16, 16, nullptr), "ring recreates") && ok;

    VideoFrame a;
    VideoFrame b;
    ok = check(ring.claim(&a) && ring.claim(&b), "both slots claimable") && ok;
    a.ptsSeconds = 1.0;
    b.ptsSeconds = 2.0;
    ring.publish(a);
    ring.publish(b);

    VideoFrame out;
    ok = check(ring.poll(&out, std::chrono::milliseconds(100)) && out.ptsSeconds == 1.0 &&
                   out.sequence == 1,
               "oldest frame comes out first with a monotone sequence") && ok;
    ring.release(out.slot);
    ok = check(ring.poll(&out, std::chrono::milliseconds(100)) && out.ptsSeconds == 2.0 &&
                   out.sequence == 2,
               "second frame follows") && ok;
    ring.release(out.slot);

    ok = check(ring.claim(&a), "slot claimable again once released") && ok;
    ring.abortClaim(a.slot);
    ok = check(!ring.poll(&out, std::chrono::milliseconds(10)),
               "aborted claim stays invisible") && ok;

    // stop() drains what is visible, then nothing.
    ok = check(ring.claim(&a), "claim before stop") && ok;
    a.ptsSeconds = 5.0;
    ring.publish(a);
    ring.stop();
    ok = check(ring.poll(&out, std::chrono::milliseconds(10)) && out.ptsSeconds == 5.0,
               "visible frames still drain after stop") && ok;
    ok = check(!ring.poll(&out, std::chrono::milliseconds(10)) && !ring.claim(&b),
               "stopped ring hands out nothing more") && ok;

    const FrameRing::Stats stats = ring.stats();
    ok = check(stats.published == 3 && stats.consumed == 3 && stats.peakFilled <= 2,
               "stats count the traffic and stay inside the ring") && ok;
    return ok;
}

// 1.5b: demux + decode a 1080p fixture on the source thread and drain fast.
bool exerciseDecode(const std::string& hdPath) {
    bool ok = true;
    VideoFrameSource source;
    std::string error;
    ok = check(source.open(hdPath, 6, &error), "source opens the 1080p fixture") && ok;
    if (!ok) {
        std::fprintf(stderr, "  open: %s\n", error.c_str());
        return ok;
    }
    ok = check(source.streamInfo().width == 1920 && source.streamInfo().height == 1080 &&
                   source.streamInfo().codec == "h264",
               "stream info carries the fixture geometry") && ok;
    ok = check(source.ring().width() == 1920 && source.ring().height() == 1080,
               "the ring lives at the coded size") && ok;

    ok = check(source.startDecode(false, &error), "decode starts") && ok;

    const auto start = std::chrono::steady_clock::now();
    unsigned count = 0;
    unsigned expectedSequence = 1;
    double lastPts = -1.0;
    bool ptsMonotone = true;
    VideoFrame frame;
    while (source.ring().poll(&frame, std::chrono::milliseconds(2000))) {
        ptsMonotone = ptsMonotone && frame.ptsSeconds > lastPts;
        lastPts = frame.ptsSeconds;
        if (frame.sequence != expectedSequence) {
            ptsMonotone = false;  // reused as "sequence broke"; checked once below
        }
        ++expectedSequence;
        // A real consumer converts or uploads the pixels here; 1.5b only
        // needs the geometry to have made it across.
        if (frame.data[0] == nullptr || frame.linesize[0] < 1920) {
            ptsMonotone = false;
        }
        source.ring().release(frame.slot);
        ++count;
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    source.join();
    const VideoFrameSource::Stats stats = source.stats();

    ok = check(ptsMonotone, "pts and sequence advance, plane pointers are real") && ok;
    ok = check(count == 100, "1080p25 x 4s decodes to exactly 100 frames") && ok;
    ok = check(stats.eof && stats.framesProduced == count && stats.decodeErrors == 0 &&
                   stats.loops == 0,
               "clean EOF, every frame produced, no errors") && ok;
    const FrameRing::Stats ringStats = source.ring().stats();
    ok = check(ringStats.peakFilled <= 6, "the queue never grew past its slots") && ok;
    // Acceptance: sustained decode at 2x the source rate.
    ok = check(elapsed < static_cast<double>(count) / 50.0,
               "decodes at least twice real-time (2x of 25 fps)") && ok;
    std::printf("ok: decoded %u frames in %.2fs (%.1f fps), peakFilled %llu\n", count, elapsed,
                count / elapsed, static_cast<unsigned long long>(ringStats.peakFilled));
    return ok;
}

// 1.5b: a full ring must block the producer, and requestStop must wake it.
bool exerciseBackpressure(const std::string& hdPath) {
    bool ok = true;
    VideoFrameSource source;
    std::string error;
    ok = check(source.open(hdPath, 2, &error), "source opens with a two-slot ring") && ok;
    if (!ok) {
        return ok;
    }
    ok = check(source.startDecode(false, &error), "decode starts") && ok;

    VideoFrame frame;
    unsigned consumed = 0;
    for (; consumed < 5; ++consumed) {
        if (!source.ring().poll(&frame, std::chrono::milliseconds(2000))) {
            break;
        }
        source.ring().release(frame.slot);
    }
    ok = check(consumed == 5, "five frames arrive") && ok;

    const auto stopAt = std::chrono::steady_clock::now();
    source.requestStop();
    source.join();
    const double stoppedAfter =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - stopAt).count();
    const VideoFrameSource::Stats stats = source.stats();

    // Without blocking the producer would have run the whole clip (100
    // frames) while five were consumed. Published may exceed consumed only
    // by what fits in the ring (2) plus one unpublished claimed frame.
    ok = check(stats.framesProduced <= consumed + 2,
               "the producer stalls on a full ring instead of racing ahead") && ok;
    ok = check(stoppedAfter < 1.0, "requestStop wakes the parked producer promptly") && ok;
    std::printf("ok: produced %llu while consuming %u; stopped after %.3fs\n",
                static_cast<unsigned long long>(stats.framesProduced), consumed, stoppedAfter);
    return ok;
}

// 1.5b: loop mode keeps producing with monotone sequences across the wrap.
bool exerciseLoop(const std::string& mp4Path) {
    bool ok = true;
    VideoFrameSource source;
    std::string error;
    ok = check(source.open(mp4Path, 3, &error), "source opens the looping fixture") && ok;
    if (!ok) {
        return ok;
    }
    ok = check(source.startDecode(true, &error), "looped decode starts") && ok;

    // The fixture carries about 60 frames; 80 consumed means it looped.
    unsigned count = 0;
    unsigned expectedSequence = 1;
    bool sawPtsWrap = false;
    double lastPts = -1.0;
    VideoFrame frame;
    for (; count < 80; ++count) {
        if (!source.ring().poll(&frame, std::chrono::milliseconds(2000))) {
            break;
        }
        ok = frame.sequence == expectedSequence && ok;
        ++expectedSequence;
        if (frame.ptsSeconds < lastPts) {
            sawPtsWrap = true;
        }
        lastPts = frame.ptsSeconds;
        source.ring().release(frame.slot);
    }
    source.requestStop();
    source.join();
    const VideoFrameSource::Stats stats = source.stats();

    ok = check(count == 80, "80 frames cross a ~60-frame clip because decoding loops") && ok;
    ok = check(ok && sawPtsWrap, "sequence stays monotone and pts wraps at the seam") && ok;
    ok = check(stats.loops >= 1 && stats.decodeErrors == 0 && !stats.eof,
               "loop counter advances and the stream never declared EOF") && ok;
    std::printf("ok: looped past %u frames, loops=%llu\n", count,
                static_cast<unsigned long long>(stats.loops));
    return ok;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;

    const fs::path dir = "media-fixtures";
    std::error_code ec;
    fs::create_directories(dir, ec);

    const std::string mp4 = (dir / "probe.mp4").string();
    const std::string wav = (dir / "probe.wav").string();
    const std::string hd = (dir / "decode-hd.mp4").string();
    const std::string garbage = (dir / "garbage.bin").string();

    const bool generated =
        generate("ffmpeg -hide_banner -loglevel error -y "
                 "-f lavfi -i testsrc2=size=320x240:rate=30:duration=2 "
                 "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2 "
                 "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -ac 2 -shortest " + mp4) &&
        generate("ffmpeg -hide_banner -loglevel error -y "
                 "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 " + wav) &&
        generate("ffmpeg -hide_banner -loglevel error -y "
                 "-f lavfi -i testsrc2=size=1920x1080:rate=25:duration=4 "
                 "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -an " + hd);
    if (!generated) {
        std::printf("SKIP: the ffmpeg CLI could not generate the fixtures\n");
        return 77;
    }
    {
        std::ofstream out(garbage, std::ios::binary);
        out << std::string(4096, 'x');
    }

    bool ok = true;

    // ---- 1.5a: MediaProbe ----

    openshow::media::MediaInfo info;
    std::string error;
    ok = check(openshow::media::probeMedia(mp4, &info, &error) ==
                   openshow::media::ProbeStatus::kOk,
               "probe accepts the generated MP4") && ok;
    ok = check(info.formatName.find("mp4") != std::string::npos, "format name is mp4-like") && ok;
    ok = check(info.durationSeconds > 1.5 && info.durationSeconds < 2.5,
               "duration is about two seconds") && ok;
    ok = check(info.videoStreams.size() == 1 && info.audioStreams.size() == 1,
               "one video and one audio stream") && ok;
    if (info.videoStreams.size() == 1) {
        const auto& v = info.videoStreams[0];
        ok = check(v.codec == "h264", "video codec is h264") && ok;
        ok = check(v.width == 320 && v.height == 240, "video is 320x240") && ok;
        ok = check(v.pixelFormat == "yuv420p", "pixel format is yuv420p") && ok;
        ok = check(v.frameRate > 29.0 && v.frameRate < 31.0, "frame rate is about 30") && ok;
        ok = check(v.decoderAvailable, "h264 decoder is available") && ok;
        std::printf("ok: video %s %dx%d %s %.2ffps color %s/%s profile '%s'\n", v.codec.c_str(),
                    v.width, v.height, v.pixelFormat.c_str(), v.frameRate,
                    v.colorSpace.c_str(), v.colorRange.c_str(), v.profile.c_str());
    }
    if (info.audioStreams.size() == 1) {
        const auto& a = info.audioStreams[0];
        ok = check(a.codec == "aac", "audio codec is aac") && ok;
        ok = check(a.sampleRate == 44100, "sample rate is 44100") && ok;
        ok = check(a.channels == 2, "audio is stereo") && ok;
        ok = check(a.decoderAvailable, "aac decoder is available") && ok;
        std::printf("ok: audio %s %dHz '%s' (%s)\n", a.codec.c_str(), a.sampleRate,
                    a.channelLayout.c_str(), a.sampleFormat.c_str());
    }

    info = {};
    error.clear();
    ok = check(openshow::media::probeMedia((dir / "does-not-exist.mp4").string(), &info, &error) ==
                   openshow::media::ProbeStatus::kOpenFailed,
               "missing file is kOpenFailed") && ok;
    ok = check(!error.empty(), "missing file carries an error message") && ok;

    info = {};
    error.clear();
    ok = check(openshow::media::probeMedia(garbage, &info, &error) ==
                   openshow::media::ProbeStatus::kOpenFailed,
               "unparseable file is kOpenFailed") && ok;
    std::printf("ok: structured errors (%s)\n", error.c_str());

    info = {};
    error.clear();
    ok = check(openshow::media::probeMedia(wav, &info, &error) ==
                   openshow::media::ProbeStatus::kOk,
               "probe accepts the WAV") && ok;
    ok = check(info.videoStreams.empty() && info.audioStreams.size() == 1,
               "WAV has audio only") && ok;
    if (!info.audioStreams.empty()) {
        ok = check(info.audioStreams[0].decoderAvailable, "pcm decoder is available") && ok;
    }

    // ---- 1.5b: FrameRing + VideoFrameSource ----
    ok = exerciseRing() && ok;
    ok = exerciseDecode(hd) && ok;
    ok = exerciseBackpressure(hd) && ok;
    ok = exerciseLoop(mp4) && ok;

    if (!ok) {
        return 1;
    }
    std::printf("media checks passed\n");
    return 0;
}
