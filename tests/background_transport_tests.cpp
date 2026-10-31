/*
 * The audience-side media's owner (task 6.18b): BackgroundTransport's rules
 * pinned headless against the real object — the still-background fault
 * reporting and its deduplication, the identity rule that keeps a re-present
 * of the same asset silent, the standalone request's probe-before-accept
 * refusals, the gain/mute commandability gates, the scheduled-op grammar,
 * and the video seam's bounded two slots: the due gate in the clock's own
 * domain, the held frame a dirty redraw keeps drawing, the drop of a
 * replaced-before-drawn submission with its sequence as proof, and the
 * idempotent shutdown.
 *
 * No decoding policy lives here: opening a video transport belongs to the
 * live fixtures (background_media_tests owns the real bring-up, the
 * fault-injection scripts and the A/V continuity rules against a real
 * present). What lives here is everything the transport decides WITHOUT
 * decoding — the policy the extraction moved — driven through the same
 * public surface the session drives it through, which is exactly what R10
 * asked production and bench to share. The one exception is the 6.22c
 * retirement fixture: review A3's promise is about real audio delivery
 * during a real slow retirement, which nothing but a real device, a real
 * PipeWire stream and a real clip can measure; it skips cleanly with
 * whatever is missing.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>

#include "include/core/SkImage.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"

#include "media/PipeWireSink.h"
#include "output/VulkanContext.h"
#include "session/BackgroundTransport.h"

namespace {

using openshow::worker::AudioSource;
using openshow::worker::BackgroundTransport;
using openshow::worker::MediaFault;
using openshow::worker::VideoPresenter;
using openshow::worker::VideoSubmission;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

// The seam's other half: counts the presenter calls the transport makes,
// and answers acquireImage with nothing (no GPU in this fixture — the
// session-side acquire of a real slot belongs to the live gates).
class FakePresenter : public VideoPresenter {
public:
    bool checkClockEpoch = false;
    uint64_t clockEpoch = 0;
    bool acceptsClockEpoch(uint64_t epoch) const override {
        return !checkClockEpoch || (!(epoch & 1) && epoch == clockEpoch);
    }
    SkImage* acquireImage(unsigned, uint64_t) override { return nullptr; }
    void onPresented(unsigned token, VkSemaphore) override {
        ++presented;
        lastPresentedToken = token;
    }
    void onReplaced(unsigned token, uint64_t sequence) override {
        ++replaced;
        lastReplacedToken = token;
        lastReplacedSequence = sequence;
    }
    void onDropped(unsigned token, uint64_t sequence) override {
        ++dropped;
        lastDroppedToken = token;
        lastDroppedSequence = sequence;
    }
    int presented = 0;
    int replaced = 0;
    int dropped = 0;
    unsigned lastPresentedToken = 0;
    unsigned lastReplacedToken = 0;
    unsigned lastDroppedToken = 0;
    uint64_t lastReplacedSequence = 0;
    uint64_t lastDroppedSequence = 0;
};

openshow::scene::Background stillBackground(const std::string& path) {
    openshow::scene::Background background;
    background.hasImage = true;
    background.isVideo = false;
    background.assetId = path;
    background.assetPath = path;
    return background;
}

openshow::scene::Background videoBackground(const std::string& path) {
    openshow::scene::Background background;
    background.hasImage = true;
    background.isVideo = true;
    background.assetId = path;
    background.assetPath = path;
    return background;
}

const auto kEpoch = std::chrono::steady_clock::now();

void testIdle(BackgroundTransport& transport) {
    // A transport that never brought anything up: idle status, nothing
    // commandable, no wake owed.
    const openshow::worker::MediaStatus media = transport.mediaStatus();
    check(!media.playing && !media.live, "idle: no transport, nothing commandable");
    check(media.position == -1.0 && media.duration == -1.0, "idle: position and duration unset");
    check(media.audio == "none" && media.sources.empty(), "idle: no lane, no sources");
    check(media.standalone.live == false && media.standalone.audio == "none",
          "idle: the standalone block is idle too");
    check(!transport.mediaTransportLive() && !transport.standaloneLive(),
          "idle: neither transport answers live");
    const BackgroundTransport::VideoWake wake = transport.submissionWake(kEpoch);
    check(!wake.pending && !wake.due, "idle: no submission, no wake owed");
}

void testRefusals(BackgroundTransport& transport) {
    // The operator cannot address what is not there: both identities refuse
    // with the truth, and the request changes nothing (R15's up-front
    // answer — a refusal the caller hears, not an op that dies unheard).
    std::string error;
    check(!transport.requestMediaGain(AudioSource::kVideo, 0.5, &error),
          "gain: refused with no background transport");
    check(error == "no background media is playing", "gain: the refusal says why");
    check(!transport.requestMediaMute(AudioSource::kStandalone, true, &error),
          "mute: refused with no standalone track");
    check(error == "no standalone track is playing", "mute: the refusal says why");
    check(!transport.mediaSourceLive(AudioSource::kVideo) &&
              !transport.mediaSourceLive(AudioSource::kStandalone),
          "no identities registered");
    // A command with no transport still rides its newest-wins slot and the
    // loop's pass drops it honestly (the op-log line, no crash, no change).
    transport.requestMediaSeek(3.0);
    transport.requestMediaPause();
    transport.applyTransportOps();
    check(!transport.mediaTransportLive(), "ops with no transport change nothing");
}

void testStandaloneProbe(BackgroundTransport& transport) {
    // Probe before accept (6.15b): a file that is not decodable audio is a
    // REFUSED request that changes nothing; "" is a removal and always
    // accepts.
    std::string error;
    check(!transport.requestStandaloneAudio("/nonexistent/track.flac", &error),
          "standalone: an unreadable path is refused");
    check(!error.empty(), "standalone: the refusal names a reason");
    check(!transport.standaloneLive(), "standalone: refusal started nothing");
    check(transport.requestStandaloneAudio("", &error),
          "standalone: a removal always accepts");
}

// A queued deadline from before a clock rebase must release its lease,
// preserve the displayed picture, and never sleep against the new clock.
void testClockEpochDropsUndrawnDeadlines() {
    BackgroundTransport transport;
    FakePresenter presenter;
    presenter.checkClockEpoch = true;
    transport.setVideoPresenter(&presenter);
    transport.noteRunStart(kEpoch);
    transport.setVideoClock([] { return 1.0; });
    VideoSubmission first{1, 0.0, 0.0, 11, 0};
    transport.submitVideo(first);
    check(transport.takeDueVideo(kEpoch).toDraw.has_value(), "epoch: first frame is displayed");
    transport.submitVideo(VideoSubmission{2, 1.0, 50.0, 22, 0});
    presenter.clockEpoch = 1;
    check(transport.submissionWake(kEpoch).due, "epoch: obsolete future deadline wakes for disposal");
    const auto turn = transport.takeDueVideo(kEpoch);
    check(turn.dropped && turn.dropped->sequence == 22 && !turn.toDraw,
          "epoch: pending old deadline is returned with its release proof");
    check(transport.heldVideo() && transport.heldVideo()->sequence == 11,
          "epoch: the audience retains its held picture during the seek");
    transport.submitVideo(VideoSubmission{3, 2.0, 0.0, 33, 0});
    check(presenter.dropped == 1 && presenter.lastDroppedSequence == 33,
          "epoch: a late in-flight submission is rejected while clock seek is pending");
    presenter.clockEpoch = 2;
    transport.submitVideo(VideoSubmission{4, 2.0, 0.0, 44, 0});
    check(presenter.dropped == 2, "epoch: old clock deadlines stay invalid after completion");
    transport.submitVideo(VideoSubmission{5, 0.2, 0.2, 55, 2});
    check(transport.takeDueVideo(kEpoch).toDraw.has_value(), "epoch: completed clock accepts its own frames");
    transport.setVideoPresenter(nullptr);
}

void testStandaloneIntentGeneration() {
    BackgroundTransport transport;
    const auto older = transport.beginStandaloneIntent();
    const auto removal = transport.beginStandaloneIntent();
    check(transport.armStandaloneTrack("", removal), "intent: newest removal arms");
    check(!transport.armStandaloneTrack("unused-older-track", older),
          "intent: old validation cannot replace the removal");
    const auto latest = transport.beginStandaloneIntent();
    check(!transport.armStandaloneTrack("", removal), "intent: even removal carries its generation");
    check(transport.armStandaloneTrack("", latest), "intent: latest request retains ownership");
    transport.applyTransportOps();
    check(!transport.standaloneLive(), "intent: stale work starts no player");
}

void testStillsAndIdentity(BackgroundTransport& transport) {
    // The broken still (R18): the picture the audience should be seeing is
    // not in the cache, and the colour fill they get is a degradation the
    // operator must SEE — NAMED, once, paired with its path.
    FakePresenter presenter;
    transport.setVideoPresenter(&presenter);
    const std::string path = "/media/broken-still.png";
    bool started = (transport.applyBackground(stillBackground(path),
                                             openshow::worker::GpuHandles{},
                                             [](const std::string&) { return false; }) == BackgroundTransport::BackgroundApply::kVideoStarted);
    check(!started, "still: no video transport, no present owed");
    openshow::worker::MediaStatus media = transport.mediaStatus();
    check(media.path == path, "still: media.path names the asset on air");
    check(media.error.find("not in the decoded cache") != std::string::npos,
          "still: the degradation is NAMED");
    openshow::worker::MediaError reported;
    check(transport.takeMediaError(&reported), "still: the failure rides media.error");
    check(reported.path == path && reported.message == media.error,
          "still: the notification and status tell the same story");
    check(!transport.takeMediaError(&reported), "still: nothing queued twice");

    // Re-present the SAME asset: the alreadyReported gate must not notify
    // the same degradation again, and the sticky error stays paired.
    started = (transport.applyBackground(stillBackground(path), openshow::worker::GpuHandles{},
                                        [](const std::string&) { return false; }) == BackgroundTransport::BackgroundApply::kVideoStarted);
    check(!started, "still: a re-present still starts nothing");
    check(!transport.takeMediaError(&reported),
          "still: the same broken asset does not notify twice");
    check(transport.mediaStatus().error == media.error, "still: the sticky error stays");

    // The asset repaired: the same prepare that decodes it clears the
    // error (the recovery clears its own fault).
    (transport.applyBackground(stillBackground(path), openshow::worker::GpuHandles{},
                              [](const std::string&) { return true; }) == BackgroundTransport::BackgroundApply::kVideoStarted);
    media = transport.mediaStatus();
    check(media.error.empty(), "still: a decoded asset clears the error");
    check(media.path == path, "still: the healthy asset is still NAMED on air");

    // A plain colour fill: no asset, no story.
    (transport.applyBackground(openshow::scene::Background{}, openshow::worker::GpuHandles{},
                              [](const std::string&) { return false; }) == BackgroundTransport::BackgroundApply::kVideoStarted);
    media = transport.mediaStatus();
    check(media.path.empty() && media.error.empty(),
          "colour fill: no asset on air means nothing to report");
    transport.setVideoPresenter(nullptr);
}

void testSeam(BackgroundTransport& transport) {
    // The bounded two slots, the deadline gate in the clock's domain (R19)
    // and the lease handoffs — against a fake clock the test walks, with
    // the fake presenter proving which generation each release owns.
    FakePresenter presenter;
    transport.setVideoPresenter(&presenter);
    double now = 0.0;
    transport.setVideoClock([&now] { return now; });
    transport.noteRunStart(kEpoch);

    VideoSubmission first;
    first.token = 7;
    first.sequence = 1;
    first.deadlineSeconds = 1.0;
    first.ptsSeconds = 1.0;
    transport.submitVideo(first);

    BackgroundTransport::VideoWake wake = transport.submissionWake(kEpoch);
    check(wake.pending && !wake.due, "seam: an early slot is pending, not due");
    check(wake.sleepSeconds > 0.4 && wake.sleepSeconds < 1.01,
          "seam: the wake answer is the remaining time, never a spin");

    BackgroundTransport::VideoTurnover turn = transport.takeDueVideo(kEpoch);
    check(!turn.toDraw && !turn.replaced, "seam: nothing consumed before its deadline");
    check(!transport.heldVideo(), "seam: nothing held until a frame is drawn");

    now = 1.5;
    turn = transport.takeDueVideo(kEpoch);
    check(turn.toDraw && turn.toDraw->token == 7 && !turn.replaced,
          "seam: the due frame becomes the drawn one");
    check(presenter.replaced == 0, "seam: replacing nothing calls onReplaced zero times");
    const std::optional<VideoSubmission> held = transport.heldVideo();
    check(held && held->token == 7, "seam: the drawn frame is the static hold");
    turn = transport.takeDueVideo(kEpoch);
    check(!turn.toDraw && transport.heldVideo()->token == 7,
          "seam: a second decision holds the same frame");

    // The acquire-failure drop (the session relinquished it via
    // onReplaced; the transport forgets only the token named).
    transport.dropHeldVideo(8);
    check(transport.heldVideo().has_value(), "seam: a drop for another token changes nothing");
    transport.dropHeldVideo(7);
    check(!transport.heldVideo(), "seam: the named token stops being sampled");

    // Replaced-before-drawn: the older pending submission goes back with
    // ITS sequence as proof (6.6a), no GPU obligation, and only the newest
    // one survives.
    VideoSubmission second;
    second.token = 9;
    second.sequence = 2;
    second.deadlineSeconds = 2.0;
    transport.submitVideo(second);
    VideoSubmission third;
    third.token = 11;
    third.sequence = 3;
    third.deadlineSeconds = 2.5;
    transport.submitVideo(third);
    check(presenter.dropped == 1 && presenter.lastDroppedToken == 9 &&
              presenter.lastDroppedSequence == 2,
          "seam: the replaced-before-drawn slot is returned, named exactly");
    now = 2.75;
    turn = transport.takeDueVideo(kEpoch);
    check(turn.toDraw && turn.toDraw->token == 11, "seam: the newest pending wins");

    // Without a presenter the seam owes no wake: the loop may sleep while
    // a stale slot waits for a transport that is gone (the teardown
    // clears both, and a hand-set null must not make the loop spin).
    transport.setVideoPresenter(nullptr);
    wake = transport.submissionWake(kEpoch);
    check(!wake.pending, "seam: no presenter means no wake owed");
    transport.setVideoClock(nullptr);
}

void testScheduleGrammar() {
    // The test script R19 runs through the real seam; a malformed entry is
    // reported and SKIPPED, never half-parsed.
    const auto ops = BackgroundTransport::parseScheduledOps(
        "seek@900:0.6,pause@2400,play@3200");
    check(ops.size() == 3, "schedule: three ops parse");
    check(ops[0].kind == BackgroundTransport::kOpSeek && ops[0].atMs == 900 &&
              ops[0].seconds > 0.59 && ops[0].seconds < 0.61,
          "schedule: a seek carries its time and target");
    check(ops[1].kind == BackgroundTransport::kOpPause && ops[1].atMs == 2400,
          "schedule: a pause carries its time");
    check(ops[2].kind == BackgroundTransport::kOpPlay && ops[2].atMs == 3200,
          "schedule: a play carries its time");
    check(BackgroundTransport::parseScheduledOps("nope@100").empty(),
          "schedule: an unknown kind is skipped");
    check(BackgroundTransport::parseScheduledOps("seek@abc").empty(),
          "schedule: a non-numeric time is skipped");
    check(BackgroundTransport::parseScheduledOps("seek").empty(),
          "schedule: an op without a time is skipped");
    check(BackgroundTransport::parseScheduledOps("").empty(),
          "schedule: the empty script parses empty");
}

void testShutdown(BackgroundTransport& transport) {
    // Shutdown is the way down and it is idempotent: the destructor may
    // run it after close() did, and a never-started transport tears down
    // as cleanly as a running one.
    FakePresenter presenter;
    transport.setVideoPresenter(&presenter);
    VideoSubmission pending;
    pending.token = 21;
    pending.sequence = 4;
    pending.deadlineSeconds = 0.0;
    transport.submitVideo(pending);
    transport.shutdown();
    transport.shutdown();
    const openshow::worker::MediaStatus media = transport.mediaStatus();
    check(!media.playing && !media.live, "shutdown: the background block is idle");
    check(!media.standalone.live && media.standalone.audio == "none",
          "shutdown: the standalone block is ended by name");
    check(!transport.mediaTransportLive() && !transport.standaloneLive(),
          "shutdown: neither transport answers live");
    check(!transport.heldVideo(), "shutdown: the seam forgets its slots");
}

/* --- task 6.22c (review A3): retired audio goes silent at the hand-off ----
   A3: teardown handed the OLD pipeline to the retirement lane and only
   stopped its audio player AFTER joining the pacing thread — so for as long
   as the exit took, the retired clip's mixer kept feeding a live PipeWire
   stream, and the audience heard a background that status had already
   removed. The contract: the hand-off establishes the outgoing audio's
   intended state without waiting for the join; the lane owes destruction,
   never audible lifetime.

   Proving that needs three things no stand-in can fake together: a clip
   whose audio is REALLY delivered (a real sink, real server consumption),
   a retirement whose join is REALLY slow (6.21c2's scripted pacing exit),
   and a view INTO the in-flight retirement (retiringAudioDelivery) — the
   status mirror reports the fresh silent instance, so it cannot tell
   silent-because-handed-off from silent-because-destroyed. The fixture
   skips with whatever of ffmpeg, the PipeWire daemon, or a headless Vulkan
   device is missing. */
