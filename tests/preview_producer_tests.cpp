/*
 * The preview stream's owner (task 6.18a2): PreviewProducer's rules pinned
 * headless against the real object — the geometry limits, the generation
 * gates (selection and batch, modulo 2^32), the pin-at-accept through the
 * store's own pin, the thumbnail queue's caps and coalescing and its
 * one-decode-per-pump lookahead bound, the stream's lifecycle on the shm
 * filesystem, and the run loop's sleep answer (previewWake) that keeps a
 * stand-down a SLEEP and never a spin.
 *
 * No pixels: the live fixtures (preview_ownership, preview_selection,
 * preview_budget) own the GPU paths and the audience's deferral arithmetic
 * against a real present. What lives here is everything the producer
 * decides BEFORE it draws — the policy the extraction moved — and the one
 * rule a draw never breaks: it never opens a file. That rule the pump
 * proves against real PNGs, because a scaled decode is CPU work (SkCodec)
 * and needs no context.
 *
 * The renderer-dependent section follows scene_render_tests' convention:
 * without a usable typeface the companion cannot be built, and that section
 * exits 77 (skipped) rather than failing a machine with no fonts. Every
 * font-free rule runs first, and a failure there is a failure everywhere.
 */

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkImageInfo.h"
#include "include/encode/SkPngEncoder.h"

#include "openshow/protocol/preview_wire.h"
#include "openshow/scene/scene.h"

#include "render/SceneRenderer.h"
#include "session/PreviewProducer.h"
#include "session/SceneStore.h"

