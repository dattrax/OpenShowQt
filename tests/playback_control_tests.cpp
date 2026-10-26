// Task 5.2 acceptance: playback transitions and seek flushing pass tests.
// The PlaybackPipeline's controls — play/pause, seek, loop toggle — run
// through the real seams (the decode thread applies seeks between frames,
// the pacing thread flushes and re-anchors), driven headless in CPU mode
// against the default wall clock — and 6.7d's two time domains against a
// clock the scenario owns. Fixtures are generated with the ffmpeg
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

// A parked transport SCRUBS (task 6.7d, review R19): the seek it is given
// presents its target and then the transport holds again. Three reasons make
// this the semantic rather than the older "wait for the resume": the clock a
// held transport paces against is frozen, so the only deadline a new frame can
// have is "now"; changing what the parked picture shows is the whole point of
// seeking while paused (a scrub bar); and the offer is the seam's own way out
// of the hold — it is what releases the presentation slot a producer may be
// waiting on, so a parked transport can always digest a seek instead of
// stopping. What must NOT happen is the hold turning into a playback.
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
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto frozen = pipeline.stats().submitted;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ok = check(pipeline.stats().submitted == frozen,
               "a held transport presents nothing while nothing changes") && ok;

    const double latched = pipeline.position();
    const size_t before = rec.size();
    pipeline.seekTo(1.5);
    const bool scrubbed = waitFor([&] { return pipeline.stats().submitted > frozen; }, 4000);
    ok = check(scrubbed,
               "a seek issued while held is not swallowed: the parked picture scrubs to it") && ok;
    ok = check(latched < 1.45, "the hold latched a position the scrub had to leave behind") && ok;
    const auto scrubValues = rec.snapshot();
    ok = check(scrubValues.size() > before && scrubValues[before] >= 1.45 &&
                   scrubValues[before] < 2.2,
               "the scrubbed frame is the TARGET, not the position the hold latched") && ok;
    ok = check(pipeline.position() >= 1.45 && pipeline.position() < 2.2,
               "the reported position scrubs while the transport stays held") && ok;
    const auto afterScrub = pipeline.stats().submitted;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ok = check(pipeline.stats().submitted == afterScrub,
               "scrubbing is a picture, not a resume: the transport holds again") && ok;
    const size_t scrubbedCount = rec.size();

    pipeline.play();
    const bool landed = waitFor([&] { return rec.size() >= scrubbedCount + 4; }, 4000);
    ok = check(landed, "presentation resumed after the parked seek") && ok;
    ok = check(pipeline.stats().seeks >= 1, "the seek applied while the transport was parked") && ok;
    const auto values = rec.snapshot();
    ok = check(tailInRange(values, scrubbedCount, 1.45, 3.5),
               "play resumes FROM the scrubbed target, not from the latched position") && ok;
    std::printf("ok: parked seek scrubbed %.2f -> %.2f, resumed there\n", latched,
                pipeline.position());
    pipeline.requestStop();
    pipeline.join();
    return ok;
}