void testRetiredAudioGoesSilentAtHandoff() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::printf("   skipped the retired-audio handoff (no ffmpeg)\n");
        return;
    }
    openshow::media::PipeWireSink probe;
    std::string probeError;
    if (!probe.start(48000, &probeError)) {
        std::printf("   skipped the retired-audio handoff (no PipeWire daemon: %s)\n",
                    probeError.c_str());
        return;
    }
    probe.stop();
    openshow::worker::VulkanContext vk;
    if (!vk.createHeadless()) {
        std::printf("   skipped the retired-audio handoff (no headless Vulkan device)\n");
        return;
    }
    auto gr = GrDirectContexts::MakeVulkan(vk.backendContext());
    if (!gr) {
        std::printf("   skipped the retired-audio handoff (no Ganesh context)\n");
        return;
    }

    std::filesystem::create_directories("transport-fixtures");
    const std::string clip = "transport-fixtures/retire-audio-a.mp4";
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -nostdin -y "
        "-f lavfi -i nullsrc=s=160x96:r=12 "
        "-f lavfi -i sine=frequency=440:sample_rate=48000 "
        "-t 2 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac " + clip;
    if (std::system(command.c_str()) != 0) {
        check(false, "the audio-bearing fixture clip is generated");
        return;
    }

    // Script the slow exit BEFORE the transport opens: 6.21c2's pacing seam
    // sleeps 700 ms when the named clip's pacing thread leaves its loop —
    // a window during which the retirement lane is provably still waiting.
    // The match name is fixture-specific, so no other pipeline in this
    // binary trips it; the environment is cleared again at the end.
    setenv("OPENSHOW_FAULT_PACING_EXIT_MS", "700", 1);
    setenv("OPENSHOW_FAULT_PACING_EXIT_MATCH", "retire-audio-a.mp4", 1);

    BackgroundTransport transport;
    transport.noteRunStart(kEpoch);
    openshow::worker::GpuHandles handles;
    handles.device = vk.device();
    handles.physicalDevice = vk.physicalDevice();
    handles.graphicsQueueIndex = vk.queueIndex();
    handles.uploadQueueIndex = vk.uploadQueueIndex();
    handles.uploadQueue = vk.uploadQueue();
    handles.context = gr.get();
    const auto resident = [](const std::string&) { return true; };

    check((transport.applyBackground(videoBackground(clip), handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted),
          "the audio-bearing background transport starts");

    // Audible first: the lane reads live and the position climbs (the same
    // proof status gives the operator — without it the delivery check below
    // has nothing to silence).
    bool audible = false;
    for (int attempt = 0; attempt < 100 && !audible; ++attempt) {
        transport.refreshMediaState();
        const openshow::worker::MediaStatus media = transport.mediaStatus();
        audible = media.playing && media.audio == "live" && media.position > 0.05;
        if (!audible) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    check(audible, "the old clip's audio is really being delivered");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // consumption owns the clock

    // The clear. The hand-off must not pay the join: the scripted exit
    // sleeps 700 ms, and applyBackground returning in milliseconds is half
    // of what A3 asks for.
    const auto began = std::chrono::steady_clock::now();
    (transport.applyBackground(openshow::scene::Background{}, handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted);
    const double handoffMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - began)
                                 .count();
    check(handoffMs < 150.0, "the clear lands while the retired producer is still exiting");

    // And the retired player was ALREADY silenced while the lane waits: one
    // 50 ms settle (the server may finish one in-flight callback), then two
    // samples across a window the pre-fix code spent playing. paused() is
    // the state the hand-off established; frozen deliveredFrames is what it
    // did to the server — the actual old-source delivery the review asked
    // to capture.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto first = transport.retiringAudioDelivery();
    check(first.has_value(), "the retirement lane still holds the outgoing player");
    check(first->paused && !first->sinkActive,
          "the outgoing audio was deactivated at the hand-off, not at the join");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto second = transport.retiringAudioDelivery();
    check(second.has_value() && second->deliveredFrames == first->deliveredFrames,
          "the retired stream delivered nothing more to the server across the window");
    std::fprintf(stderr,
                 "6.22c: hand-off %.1f ms; retired delivery %llu frames frozen across 200 ms "
                 "of the scripted 700 ms exit\n",
                 handoffMs,
                 first.has_value() ? static_cast<unsigned long long>(first->deliveredFrames) : 0);

    // The seam already forgot the transport — the mirror's story is the
    // successor's silence. When the lane's sleep ends the reap rides the
    // same render-thread pass a frame loop uses.
    check(!transport.mediaTransportLive(), "status reports no background after the clear");
    for (int attempt = 0; attempt < 100 && transport.retiringAudioDelivery().has_value();
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the scripted exit
    }
    transport.applyTransportOps();
    check(!transport.retiringAudioDelivery().has_value(),
          "the render-thread pass reaped the completed retirement");

    transport.shutdown();
    unsetenv("OPENSHOW_FAULT_PACING_EXIT_MS");
    unsetenv("OPENSHOW_FAULT_PACING_EXIT_MATCH");
}

/* --- task 6.22d (review A4): a swap during a slow retirement DEFERS --------
   The retirement slot is one deep, and it used to be the frame loop's
   problem: presenting C (or clearing B) while A still retired entered an
   untimed sleep on the realtime thread before B had even been stopped. Two
   ordinary operator changes plus one slow exit froze the audience — and two
   quick changes naturally overlap any slow retirement. The contract: the
   loop never waits on capacity. applyBackground keeps the LATEST wanted
   transition (a burst coalesces), returns false (nothing owed a present —
   the scene keeps drawing the seam's held frames of what is on screen), and
   the retry rides the next applyTransportOps pass, which the lane's own wake
   makes prompt. No unbounded queue anywhere.

   The scripted 700 ms pacing exit makes "A is retiring" measurable rather
   than hoped: the third call is timed, and its deferral is checked against
   the live mirror (still B — requested vs presented kept distinct) before
   the transition lands through the loop pass. Same skip conditions as the
   6.22c fixture. */
bool makeSilentClip(const std::string& path) {
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -nostdin -y "
        "-f lavfi -i nullsrc=s=160x96:r=12 -t 2 "
        "-c:v libx264 -preset veryfast -pix_fmt yuv420p " + path;
    return std::system(command.c_str()) == 0;
}

// Drive the loop's transport pass until the predicate holds (this is what a
// frame loop does with the lane's wake: one glfw-waked pass, reap, retry).
// Returns the number of passes taken, or -1 when the budget ran out.
int pumpUntil(BackgroundTransport& transport, const std::function<bool()>& done) {
    for (int pass = 1; pass <= 100; ++pass) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport.applyTransportOps();
        if (done()) {
            return pass;
        }
    }
    return -1;
}