namespace {

namespace protocol = openshow::protocol;
using openshow::worker::ActiveScene;
using openshow::worker::PreviewProducer;
using openshow::worker::SceneRenderer;
using openshow::worker::SceneStore;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

bool shmExists(const std::string& name) {
    const std::string path = "/dev/shm" + name;  // shm_open's name drops the leading /
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

openshow::scene::Scene makeScene(const std::string& tag) {
    openshow::scene::Scene scene;
    scene.slideId = tag;
    scene.background.color = 0x204060u;
    return scene;
}

openshow::scene::Scene makeImageScene(const std::string& tag, const std::string& path) {
    openshow::scene::Scene scene = makeScene(tag);
    scene.background.hasImage = true;
    scene.background.isVideo = false;
    scene.background.assetPath = path;
    return scene;
}

// A PNG written here rather than by ffmpeg, so this fixture never skips on
// a missing tool (scene_image_cache_tests' choice, for the same reason).
bool writePng(const std::string& path, uint32_t size, uint32_t color) {
    SkBitmap bitmap;
    const SkImageInfo info =
        SkImageInfo::Make(static_cast<int>(size), static_cast<int>(size),
                          kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    if (!bitmap.tryAllocPixels(info)) {
        return false;
    }
    bitmap.erase(color, SkIRect::MakeWH(static_cast<int>(size), static_cast<int>(size)));
    sk_sp<SkData> png = SkPngEncoder::Encode(bitmap.pixmap(), SkPngEncoder::Options{});
    if (!png) {
        return false;
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t written = std::fwrite(png->data(), 1, png->size(), file);
    std::fclose(file);
    return written == png->size();
}

std::vector<PreviewProducer::ThumbnailEntry> makeBatch(
    const std::vector<std::string>& keys) {
    std::vector<PreviewProducer::ThumbnailEntry> batch;
    for (const std::string& key : keys) {
        PreviewProducer::ThumbnailEntry entry;
        entry.key = key;
        entry.scene = makeScene(key);
        batch.push_back(std::move(entry));
    }
    return batch;
}

// --- font-free rules -------------------------------------------------------

void geometryLimits() {
    PreviewProducer producer;
    PreviewProducer::PreviewInfo info;
    std::string error;
    check(!producer.start(32, 360, 15, &info, &error), "start refuses width 32");
    check(error.find("64..1920") != std::string::npos, "the refusal names the range");
    check(!producer.start(640, 24, 15, &info, &error), "start refuses height 24");
    check(!producer.start(640, 360, 0, &info, &error), "start refuses fps 0");
    check(!producer.start(640, 360, 61, &info, &error), "start refuses fps 61");
    check(!producer.start(1921, 360, 15, &info, &error), "start refuses width 1921");
    check(!producer.running(), "refusals start nothing");
    check(!producer.streamChangePending(), "a refused start leaves no pending change");

    // The accepted answer is the consumer's map: the segment's own layout.
    check(producer.start(640, 360, 24, &info, &error), "start accepts 640x360@24");
    check(info.width == 640 && info.height == 360, "info carries the geometry");
    check(info.slotCount == protocol::kPreviewSlotCount, "info carries the slot count");
    check(info.bytesPerSlot == protocol::previewSlotStrideBytes(640, 360),
          "info carries the slot stride");
    check(!info.name.empty() && info.name.front() == '/', "info names an shm object");
    check(!producer.running(), "running waits for the render thread");
    check(producer.streamChangePending(), "the accepted start is owed a segment");
}

void streamLifecycle() {
    PreviewProducer producer;
    PreviewProducer::PreviewInfo info;
    std::string error;
    check(producer.start(320, 180, 15, &info, &error), "start accepted");
    producer.applyStreamState();  // this thread plays the frame loop
    check(producer.running(), "applyStreamState builds the segment");
    check(shmExists(info.name), "the shm file exists under the announced name");
    const PreviewProducer::PreviewStatus running = producer.status();
    check(running.running && running.width == 320 && running.height == 180 &&
              running.fps == 15 && running.name == info.name && running.error.empty(),
          "the status mirror carries the live stream");
    check(!producer.streamChangePending(), "nothing owed after the apply");

    // A release naming a stream that is not live frees nothing and answers
    // nothing (R5's identity rule headless half: the live tests own the
    // slot-owning sequences); with no frame published there is no pending
    // notification either.
    producer.releaseSlot(0, 1, "some-other-stream");
    producer.releaseSlot(99, 1, info.name);
    PreviewProducer::PreviewReady ready;
    check(!producer.takeReady(&ready), "nothing is ready before a publish");

    // Replacement: the new segment is announced first and the old one is
    // unlinked by the same apply that publishes the new one (task 6.5's
    // retirement rule).
    PreviewProducer::PreviewInfo second;
    check(producer.start(960, 540, 30, &second, &error), "a second start replaces");
    check(second.name != info.name, "the replacement names a new segment");
    producer.applyStreamState();
    check(producer.running() && producer.status().width == 960,
          "the new segment is live after the apply");
    check(shmExists(second.name) && !shmExists(info.name),
          "the retired segment is unlinked");

    // Stop, and stop of something already stopped.
    check(producer.stop(), "stop reports the change");
    check(!producer.stop(), "stop is idempotent");
    producer.applyStreamState();
    check(!producer.running(), "the stream is gone after the apply");
    check(!shmExists(second.name), "stop's unlink happens on the render thread");
    const PreviewProducer::PreviewStatus down = producer.status();
    check(!down.running && down.published == 0 && down.dropped == 0,
          "the status resets with the stream");
}

void selectionGate() {
    PreviewProducer producer;
    std::string error;

    // No stream, no target — before any start, so this is the first refusal.
    check(!producer.select(7, 1, [](uint64_t, ActiveScene*) { return true; }, &error),
          "select refuses with no preview stream");
    check(error.find("preview.start") != std::string::npos,
          "the refusal names the command that fixes it");

    check(producer.start(320, 180, 15, nullptr, &error), "start accepted");
    // The DESIRED stream is the gate: between preview.start and the render
    // thread building the segment, an ask is a normal race, not a refusal.
    check(!producer.running(), "the segment is not built yet");

    // Unknown revision: the pin says no, the ask is refused — against the
    // real store, not a fake: an id the store never minted, and one it has
    // let go of, are the frontend's two ordinary ways to be wrong.
    SceneStore store;
    const auto pin = [&store](uint64_t rev, ActiveScene* out) { return store.pin(rev, out); };
    check(!producer.select(12345, 1, pin, &error), "select refuses a revision of nothing");
    check(error.find("prepared store") != std::string::npos,
          "the refusal tells the frontend to re-prepare");

    // The accept, with the store's own pin: the content copy is the entry's.
    const uint64_t revision = store.prepare(makeScene("edit"));
    check(revision != 0, "the store mints a non-zero revision");
    check(producer.select(revision, 5, pin, &error), "an advancing request is accepted");
    check(!producer.select(revision, 5, pin, &error), "a replayed request is refused");
    check(!producer.select(revision, 4, pin, &error), "a lower request is refused");
    const PreviewProducer::PreviewStatus after = producer.status();
    check(after.selection.active && after.selection.revision == revision &&
              after.selection.request == 5,
          "the selection mirror carries the accepted ask");
    const ActiveScene pinned = producer.selectedScene();
    check(pinned && pinned->slideId == "edit", "the pin content is the store's scene");

    // THE MODULO RULE. The counter is the frontend's and wraps: a step of
    // up to 2^31-1 is "later", one of 2^31 or more is "earlier", and after
    // 0xFFFFFFFF comes 0 — which must still be an advance.
    check(producer.select(0, 0x7FFFFFFF, pin, &error),
          "the largest forward step is accepted (and revision 0 stands down)");
    check(!producer.selectedScene(), "the stand-down released the pinned content");
    check(!producer.select(0, 0xFFFFFFFF, pin, &error),
          "half the space away or further is NOT forward, and is refused");
    check(producer.select(0, 0xFFFFFFFE, pin, &error), "0xFFFFFFFE follows 0x7FFFFFFF");
    check(producer.select(0, 0xFFFFFFFF, pin, &error), "one more is one more");
    check(producer.select(0, 0, pin, &error), "the counter wraps through zero");
    check(!producer.select(0, 0xFFFFFFFF, pin, &error), "and the wrapped gate still refuses back");

    // A new stream resets the generation domain: the same counter value a
    // fresh frontend would use must not be held hostage by the old one —
    // and the selection itself stands down with the stream it was drawn for.
    check(producer.start(320, 180, 15, nullptr, &error), "the stream is restarted");
    check(!producer.selectedScene(), "a restart tears the selection's pin down");
    check(!producer.status().selection.active, "and its mirror with it");
    check(producer.select(revision, 1, pin, &error),
          "the new stream accepts a fresh counter from 1 again");
}

void stopTearsEverythingDown() {
    PreviewProducer producer;
    std::string error;
    SceneStore store;
    const auto pin = [&store](uint64_t rev, ActiveScene* out) { return store.pin(rev, out); };
    const uint64_t revision = store.prepare(makeScene("edit"));

    check(producer.start(320, 180, 15, nullptr, &error), "start accepted");
    check(producer.select(revision, 1, pin, &error), "selection accepted");
    check(producer.stop(), "stop");
    check(!producer.selectedScene(), "the stopped stream gives back its pin");
    check(!producer.status().selection.active, "and the selection mirror resets");
    check(!producer.status().thumbnails.active, "and the list mirror with it");
    // The run loop must not wake for a stopped stream's ghosts.
    const auto now = std::chrono::steady_clock::now();
    check(!producer.previewWake(now, 0).owed, "a stopped stream owes nothing");
}

void wakePolicyWithoutStream() {
    PreviewProducer producer;
    const auto now = std::chrono::steady_clock::now();
    check(!producer.previewWake(now, 41).owed,
          "no stream: the loop sleeps whatever the audience counter says");
    check(!producer.previewWake(now, 0).owed, "no stream, no frames, no wake");
}

// --- renderer-dependent rules ----------------------------------------------

void thumbnailGatesAndCoalescing(SceneRenderer* audience) {
    PreviewProducer producer;
    producer.attachRenderer(nullptr, audience);  // no GPU: nothing here draws
    std::string error;
    PreviewProducer::ThumbnailAck ack;

    check(!producer.requestThumbnails(makeBatch({"a"}), 64, 64, 1, &ack, &error),
          "thumbnails refuse with no preview stream");

    check(producer.start(320, 180, 15, nullptr, &error), "start accepted");
    producer.applyStreamState();

    // Extent gates, in the operator's order: the box, then the slot. The
    // slot refusal is the one the box alone cannot see: 512x512 is a legal
    // thumbnail and an impossible one at 320x180.
    check(!producer.requestThumbnails(makeBatch({"a"}), 16, 16, 1, &ack, &error),
          "an extent under the box minimum is refused");
    check(!producer.requestThumbnails(makeBatch({"a"}), 513, 513, 1, &ack, &error),
          "an extent over the box maximum is refused");
    check(!producer.requestThumbnails(makeBatch({"a"}), 512, 512, 1, &ack, &error),
          "512x512 does not fit a 320x180 slot");
    check(error.find("does not fit") != std::string::npos,
          "and the refusal says which bound it hit");

    // Key validation and the batch cap: refusing beats truncating.
    std::vector<PreviewProducer::ThumbnailEntry> overCap;
    for (std::size_t i = 0; i <= protocol::kMaxThumbnailBatch; ++i) {
        overCap.push_back(makeBatch({"x"})[0]);
    }
    check(!producer.requestThumbnails(overCap, 64, 64, 1, &ack, &error),
          "a batch over the cap is refused outright");
    check(!producer.requestThumbnails(makeBatch({"a", ""}), 64, 64, 1, &ack, &error),
          "an empty key is refused");
    check(!producer.requestThumbnails(
              makeBatch({"a", std::string(protocol::kMaxThumbnailKeyLength + 1, 'k')}),
              64, 64, 1, &ack, &error),
          "an over-long key is refused");

    // THE GENERATION GATE, the batch's own: an empty batch is a stand-down
    // and a stand-down is a change, so it needs a new number too.
    check(producer.requestThumbnails({}, 64, 64, 1, &ack, &error),
          "an empty batch with a new request is accepted");
    check(!producer.requestThumbnails({}, 64, 64, 1, &ack, &error),
          "the same request again is refused even for a stand-down");
    check(!producer.requestThumbnails(makeBatch({"a"}), 64, 64, 1, &ack, &error),
          "the gate precedes everything else: a replayed real ask is refused too");

    // Accept a real band. Ids are minted per slide — non-zero, distinct,
    // and they keep advancing across bands; the counters answer queued vs
    // decodePending; the cache budget is the list's own 12 MiB.
    check(producer.requestThumbnails(makeBatch({"s1", "s2", "s3"}), 64, 64, 2, &ack, &error),
          "a band is accepted");
    check(ack.accepted.size() == 3 && ack.request == 2 && ack.width == 64 && ack.height == 64,
          "the ack answers every slide of the band");
    const uint32_t firstId = ack.accepted[0].second;
    check(firstId != 0 && ack.accepted[1].second != firstId &&
              ack.accepted[2].second != firstId &&
              ack.accepted[1].second != ack.accepted[2].second,
          "per-slide ids are non-zero and distinct");
    PreviewProducer::PreviewStatus status = producer.status();
    check(status.thumbnails.active && status.thumbnails.request == 2 &&
              status.thumbnails.queued == 3 && status.thumbnails.decodePending == 3,
          "the batch mirror counts the queue and the decode debt");
    check(status.thumbnails.budget == 12u * 1024u * 1024u,
          "the list's cache runs on its own 12 MiB budget, not the show's");

    // The companion exists (the band's build proved it), so a pump marks
    // all three no-image slides settled in one call — free is free.
    producer.pumpThumbnailDecode();
    status = producer.status();
    check(status.thumbnails.queued == 3 && status.thumbnails.decodePending == 0,
          "no-image slides settle without a decode");

    // The run loop's question with a settled head: owed, and the horizon is
    // a marker in the epoch — a frame is due now, not a sleep. This is the
    // 1.5e lesson in its extracted home.
    const auto now = std::chrono::steady_clock::now();
    const PreviewProducer::Wake wake = producer.previewWake(now, 0);
    check(wake.owed, "a settled thumbnail head is owed a frame");
    check(now >= wake.nextAllowed, "the first attempt is allowed immediately");
    // And with a queue whose head is NOT settled (a new band before any
    // pump), the same question must answer owed=false: the loop sleeps
    // until the pump settles it instead of spinning on a frame no codec
    // has produced yet.
    check(producer.requestThumbnails(makeBatch({"p"}), 64, 64, 3, &ack, &error),
          "a band that owes a settle replaces the old one");
    check(!producer.previewWake(now, 0).owed,
          "an unsettled head is not owed: no spin on a frame that cannot exist");

    // Replacement is the coalescing rule: the new band is the only debt.
    // (A fresh ack per request: the producer appends, so a reused one would
    // carry every band this call has ever answered.)
    PreviewProducer::ThumbnailAck tAck;
    check(producer.requestThumbnails(makeBatch({"t1", "t2"}), 64, 64, 4, &tAck, &error),
          "a newer band replaces the old wholesale");
    status = producer.status();
    check(status.thumbnails.queued == 2, "the superseded slides stopped being owed");
    check(tAck.accepted.size() == 2 && tAck.accepted[0].second != firstId,
          "the ids keep advancing across bands");

    // Stop is the wholesale stand-down: queue, companion, counters — gone.
    check(producer.stop(), "stop");
    check(!producer.previewWake(now, 0).owed, "a stopped stream owes nothing");
    status = producer.status();
    check(!status.thumbnails.active && status.thumbnails.queued == 0 &&
              status.thumbnails.decodePending == 0,
          "stop stands the list down completely");
    // And a client that asks again after the restart is not held hostage by
    // its own stale counter (stop reset it).
    check(producer.start(320, 180, 15, nullptr, &error), "restarted");
    check(producer.requestThumbnails(makeBatch({"again"}), 64, 64, 1, &ack, &error),
          "request 1 is accepted again after a stop reset the domain");
}

void lookaheadAndDecodes(SceneRenderer* audience) {
    PreviewProducer producer;
    producer.attachRenderer(nullptr, audience);
    std::string error;
    PreviewProducer::ThumbnailAck ack;

    const std::string dir = "/tmp/opencode/preview-producer-tests";
    ::mkdir(dir.c_str(), 0755);
    std::vector<std::string> pngs;
    for (int i = 0; i < 6; ++i) {
        const std::string path = dir + "/slide" + std::to_string(i) + ".png";
        if (!writePng(path, 64, 0xFF000000u + static_cast<uint32_t>(i) * 0x000100u)) {
            std::fprintf(stderr, "FAIL: cannot write PNG fixture %s\n", path.c_str());
            ++gFailures;
            return;
        }
        pngs.push_back(path);
    }

    check(producer.start(320, 180, 15, nullptr, &error), "start accepted");
    producer.applyStreamState();

    // Six slides, six distinct files, one decode per pump — and the
    // lookahead bound of four finished jobs ahead of the draw head. The
    // queue is only popped by a DRAW, and nothing draws here, so after
    // four finished jobs the fifth pump must stand down until the render
    // thread consumes: the decoded pictures in flight are the memory this
    // feature costs, and the bound is what makes a scrolling list flat.
    std::vector<PreviewProducer::ThumbnailEntry> batch;
    for (const std::string& path : pngs) {
        PreviewProducer::ThumbnailEntry entry;
        entry.key = path;
        entry.scene = makeImageScene(path, path);
        batch.push_back(std::move(entry));
    }
    check(producer.requestThumbnails(batch, 64, 64, 1, &ack, &error),
          "a band of image slides is accepted");
    check(producer.status().thumbnails.decodePending == 6, "all six owe a decode");

    bool fourDecoded = true;
    for (int i = 0; i < 4 && fourDecoded; ++i) {
        fourDecoded = producer.pumpThumbnailDecode();
    }
    check(fourDecoded, "each of the first four pumps decodes exactly one");
    check(producer.status().thumbnails.decodePending == 2, "four decodes leave a debt of two");
    check(!producer.pumpThumbnailDecode(),
          "the fifth pump stands down: the lookahead is a bound, not a suggestion");
    check(producer.status().thumbnails.decodePending == 2, "standing down decodes nothing");

    // A second band naming an already-decoded picture settles for free:
    // the cache answers and the codec is never asked (the draw's rule,
    // paid forward to the pump).
    std::vector<PreviewProducer::ThumbnailEntry> shared;
    for (int i = 0; i < 3; ++i) {
        PreviewProducer::ThumbnailEntry entry;
        entry.key = "shared" + std::to_string(i);
        entry.scene = makeImageScene(entry.key, pngs[0]);
        shared.push_back(std::move(entry));
    }
    check(producer.requestThumbnails(shared, 64, 64, 2, &ack, &error),
          "a band over an installed picture is accepted");
    check(producer.pumpThumbnailDecode(), "the cache settles them without opening the file");
    const PreviewProducer::PreviewStatus done = producer.status();
    check(done.thumbnails.decodePending == 0, "no decode debt after a cache-answered band");
    check(done.thumbnails.entries == 4, "the list's cache holds exactly the four decodes");
    check(done.thumbnails.decodedBytes > 0 &&
              done.thumbnails.decodedBytes <= 4u * 64u * 64u * 4u,
          "the list's bytes are bounded by the batch's own extent, not the file's");

    // A missing file settles too — as a failure, honestly: the slide draws
    // the colour fill and the queue never waits forever on a codec that
    // will not come.
    PreviewProducer::ThumbnailEntry missing;
    missing.key = "gone";
    missing.scene = makeImageScene("gone", dir + "/does-not-exist.png");
    check(producer.requestThumbnails({missing}, 64, 64, 3, &ack, &error),
          "a slide with a missing file is still accepted");
    check(producer.pumpThumbnailDecode(), "the failed decode settles the slide");
    check(producer.status().thumbnails.decodePending == 0,
          "a failed picture is settled, not pending");
}

void shrinkStandsTheBatchDown(SceneRenderer* audience) {
    PreviewProducer producer;
    producer.attachRenderer(nullptr, audience);
    std::string error;
    PreviewProducer::ThumbnailAck ack;

    check(producer.start(320, 180, 15, nullptr, &error), "start accepted");
    producer.applyStreamState();
    check(producer.requestThumbnails(makeBatch({"a", "b"}), 160, 90, 1, &ack, &error),
          "160x90 fits a 320x180 slot");

    // The stream shrinks under the standing batch. The segment is reserved
    // for its own frame size, so the honest answer is stand down and say so
    // in the counters — not overflow the slot, and not drop silently.
    check(producer.start(96, 64, 15, nullptr, &error), "the stream is replaced, smaller");
    producer.applyStreamState();
    const PreviewProducer::PreviewStatus status = producer.status();
    check(!status.thumbnails.active && status.thumbnails.queued == 0,
          "a batch the new stream cannot carry stands down on the apply");
    check(producer.status().running, "the smaller stream itself is live");
}

}  // namespace

int main() {
    geometryLimits();
    streamLifecycle();
    selectionGate();
    stopTearsEverythingDown();
    wakePolicyWithoutStream();
    if (gFailures > 0) {
        std::fprintf(stderr, "%d failure(s) in the font-free rules\n", gFailures);
        return 1;
    }

    auto renderer = std::make_unique<SceneRenderer>();
    std::string error;
    if (!renderer->create(&error)) {
        std::printf("SKIP: no usable typeface (%s)\n", error.c_str());
        return 77;
    }

    thumbnailGatesAndCoalescing(renderer.get());
    lookaheadAndDecodes(renderer.get());
    shrinkStandsTheBatchDown(renderer.get());

    if (gFailures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("preview_producer_tests: all checks passed\n");
    return 0;
}