// The swallow window (task 6.7d, review R19). A request that arrives while the
// decode thread is performing an earlier one must STILL be performed: the
// queue keeps it pending because the applier credited only the pair it was
// handed. Crediting `requested()` instead — the newest count, whatever target
// goes with it — settles the newer request unperformed: the pacing thread
// reads "flush over", presents the superseded position, and the caller is told
// a jump happened that the media never made.
//
// One pair hits that window only by luck; it is a few hundred microseconds
// wide. So the scenario fires the same shape over and over with jittered
// spacing and requires EVERY round to end at its newest target. A mispaired
// credit cannot hide: it leaves the transport a couple of seconds short, and
// the round's deadline is far tighter than the time from the superseded
// position to the target.
bool seekStormKeepsTheNewest(const std::string& path) {  // the 4 s clip
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    Recorder rec;
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "seek-storm scenario opens")) {
        return false;
    }
    if (!pipeline.start(makeSink(pipeline, rec), nullptr, &error)) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }
    bool ok = check(waitFor([&] { return rec.lastPts() >= 0.2; }, 8000),
                    "playback is running before the storm");

    constexpr int kRounds = 40;
    constexpr double kNear = 0.2;   // the request a swallowed newer target hides
    constexpr double kNew = 2.6;     // what the audience asked for, and proves
    int missed = 0;
    for (int round = 0; round < kRounds; ++round) {
        pipeline.seekTo(kNear);
        // Space the newest request from 0 to ~1.5 ms after the first: close
        // enough that some of them land inside the first one's apply, so the
        // pairing rule — not luck — decides whether the round passes.
        std::this_thread::sleep_for(std::chrono::microseconds(20 + round * 40));
        pipeline.seekTo(kNew);
        // From the superseded 0.2 s the target is 2.4 s away: a round that
        // never performed its newest request cannot reach it in this window,
        // while a performed one presents it in tens of milliseconds.
        if (!waitFor([&] { return rec.lastPts() >= kNew - 0.2; }, 1200)) {
            ++missed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30 + (round % 5) * 17));
    }
    ok = check(missed == 0,
               "every overlapping pair of seeks performs its NEWEST target (nothing swallowed)") && ok;
    const auto stats = pipeline.stats();
    ok = check(stats.seeks >= kRounds && stats.fatalError.empty(),
               "the storm applied every seek it was asked for and stayed healthy") && ok;
    std::printf("ok: %d overlapping seek rounds, %d missed, seeks=%llu discards=%llu\n", kRounds,
                missed, static_cast<unsigned long long>(stats.seeks),
                static_cast<unsigned long long>(stats.seekDiscards));
    pipeline.requestStop();
    pipeline.join();
    return ok;
}

// --- the two time domains (task 6.7d, review R19) ---

// A transport clock the scenario controls: a base in media seconds plus wall
// time since the anchor, which is exactly what AvPlayer's fallback clock is
// and does on a seek. The pair is guarded as one value, so a reader can never
// combine a new base with an old anchor. (Handed to the pipeline as a lambda
// over a reference: the mutex makes the object itself non-copyable.)
class TestClock {
public:
    explicit TestClock(double startSeconds) { anchor(startSeconds); }
    void anchor(double seconds) {
        std::lock_guard<std::mutex> lock(fMutex);
        fBaseSeconds = seconds;
        fBaseNs = std::chrono::steady_clock::now();
    }
    double operator()() const {
        std::lock_guard<std::mutex> lock(fMutex);
        return fBaseSeconds +
               std::chrono::duration<double>(std::chrono::steady_clock::now() - fBaseNs).count();
    }

private:
    mutable std::mutex fMutex;
    double fBaseSeconds = 0.0;
    std::chrono::steady_clock::time_point fBaseNs{};
};

// The submission log R19 needs: BOTH times of every frame plus the clock as
// the submission arrived, so "which field lives in which domain" is a
// measurement rather than a reading of the code.
struct DomainLog {
    struct Row {
        double pts = 0.0;
        double deadline = 0.0;
        double clock = 0.0;
    };
    mutable std::mutex mutex;
    std::vector<Row> rows;

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return rows.size();
    }
    std::vector<Row> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return rows;
    }
};

auto makeDomainSink(PlaybackPipeline& pipeline, DomainLog& log, const TestClock& clock) {
    return [&pipeline, &log, &clock](const VideoSubmission& submission) {
        {
            std::lock_guard<std::mutex> lock(log.mutex);
            log.rows.push_back(
                {submission.ptsSeconds, submission.deadlineSeconds, clock()});
        }
        pipeline.onPresented(submission.token, VK_NULL_HANDLE);
    };
}