void testSwapDefersBehindSlowRetirement() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::printf("   skipped the deferred-swap fixtures (no ffmpeg)\n");
        return;
    }
    openshow::worker::VulkanContext vk;
    if (!vk.createHeadless()) {
        std::printf("   skipped the deferred-swap fixtures (no headless Vulkan device)\n");
        return;
    }
    auto gr = GrDirectContexts::MakeVulkan(vk.backendContext());
    if (!gr) {
        std::printf("   skipped the deferred-swap fixtures (no Ganesh context)\n");
        return;
    }
    std::filesystem::create_directories("transport-fixtures");
    const std::string clipA = "transport-fixtures/defer-a.mp4";
    const std::string clipB = "transport-fixtures/defer-b.mp4";
    const std::string clipC = "transport-fixtures/defer-c.mp4";
    if (!makeSilentClip(clipA) || !makeSilentClip(clipB) || !makeSilentClip(clipC)) {
        check(false, "the deferred-swap fixture clips are generated");
        return;
    }

    // Only clip A carries the slow exit (6.21c2's seam matches by name), so
    // B and C open and close at normal speed and every slow window in these
    // scenarios is provably A's retirement.
    setenv("OPENSHOW_FAULT_PACING_EXIT_MS", "800", 1);
    setenv("OPENSHOW_FAULT_PACING_EXIT_MATCH", "defer-a.mp4", 1);

    openshow::worker::GpuHandles handles;
    handles.device = vk.device();
    handles.physicalDevice = vk.physicalDevice();
    handles.graphicsQueueIndex = vk.queueIndex();
    handles.uploadQueueIndex = vk.uploadQueueIndex();
    handles.uploadQueue = vk.uploadQueue();
    handles.context = gr.get();
    const auto resident = [](const std::string&) { return true; };

    // --- A→B→C: the third swap is the first one the slot-full rule used to
    // block (B's own teardown reached the sleep loop with A's lane waiting).
    {
        BackgroundTransport transport;
        transport.noteRunStart(kEpoch);
        check((transport.applyBackground(videoBackground(clipA), handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted),
              "A starts (the clip whose retirement will be scripted slow)");
        check((transport.applyBackground(videoBackground(clipB), handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted),
              "B starts beside the retiring A (6.21c2's allowed overlap)");

        const auto began = std::chrono::steady_clock::now();
        const bool startedC = (transport.applyBackground(videoBackground(clipC), handles,
                                                        resident) == BackgroundTransport::BackgroundApply::kVideoStarted);
        const double thirdMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - began)
                                   .count();
        check(!startedC, "the C swap defers rather than waiting on A's lane");
        check(transport.applyBackground(videoBackground(clipC), handles, resident) ==
                  BackgroundTransport::BackgroundApply::kDeferred,
              "the caller receives an explicit deferred outcome");
        check(thirdMs < 100.0, "the deferred call returned in milliseconds");

        // Requested vs presented, kept distinct: the transport still in
        // service is B and status says exactly that — it does not claim the
        // deferred C, and it does not go silent about B.
        transport.refreshMediaState();
        check(transport.mediaTransportLive() && transport.mediaStatus().path == clipB,
              "while deferred, status honestly reports the live B transport");

        const int pass = pumpUntil(transport, [&] {
            transport.applyBackground(videoBackground(clipC), handles, resident);
            transport.refreshMediaState();
            return transport.mediaStatus().path == clipC;
        });
        check(pass > 0, "a loop pass retried the deferred transition and C started");
        std::fprintf(stderr, "6.22d: A→B→C deferred %.1f ms then landed on pass %d\n",
                     thirdMs, pass);
        transport.shutdown();  // drains C's (fast) retirement; must not hang
    }

    // --- A→B→clear: same deferral for a clear-to-black. The clear cannot
    // precede B's teardown, so it waits too — and status keeps reporting the
    // live B until it actually lands (no premature silence in the mirror).
    {
        BackgroundTransport transport;
        transport.noteRunStart(kEpoch);
        check((transport.applyBackground(videoBackground(clipA), handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted),
              "A starts again (clear scenario)");
        check((transport.applyBackground(videoBackground(clipB), handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted),
              "B starts beside the retiring A");

        const auto began = std::chrono::steady_clock::now();
        const bool startedClear =
            (transport.applyBackground(openshow::scene::Background{}, handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted);
        const double clearMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - began)
                                   .count();
        check(!startedClear, "the clear defers rather than waiting on A's lane");
        check(clearMs < 100.0, "the deferred clear returned in milliseconds");
        transport.refreshMediaState();
        check(transport.mediaTransportLive() && transport.mediaStatus().path == clipB,
              "a deferred clear still reports B live until it lands");

        const int pass = pumpUntil(transport, [&] {
            transport.applyBackground(openshow::scene::Background{}, handles, resident);
            transport.refreshMediaState();
            return !transport.mediaTransportLive() && transport.mediaStatus().path.empty();
        });
        check(pass > 0, "a loop pass retried the deferred clear and the transport ended");
        std::fprintf(stderr, "6.22d: A→B→clear deferred %.1f ms then landed on pass %d\n",
                     clearMs, pass);
        transport.shutdown();
    }

    unsetenv("OPENSHOW_FAULT_PACING_EXIT_MS");
    unsetenv("OPENSHOW_FAULT_PACING_EXIT_MATCH");
}

/* --- task 6.22e1 (review A5): the duration is metadata, not a frame wait --
   Publishing a staged standalone player used to open the container AGAIN
   for its duration — on the render thread, INSIDE fMutex, where every
   audience frame and every status reader waits behind it. The staged
   player's own lane reads the file once and the publish consumes the
   staged value; the seam scripts a probe slow enough that the difference
   is a measurement, not a hope. Restoring the inline probe (negative
   control) makes both the publish pass and a concurrent mediaStatus()
   reader pay the scripted wait. */
void testStandaloneDurationTravelsWithStage() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::printf("   skipped the standalone-duration fixture (no ffmpeg)\n");
        return;
    }
    openshow::media::PipeWireSink probe;
    std::string probeError;
    if (!probe.start(48000, &probeError)) {
        std::printf("   skipped the standalone-duration fixture (no PipeWire daemon: %s)\n",
                    probeError.c_str());
        return;
    }
    probe.stop();

    std::filesystem::create_directories("transport-fixtures");
    const std::string track = "transport-fixtures/stage-duration.wav";
    if (std::system(("ffmpeg -hide_banner -loglevel error -nostdin -y -f lavfi -i "
                     "sine=frequency=330:sample_rate=48000 -t 2 " + track)
                        .c_str()) != 0) {
        check(false, "the standalone fixture track is generated");
        return;
    }
    setenv("OPENSHOW_FAULT_STANDALONE_PROBE_MS", "500", 1);
    setenv("OPENSHOW_FAULT_STANDALONE_PROBE_MATCH", "stage-duration.wav", 1);

    BackgroundTransport transport;
    std::string refusal;
    check(transport.requestStandaloneAudio(track, &refusal),
          "the duration-probed standalone request is accepted");

    // A status reader running through the whole bring-up and publish: the
    // pre-fix probe ran under fMutex, so this reader felt the scripted
    // wait even though it only wanted the mirror.
    std::atomic<bool> reading{true};
    std::atomic<double> worstStatusMs{0.0};
    std::thread reader([&] {
        while (reading.load(std::memory_order_acquire)) {
            const auto began = std::chrono::steady_clock::now();
            static_cast<void>(transport.mediaStatus());
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - began)
                                  .count();
            double prior = worstStatusMs.load(std::memory_order_relaxed);
            while (ms > prior &&
                   !worstStatusMs.compare_exchange_weak(prior, ms, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // The render thread's own view: no single transport pass may approach
    // the scripted probe cost.
    double worstPassMs = 0.0;
    bool live = false;
    for (int pass = 0; pass < 400 && !live; ++pass) {
        const auto began = std::chrono::steady_clock::now();
        transport.applyTransportOps();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - began)
                              .count();
        worstPassMs = std::max(worstPassMs, ms);
        live = transport.standaloneLive();
        if (!live) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    reading.store(false, std::memory_order_release);
    reader.join();

    check(live, "the standalone track published after the slow-probe bring-up");
    check(worstPassMs < 100.0, "no transport pass paid the duration probe");
    check(worstStatusMs.load() < 100.0, "the status reader never waited behind the probe");
    transport.refreshMediaState();
    const openshow::worker::MediaStatus media = transport.mediaStatus();
    check(media.standalone.duration > 1.5 && media.standalone.duration < 3.0,
          "the staged duration reached the status block");
    std::fprintf(stderr,
                 "6.22e1: scripted 500 ms probe; worst transport pass %.1f ms, worst status "
                 "read %.1f ms, duration %.3f s\n",
                 worstPassMs, worstStatusMs.load(), media.standalone.duration);

    transport.shutdown();
    unsetenv("OPENSHOW_FAULT_STANDALONE_PROBE_MS");
    unsetenv("OPENSHOW_FAULT_STANDALONE_PROBE_MATCH");
}

/* --- task 6.22e3 (review A5): the replaced standalone player dies on a lane --
   Replacing or abandoning a standalone track destroyed its AvPlayer on the
   render thread — feeder, mixer and sink joins whose "normally milliseconds"
   the review rightly refused to accept as a contract. The hand-off now
   SILENCES the outgoing track (pause(), no joins — 6.22c's rule for the
   other player) and hands the stop to a one-deep lane; a swap arriving
   while that lane still runs is DEFERRED rather than waited: the latest
   intent wins, the live track keeps playing and keeps being reported.
   OPENSHOW_FAULT_PLAYER_STOP_MS/_MATCH sleeps inside the dying player's
   stop() after its exits have been opened — the shape of a stalled server,
   which delaying start() cannot detect.

   With a 700 ms scripted death: the swap pass returns in milliseconds and
   the retiring A is already paused with its server delivery frozen (seam);
   B publishes BESIDE A's dying lane; a removal inside the same window
   defers with B still live and still honestly named, and lands on a pass
   after the lane reports in. Restoring the inline destruction fails the
   hand-off timing and both seam checks; removing the deferral pays the
   lane inside the removal pass. */
void testStandaloneStopRunsOnLane() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::printf("   skipped the standalone-stop lane fixtures (no ffmpeg)\n");
        return;
    }
    openshow::media::PipeWireSink probe;
    std::string probeError;
    if (!probe.start(48000, &probeError)) {
        std::printf("   skipped the standalone-stop lane fixtures (no PipeWire daemon: %s)\n",
                    probeError.c_str());
        return;
    }
    probe.stop();

    std::filesystem::create_directories("transport-fixtures");
    const std::string trackA = "transport-fixtures/stop-a.m4a";
    const std::string trackB = "transport-fixtures/stop-b.m4a";
    const auto makeTrack = [](const std::string& path) {
        return std::system(("ffmpeg -hide_banner -loglevel error -nostdin -y -f lavfi -i "
                            "anullsrc=channel_layout=stereo:sample_rate=48000 -t 2 -c:a aac " +
                            path)
                               .c_str()) == 0;
    };
    if (!makeTrack(trackA) || !makeTrack(trackB)) {
        check(false, "the standalone-stop fixture tracks are generated");
        return;
    }
    setenv("OPENSHOW_FAULT_PLAYER_STOP_MS", "700", 1);
    setenv("OPENSHOW_FAULT_PLAYER_STOP_MATCH", "stop-a.m4a", 1);

    BackgroundTransport transport;
    transport.noteRunStart(kEpoch);
    std::string refusal;
    check(transport.requestStandaloneAudio(trackA, &refusal), "track A is accepted");
    check(pumpUntil(transport, [&] { return transport.standaloneLive(); }) > 0,
          "A is live before its scripted death");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let delivery accrue

    // The swap. The pass that claims A must not pay A's death.
    const auto began = std::chrono::steady_clock::now();
    check(transport.requestStandaloneAudio(trackB, &refusal), "the swap to B is accepted");
    transport.applyTransportOps();  // the claim-and-hand-off pass
    const double handoffMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - began)
                                 .count();
    check(handoffMs < 100.0, "the swap pass did not pay A's scripted stop");

    // The retiring player is observable AND silent now — paused is the
    // state the hand-off established, and no new delivery can follow it.
    const auto delivery = transport.retiringStandaloneDelivery();
    check(delivery.has_value() && delivery->paused && !delivery->sinkActive,
          "the retired track is paused at the hand-off, before the lane stops it");
    // One in-flight server callback may still complete after the
    // deactivation (6.22c's settle): the freeze is measured from the
    // settled count, not from mid-callback.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto settled = transport.retiringStandaloneDelivery();
    const uint64_t framesAtHandoff = settled ? settled->deliveredFrames : 0;

    // B publishes beside the dying lane — retirement holds the old track's
    // death, not the new one's birth.
    bool bLive = false;
    for (int pass = 0; pass < 100 && !bLive; ++pass) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        transport.applyTransportOps();
        bLive = transport.standaloneLive();
    }
    check(bLive, "B published while A's scripted stop still ran on the lane");

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto frozen = transport.retiringStandaloneDelivery();
    const bool frozenOk = frozen.has_value() && frozen->deliveredFrames == framesAtHandoff;
    check(frozenOk, "the retired track's server delivery stayed frozen across the window");
    std::fprintf(stderr,
                 "6.22e3: hand-off %.1f ms; retiring delivery frozen at %llu frames across "
                 "120 ms of the scripted 700 ms exit\n",
                 handoffMs,
                 static_cast<unsigned long long>(framesAtHandoff));

    // The removal inside the same scripted window: deferred, not waited.
    // B keeps playing and the mirror keeps naming it — requested-vs-
    // presented stays honest while the lane drains.
    const auto removalBegan = std::chrono::steady_clock::now();
    check(transport.requestStandaloneAudio("", &refusal), "the removal request is accepted");
    transport.applyTransportOps();
    const double removalMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - removalBegan)
                                 .count();
    check(removalMs < 100.0, "the deferred removal returned in milliseconds");
    transport.refreshMediaState();
    check(transport.standaloneLive() && transport.mediaStatus().standalone.live &&
              transport.mediaStatus().standalone.path == trackB,
          "while deferred, status honestly reports the live B");

    const int landed = pumpUntil(transport, [&] { return !transport.standaloneLive(); });
    check(landed > 0, "the deferred removal landed on a later pass once the lane reported");
    std::fprintf(stderr, "6.22e3: removal deferred %.1f ms then landed on pass %d\n", removalMs,
                 landed);

    transport.shutdown();  // drains both lanes; must not hang
    unsetenv("OPENSHOW_FAULT_PLAYER_STOP_MS");
    unsetenv("OPENSHOW_FAULT_PLAYER_STOP_MATCH");
}

