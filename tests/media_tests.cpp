/*
 * Task 1.5a: MediaProbe against generated fixtures.
 *
 * The fixtures are produced with the ffmpeg CLI (H.264/AAC MP4 plus a plain
 * WAV) so the probe sees real container and codec headers. When the CLI is
 * unavailable the test exits 77, which ctest reports as skipped.
 */

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "media/MediaProbe.h"

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

}  // namespace

int main() {
    namespace fs = std::filesystem;

    const fs::path dir = "media-fixtures";
    std::error_code ec;
    fs::create_directories(dir, ec);

    const std::string mp4 = (dir / "probe.mp4").string();
    const std::string wav = (dir / "probe.wav").string();
    const std::string garbage = (dir / "garbage.bin").string();

    const bool generated =
        generate("ffmpeg -hide_banner -loglevel error -y "
                 "-f lavfi -i testsrc2=size=320x240:rate=30:duration=2 "
                 "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2 "
                 "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -ac 2 -shortest " + mp4) &&
        generate("ffmpeg -hide_banner -loglevel error -y "
                 "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 " + wav);
    if (!generated) {
        std::printf("SKIP: the ffmpeg CLI could not generate the fixtures\n");
        return 77;
    }
    {
        std::ofstream out(garbage, std::ios::binary);
        out << std::string(4096, 'x');
    }

    bool ok = true;

    // A real H.264/AAC MP4: every reported field must match the fixture.
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

    // Structured failures: a missing path and a file that is not media at
    // all both answer kOpenFailed with an explanatory message.
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

    // Audio-only container: one stream, no video, still usable.
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

    if (!ok) {
        return 1;
    }
    std::printf("media probe checks passed\n");
    return 0;
}