// R19's first complaint, stated as data: the pacing layer computes a
// re-anchored deadline and the session compares a field against the transport
// clock, so ONE of the two must be in the clock's domain. The scenario starts
// where every transport starts (clock 0 at pts 0, the two domains agreeing),
// then does the one thing that pulls them apart — a hold, which the wall clock
// spends — and demands that the frames after it carry the media position in
// one field and the clock's own present time in the other. A field that mixes
// them shows up as a 500-second outlier.
bool twoTimeDomains(const std::string& path) {
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    DomainLog log;
    TestClock clock(0.0);
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "two-domain scenario opens")) {
        return false;
    }
    auto sink = makeDomainSink(pipeline, log, clock);
    if (!check(pipeline.start(sink, [&clock] { return clock(); }, &error),
               "two-domain playback starts")) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }
    const bool flowing = waitFor([&] { return log.size() >= 6; }, 8000);
    bool ok = check(flowing, "frames are submitted against the injected clock");
    const size_t aligned = log.size();
    bool alignedRows = true;
    for (const auto& row : log.snapshot()) {
        alignedRows = alignedRows && std::fabs(row.deadline - row.pts) < 0.2;
    }
    ok = check(alignedRows,
               "at the start the two domains agree: the clock began with the media") && ok;

    // The hold is what separates them: the clock runs, the media does not, and
    // the resume re-anchors the deadline on the clock's present time. The
    // pacing thread has to be PARKED before the clock moves, or it never sees
    // the hold it is supposed to re-anchor on.
    pipeline.pause();
    const bool parked = waitFor([&] { return pipeline.paused(); }, 2000);
    ok = check(parked, "the pacing thread parked on the hold") && ok;
    const auto frozenSubmits = pipeline.stats().submitted;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ok = check(pipeline.stats().submitted == frozenSubmits,
               "nothing presents while parked (the clock is the only thing moving)") && ok;
    clock.anchor(clock() + 500.0);
    const size_t beforeOffset = log.size();
    pipeline.play();
    const bool resumed = waitFor([&] { return log.size() >= beforeOffset + 8; }, 8000);
    ok = check(resumed, "presentation resumed on the re-anchored clock") && ok;

    const auto rows = log.snapshot();
    bool ptsIsMedia = beforeOffset + 1 < rows.size();
    bool deadlineIsClock = ptsIsMedia;
    bool deadlinesMonotone = true;
    bool someFuture = false;
    double minAhead = 1e9;
    double maxAhead = -1e9;
    for (size_t i = beforeOffset + 1; i < rows.size(); ++i) {
        ptsIsMedia = ptsIsMedia && rows[i].pts >= 0.0 && rows[i].pts < 10.0;
        deadlineIsClock =
            deadlineIsClock && rows[i].deadline > 400.0 && rows[i].deadline < 700.0;
        const double ahead = rows[i].deadline - rows[i].clock;
        minAhead = std::min(minAhead, ahead);
        maxAhead = std::max(maxAhead, ahead);
        someFuture = someFuture || ahead > 0.002;
        deadlinesMonotone = deadlinesMonotone && rows[i].deadline >= rows[i - 1].deadline;
    }
    ok = check(ptsIsMedia, "ptsSeconds stays the MEDIA position — the hold is not in it") && ok;
    ok = check(deadlineIsClock, "deadlineSeconds lives in the transport clock's domain") && ok;
    ok = check(minAhead > -0.25 && maxAhead < 0.25,
               "the deadline is the near present time the pacer chose for the frame") && ok;
    ok = check(deadlinesMonotone, "deadlines climb with the clock") && ok;
    ok = check(someFuture, "at least one frame was genuinely future (so the gate does work)")
           && ok;
    ok = check(pipeline.position() >= 0.0 && pipeline.position() < 10.0,
               "the reported position is media time, not the clock") && ok;
    // The shared gate, exercised on the recorded rows: due means the clock
    // reached the deadline — and the media pts is 500 s behind this clock, so
    // the comparison the session used to make would have called every frame of
    // this run overdue before it existed.
    bool gateFollowsDeadline = beforeOffset + 1 < rows.size();
    bool ptsWouldPresentEverything = true;
    for (size_t i = beforeOffset + 1; i < rows.size(); ++i) {
        const VideoSubmission sub{0u, rows[i].pts, rows[i].deadline, 0ull};
        gateFollowsDeadline = gateFollowsDeadline &&
                              !videoSubmissionDue(sub, rows[i].deadline - 0.05) &&
                              videoSubmissionDue(sub, rows[i].deadline + 0.05);
        ptsWouldPresentEverything = ptsWouldPresentEverything && rows[i].clock > rows[i].pts;
    }
    ok = check(gateFollowsDeadline, "videoSubmissionDue turns on the deadline and nothing else")
           && ok;
    ok = check(ptsWouldPresentEverything,
               "this clock has passed every media pts — comparing those would present early") && ok;
    std::printf("ok: domains — %zu aligned rows, then pts %.2f..%.2f s against deadline %.1f..%.1f s "
                "(deadline-clock %.3f..%.3f s)\n",
                aligned, rows.empty() ? 0.0 : rows.front().pts,
                rows.empty() ? 0.0 : rows.back().pts,
                rows.empty() ? 0.0 : rows.front().deadline,
                rows.empty() ? 0.0 : rows.back().deadline, minAhead, maxAhead);
    pipeline.requestStop();
    pipeline.join();
    return ok;
}