// 6.23a: C arrives at the completed-stage claim boundary while A still
// occupies the retirement lane. B must remain held, not overwrite A's owner.
void testStandaloneClaimReservesRetirement() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) return;
    openshow::media::PipeWireSink probe;
    std::string error;
    if (!probe.start(48000, &error)) {
        std::printf("   skipped standalone claim race (no PipeWire: %s)\n", error.c_str());
        return;
    }
    probe.stop();
    std::filesystem::create_directories("transport-fixtures");
    const std::string a = "transport-fixtures/claim-a.m4a";
    const std::string b = "transport-fixtures/claim-b.m4a";
    const std::string c = "transport-fixtures/claim-c.m4a";
    for (const auto& path : {a, b, c}) {
        if (std::system(("ffmpeg -hide_banner -loglevel error -nostdin -y -f lavfi -i "
                         "anullsrc=channel_layout=stereo:sample_rate=48000 -t 2 -c:a aac "
                         + path).c_str()) != 0) {
            check(false, "standalone claim fixtures generated");
            return;
        }
    }
    setenv("OPENSHOW_FAULT_PLAYER_STOP_MS", "1500", 1);
    setenv("OPENSHOW_FAULT_PLAYER_STOP_MATCH", "claim-a.m4a", 1);
    BackgroundTransport transport;
    bool inject = false;
    bool injected = false;
    transport.setStandaloneStageClaimHook([&] {
        if (inject && !injected) {
            injected = true;
            std::thread request([&] { transport.armStandaloneTrack(c); });
            request.join();
        }
    });
    check(transport.requestStandaloneAudio(a, &error), "claim race: A accepted");
    check(pumpUntil(transport, [&] { return transport.standaloneLive(); }) > 0,
          "claim race: A is live");
    transport.armStandaloneTrack(b);
    transport.applyTransportOps();
    inject = true;
    check(pumpUntil(transport, [&] { return injected; }) > 0,
          "claim race: request C arrives as B completes");
    check(transport.retiringStandaloneDelivery().has_value(),
          "claim race: A's retirement ownership is retained");
    check(!transport.standaloneLive(), "claim race: superseded B remains held while A retires");
    const auto began = std::chrono::steady_clock::now();
    transport.applyTransportOps();
    check(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
              .count() < 100, "claim race: occupied capacity does not block rendering");
    check(pumpUntil(transport, [&] {
        return transport.standaloneLive() && transport.mediaStatus().standalone.path == c;
    }) > 0, "claim race: latest C starts after both predecessor owners retire safely");
    transport.shutdown();
    unsetenv("OPENSHOW_FAULT_PLAYER_STOP_MS");
    unsetenv("OPENSHOW_FAULT_PLAYER_STOP_MATCH");
}

