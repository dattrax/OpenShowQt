// Task 5.2 acceptance: playback transitions and seek flushing pass tests.
// The PlaybackPipeline's controls — play/pause, seek, loop toggle — run
// through the real seams (the decode thread applies seeks between frames,
// the pacing thread flushes and re-anchors), driven headless in CPU mode
// against the default wall clock. Fixtures are generated with the ffmpeg
// CLI; the test exits 77 (skipped) when the CLI is unavailable.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "playback/PlaybackPipeline.h"

namespace {

using openshow::worker::PlaybackPipeline;
using openshow::worker::VideoSubmission;

bool check(bool ok, const char* message) {
    std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "ok" : "FAIL", message);
    return ok;
}

bool generate(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

// What the sink records: the submitted media pts and the decoder's
// sequence, so each scenario can inspect the tail after a control call.
struct Recorder {
    mutable std::mutex mutex;
    std::vector<double> pts;
    std::vector<uint64_t> sequences;
    bool sequencesMonotone = true;

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return pts.size();
    }
    double lastPts() const {
        std::lock_guard<std::mutex> lock(mutex);
        return pts.empty() ? -1.0 : pts.back();
    }
    std::vector<double> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return pts;
    }
};

// The sink plays the frontend's part: record, then hand the token back
// immediately, as a presenter that draws every frame would.
auto makeSink(PlaybackPipeline& pipeline, Recorder& rec) {
    return [&pipeline, &rec](const VideoSubmission& submission) {
        {
            std::lock_guard<std::mutex> lock(rec.mutex);
            rec.pts.push_back(submission.ptsSeconds);
            rec.sequences.push_back(submission.sequence);
            if (rec.sequences.size() > 1 &&
                submission.sequence <= rec.sequences[rec.sequences.size() - 2]) {
                rec.sequencesMonotone = false;
            }
        }
        pipeline.onPresented(submission.token, VK_NULL_HANDLE);
    };
}

// True when every step from `from` onwards rises, and the whole tail
// sits inside [lo, hi].
bool tailInRange(const std::vector<double>& values, size_t from, double lo, double hi) {
    if (values.size() <= from + 2) {
        return false;
    }
    for (size_t i = from; i < values.size(); ++i) {
        if (values[i] < lo || values[i] > hi) {
            return false;
        }
        if (i > from && values[i] <= values[i - 1]) {
            return false;
        }
    }
    return true;
}

// --- pause and resume ---

// Pause freezes presentation and position; resume continues from there —
// re-anchored, so no frame counts as missed across the gap.
bool pauseResume(const std::string& path) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    Recorder rec;
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "pause scenario opens (CPU mode)")) {
        std::fprintf(stderr, "  open: %s\n", error.c_str());
        return false;
    }
    if (!check(pipeline.position() < 0.0, "position is unknown before playback")) {
        return false;
    }
    if (!check(pipeline.start(makeSink(pipeline, rec), nullptr, &error), "playback starts")) {
        return false;
    }
    const bool flowing = waitFor([&] { return rec.size() >= 10; }, 8000);
    bool ok = check(flowing, "frames pace out before the pause");

    pipeline.pause();
    ok = check(pipeline.paused(), "the pipeline reports paused") && ok;
    const auto frozenPresents = pipeline.stats().submitted;
    const double frozenPosition = pipeline.position();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    ok = check(pipeline.stats().submitted == frozenPresents,
               "submissions are frozen while paused (the clock kept running)") && ok;
    ok = check(pipeline.position() == frozenPosition, "the position is frozen too") && ok;
    std::printf("ok: parked at submitted=%llu position=%.3fs\n",
                static_cast<unsigned long long>(frozenPresents), frozenPosition);

    pipeline.play();
    const bool resumed = waitFor([&] { return rec.size() >= frozenPresents + 5; }, 4000);
    ok = check(resumed, "presentation continues after play()") && ok;
    const auto stats = pipeline.stats();
    ok = check(stats.missed == 0,
               "resume re-anchored the clock: nothing presented late across the pause") && ok;
    ok = check(pipeline.position() > frozenPosition, "the position advanced again") && ok;
    ok = check(rec.sequencesMonotone, "submissions stayed in decode order") && ok;
    pipeline.requestStop();
    pipeline.join();
    return ok;
}

