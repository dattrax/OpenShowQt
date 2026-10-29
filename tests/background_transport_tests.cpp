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
 * No GPU here: opening a video transport belongs to the live fixtures
 * (background_media_tests owns the real bring-up, the fault-injection
 * scripts and the A/V continuity rules against a real present). What lives
 * here is everything the transport decides WITHOUT decoding — the policy
 * the extraction moved — driven through the same public surface the session
 * drives it through, which is exactly what R10 asked production and bench
 * to share.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "include/core/SkImage.h"

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

void testStillsAndIdentity(BackgroundTransport& transport) {
    // The broken still (R18): the picture the audience should be seeing is
    // not in the cache, and the colour fill they get is a degradation the
    // operator must SEE — NAMED, once, paired with its path.
    FakePresenter presenter;
    transport.setVideoPresenter(&presenter);
    const std::string path = "/media/broken-still.png";
    bool started = transport.applyBackground(stillBackground(path),
                                             openshow::worker::GpuHandles{},
                                             [](const std::string&) { return false; });
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
    started = transport.applyBackground(stillBackground(path), openshow::worker::GpuHandles{},
                                        [](const std::string&) { return false; });
    check(!started, "still: a re-present still starts nothing");
    check(!transport.takeMediaError(&reported),
          "still: the same broken asset does not notify twice");
    check(transport.mediaStatus().error == media.error, "still: the sticky error stays");

    // The asset repaired: the same prepare that decodes it clears the
    // error (the recovery clears its own fault).
    transport.applyBackground(stillBackground(path), openshow::worker::GpuHandles{},
                              [](const std::string&) { return true; });
    media = transport.mediaStatus();
    check(media.error.empty(), "still: a decoded asset clears the error");
    check(media.path == path, "still: the healthy asset is still NAMED on air");

    // A plain colour fill: no asset, no story.
    transport.applyBackground(openshow::scene::Background{}, openshow::worker::GpuHandles{},
                              [](const std::string&) { return false; });
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
    testStillsAndIdentity(transport);
    testSeam(transport);
    testScheduleGrammar();

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