// 6.23e: a superseded successful player, or a failed obsolete stage, is
// silent bookkeeping. A genuine failure of the current intent stays visible.
void testStandaloneStageOutcomes() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) return;
    openshow::media::PipeWireSink probe;
    std::string error;
    if (!probe.start(48000, &error)) {
        std::printf("   skipped stage outcomes (no PipeWire: %s)\n", error.c_str());
        return;
    }
    probe.stop();
    std::filesystem::create_directories("transport-fixtures");
    const std::string a = "transport-fixtures/outcome-a.m4a";
    const std::string b = "transport-fixtures/outcome-b.m4a";
    const std::string c = "transport-fixtures/outcome-c.m4a";
    for (const auto& path : {a, b, c}) {
        const auto command = std::string("ffmpeg -v error -nostdin -y -f lavfi -i ")
            + "anullsrc=channel_layout=stereo:sample_rate=48000 -t 2 -c:a aac " + path;
        if (std::system(command.c_str()) != 0) { check(false, "outcome fixtures generated"); return; }
    }
    setenv("OPENSHOW_FAULT_AUDIO_START_MS", "700", 1);
    setenv("OPENSHOW_FAULT_AUDIO_START_MATCH", "outcome-a.m4a", 1);
    {
        BackgroundTransport transport;
        check(transport.requestStandaloneAudio(a, &error), "outcome: A validates");
        transport.applyTransportOps();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        transport.armStandaloneTrack(b);
        check(pumpUntil(transport, [&] {
            return transport.standaloneLive() && transport.mediaStatus().standalone.path == b;
        }) > 0, "outcome: healthy B supersedes the held A");
        openshow::worker::MediaError failure;
        check(!transport.takeMediaError(&failure) && transport.mediaStatus().standalone.error.empty(),
              "outcome: successful supersession emits no media error");
        transport.shutdown();
    }
    unsetenv("OPENSHOW_FAULT_AUDIO_START_MS");
    unsetenv("OPENSHOW_FAULT_AUDIO_START_MATCH");
    {
        BackgroundTransport transport;
        bool superseded = false;
        setenv("OPENSHOW_FAULT_AUDIO_MIXER_FAIL", "1", 1);
        transport.setStandaloneStageClaimHook([&] {
            if (!superseded) {
                superseded = true;
                unsetenv("OPENSHOW_FAULT_AUDIO_MIXER_FAIL");
                transport.armStandaloneTrack(b);
            }
        });
        check(transport.requestStandaloneAudio(a, &error), "outcome: soon-obsolete A validates");
        check(pumpUntil(transport, [&] {
            return transport.standaloneLive() && transport.mediaStatus().standalone.path == b;
        }) > 0, "outcome: B replaces an obsolete failed stage");
        openshow::worker::MediaError failure;
        check(!transport.takeMediaError(&failure), "outcome: obsolete failure does not fault the current intent");
        transport.setStandaloneStageClaimHook({});
        setenv("OPENSHOW_FAULT_AUDIO_MIXER_FAIL", "1", 1);
        transport.armStandaloneTrack(c);
        check(pumpUntil(transport, [&] {
            const auto media = transport.mediaStatus().standalone;
            return media.path == c && !media.error.empty();
        }) > 0, "outcome: current C's genuine start failure is visible");
        check(transport.takeMediaError(&failure) && failure.path == c &&
                  failure.message.find("injected mixer failure") != std::string::npos,
              "outcome: the genuine notification carries its own path and reason");
        check(!transport.takeMediaError(&failure), "outcome: genuine failure is not duplicated");
        unsetenv("OPENSHOW_FAULT_AUDIO_MIXER_FAIL");
        transport.shutdown();
    }
}