// --- backward seek after a wrap ---

// R19's freeze, in one transport: the clip has looped, so pts continuity runs
// past the clip length; the operator then seeks back, and the transport clock
// re-anchors on the target. If the wrap base survives in the media position,
// the picture sits in the session's pending slot until the clock climbs the
// whole clip length to reach it — the audience watches the old frame while
// the audio plays from the target.
bool seekAfterWrap(const std::string& path) {  // the 1 s clip, looping
    PlaybackPipeline pipeline;
    PlaybackPipeline::Options options;
    options.path = path;
    options.loop = true;
    DomainLog log;
    TestClock clock(0.0);
    std::string error;
    if (!check(pipeline.openCpu(options, &error), "seek-after-wrap scenario opens")) {
        return false;
    }
    auto sink = makeDomainSink(pipeline, log, clock);
    if (!check(pipeline.start(sink, [&clock] { return clock(); }, &error), "seek-after-wrap playback starts")) {
        std::fprintf(stderr, "  start: %s\n", error.c_str());
        return false;
    }
    const bool wrapped =
        waitFor([&] { return pipeline.stats().loops >= 1 && pipeline.position() > 1.15; }, 6000);
    bool ok = check(wrapped, "the clip looped once, so the continuity base is a clip long");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // The transport's own move: fan the seek out, then re-anchor the clock on
    // the target, as AvPlayer::seekTo does when the audio has landed.
    const size_t before = log.size();
    const auto submittedBefore = pipeline.stats().submitted;
    pipeline.seekTo(0.3);
    clock.anchor(0.3);
    const bool resumed = waitFor([&] { return log.size() >= before + 4; }, 5000);
    ok = check(resumed, "presentation resumed after the backward seek across the wrap") && ok;
    const auto rows = log.snapshot();
    bool ptsAtTarget = true;
    bool deadlineAtClock = true;
    bool heldByClockAgainstPts = false;
    double minPts = 1e9;
    double maxPts = -1e9;
    // One pre-seek submission can still be in flight when seekTo() lands.
    for (size_t i = before + 1; i < rows.size(); ++i) {
        ptsAtTarget = ptsAtTarget && rows[i].pts >= 0.25 && rows[i].pts <= 0.95;
        const double ahead = rows[i].deadline - rows[i].clock;
        deadlineAtClock = deadlineAtClock && ahead > -0.25 && ahead < 0.35;
        // A pacer is allowed to be a frame interval early. It is not allowed
        // to be a CLIP late: that is the freeze, and the audience cannot see
        // the frame until the clock catches up.
        heldByClockAgainstPts = heldByClockAgainstPts || rows[i].pts - rows[i].clock > 0.5;
        minPts = std::min(minPts, rows[i].pts);
        maxPts = std::max(maxPts, rows[i].pts);
    }
    ok = check(ptsAtTarget,
               "the target presents — the wrap base does not carry across a seek") && ok;
    ok = check(deadlineAtClock,
               "its deadline sits on the re-anchored clock, so the session draws it now") && ok;
    // The freeze condition itself, stated the way the session used to test it.
    ok = check(!heldByClockAgainstPts,
               "no frame after the seek waits a clip on a clock-versus-pts comparison") && ok;
    const double position = pipeline.position();
    ok = check(position >= 0.3 && position < 1.0,
               "the reported position is the target, not target plus the clip") && ok;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ok = check(pipeline.stats().submitted > submittedBefore + 3,
               "the transport keeps submitting after the seek") && ok;
    std::printf("ok: seek->0.3 after a wrap presents pts %.3f..%.3f s, position %.3f s\n", minPts,
                maxPts, position);
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
    ok = seekStormKeepsTheNewest(clip4s) && ok;
    ok = twoTimeDomains(clip2s) && ok;
    ok = seekAfterWrap(clipLoop) && ok;
    ok = loopThenOff(clipLoop) && ok;
    return ok ? 0 : 1;
}