// --- forward seek ---

// A forward seek flushes the pre-target material: after the flush nothing
// from the old position presents, the frame near the target does, and the
// decode thread counted what it skipped.
bool forwardSeek(const std::string& path) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    Recorder rec;
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "forward-seek scenario opens")) {
        return false;
    }
    if (!pipeline.start(makeSink(pipeline, rec), nullptr, &error)) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }
    const bool flowing = waitFor([&] { return rec.lastPts() >= 0.25; }, 8000);
    bool ok = check(flowing, "playback is running before the seek");

    const size_t before = rec.size();
    pipeline.seekTo(1.5);
    const bool applied = waitFor([&] { return pipeline.stats().seeks >= 1; }, 4000);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    const auto stats = pipeline.stats();
    ok = check(applied && stats.seeks == 1, "the seek applied exactly once") && ok;
    ok = check(stats.seekDiscards >= 30,
               "frames short of the target were skipped before the ring (>=30)") && ok;
    const auto values = rec.snapshot();
    // One pre-seek submission can still be in flight when seekTo() is
    // called; everything after that one presents the new position.
    ok = check(tailInRange(values, before + 1, 1.45, 2.2),
               "after the flush every submission sits at the seek target") && ok;
    std::printf("ok: seek->1.5 applied, seeks=%llu seekDiscards=%llu tail=%zu\n",
                static_cast<unsigned long long>(stats.seeks),
                static_cast<unsigned long long>(stats.seekDiscards),
                values.size() > before ? values.size() - before : 0);
    pipeline.requestStop();
    pipeline.join();
    return ok;
}

// --- backward seek ---

// The regression this pins: a backward seek's pts drop must not read as
// a loop wrap — the effective pts continues from the target, never from
// target + clip length.
bool backwardSeek(const std::string& path) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;  // the 4 s clip: a wrap misread would land past 3.5 s
    Recorder rec;
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "backward-seek scenario opens")) {
        return false;
    }
    if (!pipeline.start(makeSink(pipeline, rec), nullptr, &error)) {
        return false;
    }
    const bool flowing = waitFor([&] { return rec.lastPts() >= 1.2; }, 8000);
    bool ok = check(flowing, "playback passed 1.2 s before seeking back");

    const size_t before = rec.size();
    pipeline.seekTo(0.5);
    const bool applied = waitFor([&] { return pipeline.stats().seeks >= 1; }, 4000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ok = check(applied, "the backward seek applied") && ok;
    const auto values = rec.snapshot();
    // With the loop-wrap misread, the tail would present at ~4.5 s.
    ok = check(tailInRange(values, before + 1, 0.45, 1.1),
               "after the backward seek media continues from the target, not target+clip") && ok;
    ok = check(pipeline.position() < 1.2, "the reported position moved back with the seek") && ok;
    ok = check(rec.sequencesMonotone, "the decoder's sequence stayed monotone across the seek") && ok;
    std::printf("ok: seek->0.5 applied, position=%.3fs\n", pipeline.position());
    pipeline.requestStop();
    pipeline.join();
    return ok;
}

// --- seek while paused ---

// The seek is accepted while parked; nothing presents until playback
// resumes and the flush has landed.
bool seekWhilePaused(const std::string& path) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    Recorder rec;
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "seek-while-paused scenario opens")) {
        return false;
    }
    if (!pipeline.start(makeSink(pipeline, rec), nullptr, &error)) {
        return false;
    }
    const bool flowing = waitFor([&] { return rec.size() >= 4; }, 8000);
    bool ok = check(flowing, "playback started before the pause");

    pipeline.pause();
    const auto frozen = pipeline.stats().submitted;
    const size_t before = rec.size();
    pipeline.seekTo(1.5);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ok = check(pipeline.stats().submitted == frozen,
               "nothing presents while parked, even with a seek queued") && ok;

    pipeline.play();
    const bool landed = waitFor([&] { return rec.size() >= before + 4; }, 4000);
    ok = check(landed, "presentation resumed after the parked seek") && ok;
    const auto stats = pipeline.stats();
    ok = check(stats.seeks >= 1, "the queued seek applied once playback moved again") && ok;
    const auto values = rec.snapshot();
    ok = check(tailInRange(values, before, 1.45, 1.9),
               "the first submission after resuming presents the seek target") && ok;
    pipeline.requestStop();
    pipeline.join();
    return ok;
}