/* --- task 6.22e4 (review A5): the seek-completion wait off the loop -------
   AvPlayer::seekTo fanned the jump out and then waited up to 500 ms PER
   FEEDER for the container's acknowledgment — inline, on the transport
   pass. A slow acknowledgment held the audience's frame loop for its whole
   window; "normally milliseconds" is not a contract. The seek now splits:
   the pass FANS OUT (no wait), a lane worker holds the WAIT, and the pass
   the completion wakes runs the FINISH half (sink flush, clock rebase) and
   the fault report. While a flight runs, the momentary-op slot is not
   consumed — a pause behind the seek holds its turn — so no op overtakes
   an unfinished seek; gain, mute and loop are independent slots. And a
   transport retired mid-flight takes its flight along: the lane joins the
   seek worker BEFORE stopping the player, because the worker waits on
   feeders that player owns.

   OPENSHOW_FAULT_SEEK_ACK_MS/_MATCH sleeps between the container's jump
   and the settle — the ack window a waiter pays. With a 300 ms scripted
   ack: the seek pass returns in milliseconds, a pause asked for mid-flight
   is still unapplied (status still says playing), and the completion lands
   with the position at the target and the held pause applied behind it. A
   swap across the second flight hand-offs fast (the lane joins the worker,
   not the frame tick), and shutdown drains it. Reverting to the synchronous
   seek pays the ack on the pass and swallows the held pause's turn; opening
   the op slot mid-flight loses the hold — the finding's exact symptoms. */
void testSeekCompletesOffTheFrameTick() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::printf("   skipped the seek-flight fixtures (no ffmpeg)\n");
        return;
    }
    openshow::media::PipeWireSink probe;
    std::string probeError;
    if (!probe.start(48000, &probeError)) {
        std::printf("   skipped the seek-flight fixtures (no PipeWire daemon: %s)\n",
                    probeError.c_str());
        return;
    }
    probe.stop();
    openshow::worker::VulkanContext vk;
    if (!vk.createHeadless()) {
        std::printf("   skipped the seek-flight fixtures (no headless Vulkan device)\n");
        return;
    }
    auto gr = GrDirectContexts::MakeVulkan(vk.backendContext());
    if (!gr) {
        std::printf("   skipped the seek-flight fixtures (no Ganesh context)\n");
        return;
    }

    std::filesystem::create_directories("transport-fixtures");
    const std::string clip = "transport-fixtures/seek-ack.mp4";
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -nostdin -y "
        "-f lavfi -i nullsrc=s=160x96:r=12 "
        "-f lavfi -i sine=frequency=440:sample_rate=48000 "
        "-t 6 -c:v libx264 -preset veryfast -pix_fmt yuv420p -c:a aac " + clip;
    if (std::system(command.c_str()) != 0) {
        check(false, "the seek-flight fixture clip is generated");
        return;
    }
    setenv("OPENSHOW_FAULT_SEEK_ACK_MS", "300", 1);
    setenv("OPENSHOW_FAULT_SEEK_ACK_MATCH", "seek-ack.mp4", 1);

    BackgroundTransport transport;
    transport.noteRunStart(kEpoch);
    openshow::worker::GpuHandles handles;
    handles.device = vk.device();
    handles.physicalDevice = vk.physicalDevice();
    handles.graphicsQueueIndex = vk.queueIndex();
    handles.uploadQueueIndex = vk.uploadQueueIndex();
    handles.uploadQueue = vk.uploadQueue();
    handles.context = gr.get();
    const auto resident = [](const std::string&) { return true; };
    check((transport.applyBackground(videoBackground(clip), handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted),
          "the audio-bearing background transport starts for the seek flight");

    bool audible = false;
    for (int attempt = 0; attempt < 100 && !audible; ++attempt) {
        transport.refreshMediaState();
        const openshow::worker::MediaStatus media = transport.mediaStatus();
        audible = media.playing && media.audio == "live" && media.position > 0.05;
        if (!audible) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    check(audible, "the clip is really delivering audio before its seek");

    // The seek pass. Pre-6.22e4 this call contained the wait: the scripted
    // 300 ms ack WAS this function's runtime.
    transport.requestMediaSeek(2.0);
    const auto began = std::chrono::steady_clock::now();
    transport.applyTransportOps();
    const double seekPassMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - began)
                                  .count();
    check(seekPassMs < 100.0, "the seek pass did not pay the scripted ack");

    // A pause asked for while the flight waits must NOT be consumed: the
    // order the synchronous seek enforced was pause-after-seek, and the
    // mirror's story mid-flight is still the live transport.
    transport.requestMediaPause();
    const auto heldBegan = std::chrono::steady_clock::now();
    transport.applyTransportOps();
    const double heldPassMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - heldBegan)
                                  .count();
    transport.refreshMediaState();
    check(heldPassMs < 100.0, "a pass behind an in-flight seek stayed fast");
    check(transport.mediaStatus().playing,
          "the pause held its turn behind the in-flight seek");

    // The completion: the finish half rebases the clock onto the target;
    // the held pause then lands behind it, two facts one after the other.
    const int landed = pumpUntil(transport, [&] {
        transport.refreshMediaState();
        const auto media = transport.mediaStatus();
        return !media.playing && media.position > 1.4 && media.position < 3.2;
    });
    check(landed > 0, "the completion, held pause and target picture land in order");
    const double position = transport.mediaStatus().position;
    check(position > 1.4 && position < 3.2,
          "the seek finished at its target with the clock rebased");
    std::fprintf(stderr,
                 "6.22e4: seek pass %.1f ms against a 300 ms scripted ack; completion and "
                 "held pause landed on pass %d at %.2f s\n",
                 seekPassMs, landed, position);

    // Retirement across a flight: the second seek starts its 300 ms wait,
    // and the background goes away mid-window. The hand-off must not pay
    // the ack — the flight rides the lane, whose join of the seek worker
    // releases the wait (abort) and then ends the player safely.
    transport.requestMediaSeek(1.0);
    transport.applyTransportOps();  // flight #2, waiting
    const auto swapBegan = std::chrono::steady_clock::now();
    (transport.applyBackground(openshow::scene::Background{}, handles, resident) == BackgroundTransport::BackgroundApply::kVideoStarted);
    const double swapMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - swapBegan)
            .count();
    check(swapMs < 150.0, "the swap across an in-flight seek did not pay the ack");
    transport.shutdown();  // drains the retirement holding the flight; must not hang
    std::fprintf(stderr, "6.22e4: swap over a waiting seek flight took %.1f ms\n", swapMs);
    unsetenv("OPENSHOW_FAULT_SEEK_ACK_MS");
    unsetenv("OPENSHOW_FAULT_SEEK_ACK_MATCH");
}