// --- looping, then toggling it off ---

// With loop set, the effective pts climbs across the wrap without ever
// restarting; toggling loop off lets the second pass finish the clip.
bool loopThenOff(const std::string& path) {  // the 1 s clip
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    options.loop = true;
    Recorder rec;
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "loop scenario opens (loop on)")) {
        return false;
    }
    if (!pipeline.start(makeSink(pipeline, rec), nullptr, &error)) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }
    const bool wrapped = waitFor([&] { return pipeline.stats().loops >= 1; }, 5000);
    bool ok = check(wrapped, "the decoder rewound at EOF");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto values = rec.snapshot();
    bool monotoneAcrossWrap = !values.empty();
    for (size_t i = 1; i < values.size(); ++i) {
        monotoneAcrossWrap = monotoneAcrossWrap && values[i] > values[i - 1];
    }
    ok = check(monotoneAcrossWrap && values.back() > 1.0,
               "the effective pts kept climbing across the wrap") && ok;

    pipeline.setLoop(false);
    const bool finished = waitFor([&] { return pipeline.stats().eof; }, 5000);
    ok = check(finished, "with loop toggled off the clip ended at EOF") && ok;
    const auto stats = pipeline.stats();
    ok = check(stats.loops == 1 && stats.eof, "exactly one loop, then a clean finish") && ok;
    // The pacing thread still drains what the ring held; give it a beat,
    // then demand true silence past the end.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto stopCount = pipeline.stats().submitted;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ok = check(pipeline.stats().submitted == stopCount, "nothing presents past the end") && ok;
    ok = check(stats.ringPeakFilled <= stats.ringDepth && stats.decodeErrors == 0 &&
                   stats.fatalError.empty(),
               "the looped run stayed inside its bounds") && ok;
    std::printf("ok: loop wrap at pts>1.0, eof after toggle-off, final pts=%.3fs\n",
                pipeline.position());
    pipeline.requestStop();
    pipeline.join();
    return ok;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;

    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::printf("SKIP: the ffmpeg CLI is not available\n");
        return 77;
    }
    const fs::path dir = "playback-control-fixtures";
    std::error_code ec;
    fs::create_directories(dir, ec);

    // Default GOP: the only keyframe sits at 0, so every seek re-decodes
    // from the clip start and must skip forward — the skip counters make
    // the flush observable. Sizes follow the media_tests recipes.
    const std::string clip2s = (dir / "control-2s.mp4").string();
    const std::string clip4s = (dir / "control-4s.mp4").string();
    const std::string clipLoop = (dir / "control-loop-1s.mp4").string();
    if (!generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                  "-f lavfi -i testsrc2=size=320x240:rate=30:duration=2 "
                  "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -an " + clip2s) ||
        !generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                  "-f lavfi -i testsrc2=size=320x240:rate=25:duration=4 "
                  "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -an " + clip4s) ||
        !generate("ffmpeg -hide_banner -loglevel error -nostdin -y "
                  "-f lavfi -i testsrc2=size=320x240:rate=25:duration=1 "
                  "-c:v libx264 -preset ultrafast -pix_fmt yuv420p -an " + clipLoop)) {
        std::printf("SKIP: the ffmpeg CLI could not generate the fixtures\n");
        return 77;
    }

    bool ok = true;
    ok = pauseResume(clip2s) && ok;
    ok = forwardSeek(clip2s) && ok;
    ok = backwardSeek(clip4s) && ok;
    ok = seekWhilePaused(clip2s) && ok;
    ok = loopThenOff(clipLoop) && ok;
    return ok ? 0 : 1;
}