/* --- task 6.22f (review A6): the mirror retires with the player -----------
   Replacing an ACTIVE standalone track removed the old player, cleared the
   command gate and took it down — but only the empty-path removal also
   updated the published block. Through the successor's whole bring-up,
   media.standalone therefore kept reporting the GONE player as live and
   playing at a frozen position (the refresh early-returns while no player
   is up: the swap owns the block, and until 6.22f it had said nothing for
   a replacement), while gain and mute were already being refused as "no
   standalone track is playing" — controls that look alive and are refused,
   a missing state transition rather than a sampling delay. The claim now
   publishes the pending-selection state for BOTH cases: the block names
   the wanted path with live=false and no position it does not have —
   requested-not-yet-presented, agreeing with the gate.
   OPENSHOW_FAULT_AUDIO_START_MS makes B's bring-up hold for 700 ms so the
   intermediate window is measured, not hoped; reverting the claim to
   removal-only fields leaves the removed instance live in status and
   fails the intermediate checks. The leg after it pins the error's owner: a
   failed bring-up writes its own path and sticky error, and the next claim
   drops that error beside its new path — a story belongs to the instance
   that lived it. */
void testStandaloneStatusTracksReplacement() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::printf("   skipped the replacement-status fixture (no ffmpeg)\n");
        return;
    }
    openshow::media::PipeWireSink probe;
    std::string probeError;
    if (!probe.start(48000, &probeError)) {
        std::printf("   skipped the replacement-status fixture (no PipeWire daemon: %s)\n",
                    probeError.c_str());
        return;
    }
    probe.stop();

    std::filesystem::create_directories("transport-fixtures");
    const std::string trackA = "transport-fixtures/mirror-a.m4a";
    const std::string trackB = "transport-fixtures/mirror-slowb.m4a";
    const auto makeTrack = [](const std::string& path) {
        return std::system(("ffmpeg -hide_banner -loglevel error -nostdin -y -f lavfi -i "
                            "anullsrc=channel_layout=stereo:sample_rate=48000 -t 2 -c:a aac " +
                            path)
                               .c_str()) == 0;
    };
    if (!makeTrack(trackA) || !makeTrack(trackB)) {
        check(false, "the replacement-status fixture tracks are generated");
        return;
    }

    BackgroundTransport transport;
    transport.noteRunStart(kEpoch);
    std::string refusal;
    check(transport.requestStandaloneAudio(trackA, &refusal), "track A is accepted");
    const int up = pumpUntil(transport, [&] {
        transport.refreshMediaState();
        const auto block = transport.mediaStatus().standalone;
        return block.live && block.playing && block.position > 0.05;
    });
    check(up > 0, "A is live and advancing before the replacement");

    // The replacement. B's bring-up is scripted slow, so the intermediate
    // window is the majority of the swap: what does the block say while no
    // player exists?
    setenv("OPENSHOW_FAULT_AUDIO_START_MS", "700", 1);
    setenv("OPENSHOW_FAULT_AUDIO_START_MATCH", "mirror-slowb", 1);
    check(transport.requestStandaloneAudio(trackB, &refusal), "the swap to B is accepted");
    transport.applyTransportOps();  // the claim-and-stage pass
    transport.refreshMediaState();
    const auto pending = transport.mediaStatus().standalone;
    check(!pending.live && !pending.playing,
          "the claim took the removed player out of status at once");
    check(pending.path == trackB, "the pending block names the wanted track");
    check(pending.position < 0.0, "the pending block presents no position it does not have");
    std::string gainError;
    check(!transport.requestMediaGain(AudioSource::kStandalone, 0.5, &gainError),
          "and the command gate refuses what the mirror already shows");

    // Through the bring-up window: the early-returning refresh preserves
    // the pending state, it does not resurrect A.
    for (int pass = 0; pass < 5; ++pass) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport.applyTransportOps();
        transport.refreshMediaState();
    }
    const auto midflight = transport.mediaStatus().standalone;
    check(!midflight.live && !midflight.playing && midflight.path == trackB &&
              midflight.position < 0.0,
          "the pending state holds through the slow bring-up");
    std::fprintf(stderr, "6.22f: mid-replacement status: live=%d playing=%d position=%.2f\n",
                 midflight.live ? 1 : 0, midflight.playing ? 1 : 0, midflight.position);

    const int landed = pumpUntil(transport, [&] {
        transport.refreshMediaState();
        const auto block = transport.mediaStatus().standalone;
        return block.live && block.playing && block.position > 0.0;
    });
    check(landed > 0, "the successor comes live, playing and advancing behind the window");
    std::fprintf(stderr, "6.22f: B published on pass %d of the pump after its 700 ms start\n",
                 landed);
    unsetenv("OPENSHOW_FAULT_AUDIO_START_MS");
    unsetenv("OPENSHOW_FAULT_AUDIO_START_MATCH");

    // The other half of the contract: the error belongs to the instance that
    // wrote it. A bring-up that fails tells its story in this block (path
    // and sticky error, live=false); the NEXT selection must not inherit it,
    // because beside a new path it describes a track that never had that
    // failure. So the claim clears it — and a removal still keeps it, which
    // the existing scenarios read (the operator needs to know what broke).
    setenv("OPENSHOW_FAULT_AUDIO_MIXER_FAIL", "1", 1);
    const std::string trackC = "transport-fixtures/mirror-fails.m4a";
    if (!makeTrack(trackC)) {
        check(false, "the failing-bring-up fixture track is generated");
    }
    check(transport.requestStandaloneAudio(trackC, &refusal), "the doomed selection is accepted");
    const int failed = pumpUntil(transport, [&] {
        transport.refreshMediaState();
        return transport.mediaStatus().standalone.path == trackC &&
               !transport.mediaStatus().standalone.live &&
               !transport.mediaStatus().standalone.error.empty();
    });
    check(failed > 0, "a failed bring-up writes its own path and error into the block");
    unsetenv("OPENSHOW_FAULT_AUDIO_MIXER_FAIL");
    check(transport.requestStandaloneAudio(trackB, &refusal),
          "and a new selection is accepted beside that error");
    transport.applyTransportOps();  // the claim-and-stage pass again
    transport.refreshMediaState();
    const auto after = transport.mediaStatus().standalone;
    check(after.path == trackB && !after.live && after.error.empty(),
          "the new claim leaves the removed instance's error behind");
    std::fprintf(stderr, "6.22f: failed bring-up wrote an error on pass %d; the next claim: "
                         "live=%d error=\"%s\"\n",
                 failed, after.live ? 1 : 0, after.error.c_str());

    transport.shutdown();
}

}  // namespace

int main() {
    // The intent slots wake the render loop through GLFW's cross-thread
    // nudge; without a window the call is a no-op that raises a library
    // error, and this fixture has no display to answer it — silence it on
    // purpose, the live fixtures cover the real wake path.
    glfwSetErrorCallback([](int, const char*) {});
    glfwInit();  // best-effort: posted events land on its wakeup pipe when real

    BackgroundTransport transport;
    testIdle(transport);
    testRefusals(transport);
    testStandaloneProbe(transport);
    testStandaloneIntentGeneration();
    testClockEpochDropsUndrawnDeadlines();
    testStillsAndIdentity(transport);
    testSeam(transport);
    testScheduleGrammar();

    testRetiredAudioGoesSilentAtHandoff();
    testSwapDefersBehindSlowRetirement();
    testStandaloneDurationTravelsWithStage();
    testStandaloneStopRunsOnLane();
    testStandaloneClaimReservesRetirement();
    testStandaloneStageOutcomes();
    testSeekCompletesOffTheFrameTick();
    testStandaloneStatusTracksReplacement();

    BackgroundTransport shutdownTransport;
    testShutdown(shutdownTransport);

    glfwTerminate();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d background-transport checks failed\n", gFailures);
        return 1;
    }
    std::printf("all background-transport checks passed\n");
    return 0;
}
