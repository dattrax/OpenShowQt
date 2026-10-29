/*
 * The store's rules (task 6.18a, reviews R1/R4/R5/R10/R13): one mutable scene
 * truth, and the three states that describe it are never collapsed into one.
 *
 * These fixtures drive the real SceneStore headless — no window, no Vulkan, no
 * display — because every rule below is arithmetic about commands and frames,
 * not about pixels. The findings the extraction closes out were all shadows of
 * OutputSession holding this state twice (a desired field and a Status mirror of
 * it), so each fixture attacks the duplication directly:
 *
 *  - R4 (status credits a frame that has not presented): published state moves
 *    ONLY in commit(), and only to the revision the drawn snapshot carried. A
 *    command that arrives after the capture is still owed afterwards; a failed
 *    present leaves the audience on the old scene with the command still
 *    queued; and the scene stays LANDED, which is why the next frame does not
 *    apply it a second time (the fault fixtures' armed presents depend on it).
 *  - R5 (a frontend names a revision it does not own): 0 is never minted, an id
 *    is never reused, and a lookup refuses what the store has let go — while a
 *    caller holding a pinned copy is unaffected by that eviction.
 *  - R10 (preparation blocks): prepare moves no output revision and provokes no
 *    frame, and the store's cap holds whatever the churn looks like.
 *  - R13 (a command mutates what is on screen): the transition kind belongs to
 *    the change that moved the desired scene, and is carried to the frame that
 *    reveals it.
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "openshow/scene/scene.h"
#include "session/SceneStore.h"

namespace {

using openshow::worker::ActiveScene;
using openshow::worker::SceneStore;
using openshow::worker::SceneTransitionKind;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

// A scene whose content is identifiable by pointer AND by value: the fixtures
// below have to tell "the store handed me entry 7's content" apart from "it
// handed me whatever its map held first", and equality on the compiled scene is
// the only honest way to ask.
openshow::scene::Scene makeScene(const std::string& tag) {
    openshow::scene::Scene scene;
    scene.slideId = tag;
    scene.background.color = 0x123456u;
    scene.background.hasImage = true;
    scene.background.assetPath = "/assets/" + tag + ".jpg";
    openshow::scene::SceneItem item;
    item.lines = {"line of " + tag};
    scene.items.push_back(item);
    return scene;
}

// --- rules about a store nobody has presented anything on yet ----------------
void freshStoreIsHonestlyEmpty() {
    SceneStore store;
    const SceneStore::Published published = store.published();
    check(published.revision == 0, "fresh: no revision has been acknowledged");
    check(published.activeScene == 0, "fresh: nothing is on air");
    check(published.pendingScene == 0, "fresh: nothing is queued");
    check(!published.transitioning, "fresh: no blend is running");
    check(published.prepared == 0, "fresh: the store holds nothing");
    check(store.prepared() == 0, "fresh: prepared() agrees");
    check(!store.holds(0), "fresh: revision 0 is not a scene");

    // prepare() files and mints; it moves no output revision and provokes no
    // frame (R10: handing the worker a scene is not an act of output).
    const std::uint64_t first = store.prepare(makeScene("a"));
    check(first != 0, "prepare: the minted id is never 0");
    check(store.published().revision == 0, "prepare: no output revision moved");
    check(store.published().activeScene == 0, "prepare: nothing went on air");
    check(store.published().pendingScene == 0, "prepare: nothing was queued");
    check(store.published().prepared == 1, "prepare: the count is the only report");

    std::uint64_t previous = first;
    bool monotonic = true;
    bool neverZero = true;
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t id = store.prepare(makeScene("b" + std::to_string(i)));
        monotonic = monotonic && id > previous;
        neverZero = neverZero && id != 0;
        previous = id;
    }
    check(monotonic, "prepare: ids are handed out in increasing order");
    check(neverZero, "prepare: no id is ever 0");
    check(store.prepared() == 6, "prepare: every scene is held");
    check(store.published().revision == 0, "prepare: six scenes, still no revision");
}

// --- who may be named -------------------------------------------------------
void onlyAHeldRevisionCanBePresented() {
    SceneStore store;
    const std::uint64_t id = store.prepare(makeScene("a"));

    constexpr std::uint64_t kUntouched = 0xAAu;
    std::uint64_t revision = kUntouched;
    bool changed = true;
    check(!store.present(0, SceneTransitionKind::kFade, &revision, &changed),
          "present: revision 0 is refused (black is a prepared cleared scene)");
    check(!store.present(id + 1000, SceneTransitionKind::kFade, &revision, &changed),
          "present: an id the store does not hold is refused");
    check(revision == kUntouched, "present: a refusal writes no acknowledgement");
    check(!store.holds(id + 1000), "holds: an unknown id is not held");
    check(!store.holds(0), "holds: revision 0 is held by nobody");

    ActiveScene pinned = std::make_shared<const openshow::scene::Scene>(makeScene("sentinel"));
    check(!store.pin(id + 1000, &pinned), "pin: an unknown id pins nothing");
    check(pinned == nullptr, "pin: a refused pin hands back null, not stale content");
    check(!store.pin(0, &pinned), "pin: revision 0 pins nothing");
    check(store.published().pendingScene == 0, "present: a refusal queued nothing");
    check(store.desiredScene() == 0, "present: a refusal desired nothing new");
    check(store.published().revision == 0, "present: a refusal spent no revision");

    // A held id is presentable, and the pin hands back that entry's content.
    check(store.present(id, SceneTransitionKind::kFade, &revision, &changed),
          "present: a held id is accepted");
    check(changed, "present: accepting it moved the desired scene");
    check(revision == 1, "present: the change is acknowledged with revision 1");
    ActiveScene content;
    check(store.pin(id, &content) && content && *content == makeScene("a"),
          "pin: a held id hands back its own content");
}

// --- the mask and the destination -------------------------------------------
void layerBitsAndRevisions() {
    SceneStore store;
    constexpr std::uint32_t kBit = 1u << 2;
    std::uint64_t revision = 0;

    store.setLayerBit(kBit, true, &revision);
    check(revision == 1, "layer: the first toggle is acknowledged with revision 1");
    check((store.desiredMask() & kBit) != 0, "layer: the bit is set in the desired mask");
    store.setLayerBit(kBit, false, &revision);
    check(revision == 2, "layer: the clear gets its own revision");
    check((store.desiredMask() & kBit) == 0, "layer: the bit is cleared");

    // An operator's command is credited with the frame that carries it, so a
    // repeat of a state that already holds still gets a revision of its own:
    // the ask is newer than anything the audience has been shown.
    store.setLayerBit(kBit, false, &revision);
    check(revision == 3, "layer: a repeat ask still moves the output revision");
    SceneStore::Decision decision;
    store.decide(&decision);
    check(decision.commandsDue, "layer: the loop owes the frame that proves it");
    check(decision.snapshot.mask == 0u, "layer: the snapshot carries the cleared mask");
    check(store.published().mask == 0u, "layer: the published mask waits for its frame");

    // Turning it on again and committing a frame is what makes it reportable.
    store.setLayerBit(kBit, true, &revision);
    store.decide(&decision);
    check(decision.snapshot.mask == kBit, "layer: the snapshot carries the set bit");
    store.landMask(decision.snapshot.mask);
    check(store.published().mask == 0u, "layer: staging the mask does not publish it");
    const SceneStore::Published published = store.commit(decision.snapshot.revision, false);
    check(published.mask == kBit, "layer: the commit publishes the mask it staged");
    check(published.revision == revision, "layer: and credits the frame's own revision");
    store.decide(&decision);
    check(!decision.commandsDue, "layer: nothing is owed once that frame is proved");
}

void destinationIsValidatedThenStaged() {
    SceneStore store;
    std::uint64_t revision = 0;

    // -1 is windowed and always valid; a panel index must exist in the snapshot
    // the caller holds (GLFW refreshes it on the render thread).
    check(store.selectMonitor(-1, 2, &revision), "monitor: windowed is always valid");
    check(revision == 0, "monitor: the destination the output already wanted is no new revision");
    check(store.selectMonitor(1, 2, &revision), "monitor: a panel inside the snapshot is valid");
    check(revision == 1, "monitor: a real move gets a revision");
    check(!store.selectMonitor(2, 2, &revision), "monitor: an index past the snapshot is refused");
    check(!store.selectMonitor(7, 1, &revision), "monitor: one display cannot hold index 7");
    check(!store.selectMonitor(0, 0, &revision), "monitor: no displays, no panels");
    check(!store.selectMonitor(-2, 2, &revision),
          "monitor: below windowed is refused (the auto sentinel is never a panel)");
    check(revision == 1, "monitor: refusals spend no revision");
    check(store.desiredMonitor() == 1, "monitor: the desired destination survived the refusals");

    SceneStore::Decision decision;
    store.decide(&decision);
    check(decision.destinationDue, "monitor: an unstaged destination owes a frame");
    check(decision.snapshot.monitor == 1, "monitor: the snapshot carries it");
    check(store.published().monitor == -1,
          "monitor: a move publishes nothing until a frame on that panel does");

    store.landMonitor(1);
    store.decide(&decision);
    check(!decision.destinationDue, "monitor: staging the move ends the debt");
    check(store.commit(decision.snapshot.revision, false).monitor == 1,
          "monitor: the frame that drew there makes it the audience's destination");

    // The hotplug case: the panel vanished between validation and apply. The
    // request is dropped rather than retried every frame, and the published
    // destination never moved, because no frame reached the other panel.
    check(store.selectMonitor(0, 2, &revision), "monitor: a fresh move is accepted");
    store.decide(&decision);
    check(decision.destinationDue, "monitor: the fresh move owes a frame");
    store.abandonMonitor();
    store.decide(&decision);
    check(!decision.destinationDue, "monitor: abandoning the request ends the debt");
    check(store.desiredMonitor() == store.landedMonitor(),
          "monitor: desired falls back to where the window really is");
    check(store.landedMonitor() == 1,
          "monitor: giving up a move moves the WANT, not the proof — the panel that "
          "vanished is not the panel the window reached");
    check(store.published().monitor == 1, "monitor: the audience keeps the proven destination");
}

// --- R4: the capture / stage / commit contract ------------------------------
void publishedStateOnlyMovesWithAPresentedFrame() {
    SceneStore store;
    store.seed(/*mask=*/0b00110, /*monitor=*/1);
    SceneStore::Decision decision;
    store.decide(&decision);
    check(!decision.commandsDue && !decision.destinationDue,
          "seed: a fresh output owes nothing it has not been asked for");
    check(decision.snapshot.mask == 0b00110 && decision.snapshot.monitor == 1,
          "seed: the renderer's own state is the starting snapshot");
    check(store.published().mask == 0b00110 && store.published().monitor == 1,
          "seed: mask and destination are true from the start");
    check(store.published().revision == 0, "seed: nothing has been presented yet");

    const std::uint64_t a = store.prepare(makeScene("a"));
    bool changed = false;
    check(store.present(a, SceneTransitionKind::kCut, nullptr, &changed) && changed,
          "present: naming a held scene changes the desired scene");
    const std::uint64_t owed = store.published().revision + 1;
    check(owed == 1, "present: the change is the first output revision");

    store.decide(&decision);
    check(decision.commandsDue, "present: the owed command provokes a frame");
    check(decision.snapshot.scene == a, "capture: the snapshot names the desired scene");
    check(decision.snapshot.kind == SceneTransitionKind::kCut,
          "capture: the transition travels with the change (R13)");
    check(decision.snapshot.content != nullptr, "capture: the scene content is pinned");
    check(*decision.snapshot.content == makeScene("a"), "capture: the pinned content IS the entry");

    // Queued, not on air: pending names the accepted command and active still
    // says nothing has ever been drawn.
    check(store.published().pendingScene == a, "queue: pending names the accepted command");
    check(store.published().activeScene == 0, "queue: nothing is confirmed on air");

    // A present that FAILS (the acquire or the submit) stages the scene but
    // publishes nothing — and, crucially, leaves it landed, so the next frame
    // does not apply the same scene again. Re-applying would re-arm the
    // scenePresentFails fixtures and reopen a transport the audience never saw.
    store.landScene(a);
    store.decide(&decision);
    check(decision.snapshot.scene == store.landedScene(),
          "failed present: the scene is already landed, so the next frame applies nothing");
    check(store.published().activeScene == 0,
          "failed present: the audience still has no scene, and status says so");
    check(store.published().pendingScene == a, "failed present: the command is still queued");
    check(store.published().revision == 0, "failed present: no revision is credited");

    // The frame that does reach the compositor credits exactly its snapshot.
    const SceneStore::Published published = store.commit(decision.snapshot.revision, false);
    check(published.revision == owed, "commit: the drawn snapshot's revision is what is credited");
    check(published.activeScene == a, "commit: the scene the frame drew is now on air");
    check(published.pendingScene == 0, "commit: the queue settles when the audience has it");
    check(!published.transitioning, "commit: a cut settled with the frame");
    store.decide(&decision);
    check(!decision.commandsDue, "commit: nothing is owed once the frame is proved");

    // Re-presenting exactly what is confirmed on air starts no frame, so it
    // queues nothing and spends no revision.
    const std::uint64_t beforeRevision = store.published().revision;
    check(store.present(a, SceneTransitionKind::kFade, nullptr, &changed) && !changed,
          "re-present: the confirmed scene was not 'changed'");
    check(store.published().pendingScene == 0, "re-present: nothing was queued");
    check(store.published().revision == beforeRevision,
          "re-present: no revision was spent on a frame nobody needs");

    // ...but while a blend runs the committed switch has not visually settled,
    // so presenting the scene the audience is watching DOES queue it again: the
    // switch the operator asked for is not yet the picture they have.
    store.commit(store.published().revision, /*transitioning=*/true);
    check(store.present(a, SceneTransitionKind::kFade, nullptr, &changed) && !changed,
          "mid-blend: the desired scene is unchanged");
    check(store.published().pendingScene == a,
          "mid-blend: re-presenting the on-air scene re-queues it until the blend settles");

    // A command that arrives AFTER the capture must not be credited to it.
    store.commit(store.published().revision, /*transitioning=*/false);
    check(store.published().pendingScene == 0, "late command: the settled queue is empty");
    store.decide(&decision);
    const std::uint64_t drawn = decision.snapshot.revision;
    check(!decision.commandsDue, "late command: the loop starts from a settled output");
    std::uint64_t ack = 0;
    store.setLayerBit(1u << 3, true, &ack);  // a bit nothing has ever staged
    check(ack == drawn + 1, "late command: the ask is acknowledged with a newer revision");
    const SceneStore::Published credited = store.commit(drawn, false);
    check(credited.revision == drawn,
          "late command: the frame credits its own snapshot, not the newest ask");
    check(credited.mask == store.landedMask() && (credited.mask & (1u << 3)) == 0,
          "late command: the mask it reports is the one it staged, not the one asked for");
    store.decide(&decision);
    check(decision.commandsDue, "late command: the newer ask is still owed after that commit");
    const SceneStore::Published settled = store.commit(decision.snapshot.revision, false);
    check(settled.revision == ack, "late command: the next frame credits it");
}

// A frame of the OLD scene while a NEW one is already queued: the commit has
// to tell landed from desired, because a frame proves the scene it DREW, not
// the one the operator asked for in the meantime. Publishing the wanted scene
// is R4's lie in its second form — "the audience has C" while the panel still
// shows B — and emptying the queue at the same moment swallows a command that
// no frame has ever carried. This is the shape the live S2 fixture lives for
// (B's apply parked at 400 ms with C landing inside it); pinned here the store
// owns the rule on its own, with no timing in it.
void aFrameOfTheOldSceneDoesNotCreditTheNewOne() {
    SceneStore store;
    const std::uint64_t b = store.prepare(makeScene("b"));
    const std::uint64_t c = store.prepare(makeScene("c"));
    store.present(b, SceneTransitionKind::kCut, nullptr, nullptr);
    store.landScene(b);
    const SceneStore::Published firstFrame = store.commit(/*drawnRevision=*/1, false);
    check(firstFrame.activeScene == b, "diverging: B's frame proves B");
    check(firstFrame.pendingScene == 0, "diverging: nothing is owed once B is on air");

    // C is asked for while the loop is still working on B's reveal.
    store.present(c, SceneTransitionKind::kFade, nullptr, nullptr);
    check(store.published().pendingScene == c, "diverging: C is queued while B is on air");

    // A further frame of B's snapshot — the crossfade's next blend. It proves B
    // again, and it must not be allowed to become C's proof.
    store.landScene(b);
    const SceneStore::Published fading = store.commit(/*drawnRevision=*/1, /*transitioning=*/true);
    check(fading.activeScene == b,
          "diverging: a frame of B reports B even though C is wanted (R4: the WANTED scene "
          "is not what the audience has)");
    check(fading.pendingScene == c,
          "diverging: C stays queued through it — a frame that did not draw a command "
          "cannot settle it");
    check(fading.transitioning, "diverging: the blend still running is honestly reported");

    // One more blend, still B: the debt is patient.
    const SceneStore::Published fadingAgain = store.commit(1, true);
    check(fadingAgain.activeScene == b && fadingAgain.pendingScene == c,
          "diverging: the second blend still reports B with C owed");

    // Then the frame that really carries C, and only then does the queue close.
    store.landScene(c);
    const SceneStore::Published drawn = store.commit(/*drawnRevision=*/2, /*transitioning=*/true);
    check(drawn.activeScene == c, "diverging: C is on air when a frame carries it");
    check(drawn.pendingScene == 0, "diverging: and the queue settles only then");
    store.landScene(c);
    check(store.commit(2, false).pendingScene == 0,
          "diverging: a settled frame of the on-air scene keeps the queue empty");
}

// --- R13: the reveal belongs to the change, not to the last ask -------------
void transitionKindTravelsWithTheChange() {
    SceneStore store;
    const std::uint64_t a = store.prepare(makeScene("a"));
    const std::uint64_t b = store.prepare(makeScene("b"));
    bool changed = false;
    store.present(a, SceneTransitionKind::kFade, nullptr, &changed);
    store.present(b, SceneTransitionKind::kCut, nullptr, &changed);
    SceneStore::Decision decision;
    store.decide(&decision);
    check(decision.snapshot.scene == b, "kind: the newest present names the scene");
    check(decision.snapshot.kind == SceneTransitionKind::kCut, "kind: and its reveal");

    // A repeat ask for the same scene under a different kind changes nothing:
    // the kind belongs to the change that moved the desired scene. Silently
    // re-writing it would restage a reveal the frame may already be drawing.
    store.present(b, SceneTransitionKind::kFade, nullptr, &changed);
    check(!changed, "kind: a repeat ask changes nothing");
    store.decide(&decision);
    check(decision.snapshot.kind == SceneTransitionKind::kCut,
          "kind: the reveal of a settled change is not rewritten by a repeat");
}

// --- the bounded store, and what eviction may not touch ---------------------
void evictionExemptsWhatCanBePutOnAir() {
    SceneStore store;
    const std::uint64_t landed = store.prepare(makeScene("on-air"));
    store.present(landed, SceneTransitionKind::kCut, nullptr, nullptr);
    store.landScene(landed);
    const std::uint64_t queued = store.prepare(makeScene("queued"));
    store.present(queued, SceneTransitionKind::kFade, nullptr, nullptr);

    // Churn far past the cap. The exemptions are the DESIRED scene (its command
    // is accepted and its frame has not run) and the LANDED scene (the renderer
    // is drawing it and a fade may still be blending from it).
    std::uint64_t newest = queued;
    std::uint64_t firstChurn = 0;
    bool capHeld = true;
    bool neverReused = true;
    for (int i = 0; i < 60; ++i) {
        newest = store.prepare(makeScene("churn" + std::to_string(i)));
        if (firstChurn == 0) {
            firstChurn = newest;
        }
        capHeld = capHeld && store.prepared() <= SceneStore::kMaxPrepared;
        neverReused = neverReused && newest > queued;
    }
    check(capHeld, "evict: the cap holds through the churn");
    check(store.prepared() == SceneStore::kMaxPrepared, "evict: the store sits at its cap");
    check(neverReused, "evict: eviction never recycles an id back into circulation");
    check(store.holds(landed), "evict: the landed scene is never a victim");
    check(store.holds(queued), "evict: the queued scene is never a victim");
    check(store.holds(newest), "evict: the newest entry is held");
    check(!store.holds(firstChurn), "evict: the oldest eligible entry is gone");

    // The exempt scenes are still presentable; the evicted one is refused the
    // ordinary way: re-prepare rather than wait for imagery never coming.
    check(store.present(landed, SceneTransitionKind::kFade, nullptr, nullptr),
          "evict: the landed scene can still be presented");
    check(!store.present(firstChurn, SceneTransitionKind::kFade, nullptr, nullptr),
          "evict: an evicted id is refused, not silently ignored");
}

void walkVisitsExactlyWhatIsHeld() {
    SceneStore store;
    for (int i = 0; i < 24; ++i) {
        char tag[8];
        std::snprintf(tag, sizeof tag, "%02d", i);
        store.prepare(makeScene(tag));
    }
    std::vector<std::string> tags;
    store.forEachPrepared([&tags](const openshow::scene::Scene& held) {
        tags.push_back(held.slideId);
    });
    check(tags.size() == store.prepared(), "walk: every held scene, and only those");
    check(store.prepared() == SceneStore::kMaxPrepared, "walk: 24 prepared leaves the cap's 16");
    check(!tags.empty() && tags.front() == "08", "walk: the eight evicted elders are not visited");
    check(tags.back() == "23", "walk: the newest entry is visited");
    bool ascending = true;
    for (std::size_t i = 1; i < tags.size(); ++i) {
        ascending = ascending && tags[i - 1] < tags[i];
    }
    check(ascending, "walk: oldest id first");
}

// --- a pinned instance outlives the store's memory of it --------------------
void pinnedContentOutlivesEviction() {
    SceneStore store;
    auto source = makeScene("a");
    const std::uint64_t a = store.prepare(source);
    source.items.front().lines = {"edited after prepare"};
    ActiveScene preview;
    ActiveScene repeatedPreview;
    check(store.pin(a, &preview) && store.pin(a, &repeatedPreview),
          "pin: the prepared revision is available to previews");
    check(preview && *preview == makeScene("a"),
          "prepare: later source edits cannot change stored content");
    check(preview && preview == repeatedPreview,
          "pin: repeated preview selections share one immutable instance");
    store.present(a, SceneTransitionKind::kFade, nullptr, nullptr);

    SceneStore::Decision first;
    store.decide(&first);
    SceneStore::Decision second;
    store.decide(&second);
    check(first.snapshot.content.get() == second.snapshot.content.get(),
          "capture: repeated captures of one scene id share one instance");
    check(first.snapshot.content == preview,
          "capture: output and selected preview share scene and layout identity");
    store.forEachPrepared([&](const openshow::scene::Scene& scene) {
        check(&scene == preview.get(), "walk: visits the same immutable stored instance");
    });
    check(first.snapshot.content.use_count() >= 3, "capture: the pin is a real shared reference");

    // Move on, then churn the store past its cap so entry `a` is evicted for
    // real. The snapshots already taken still hold the content they were
    // captured with — that is the whole reason the capture pins.
    const ActiveScene held = first.snapshot.content;
    const std::uint64_t b = store.prepare(makeScene("b"));
    store.present(b, SceneTransitionKind::kCut, nullptr, nullptr);
    for (int i = 0; i < 40; ++i) {
        store.prepare(makeScene("churn" + std::to_string(i)));
    }
    check(!store.holds(a), "capture: the old entry really left the store");
    check(held != nullptr && *held == makeScene("a"),
          "capture: a snapshot's pinned content survives its entry's eviction");
    SceneStore::Decision third;
    store.decide(&third);
    check(third.snapshot.content.get() != held.get(), "capture: a new scene id gets a new copy");
    check(third.snapshot.content && third.snapshot.content->slideId == "b", "capture: and it is the new scene");
    check(store.published().prepared == store.prepared(), "capture: the count tracks the churn");

    // Nothing presented: the capture's content is null, and the black clear
    // stands on its own.
    SceneStore empty;
    SceneStore::Decision decision;
    empty.decide(&decision);
    check(decision.snapshot.scene == 0, "capture: an output that has never presented names 0");
    check(decision.snapshot.content == nullptr, "capture: and pins nothing");
}

// --- the two threads the store is designed for ------------------------------
void commandAndRenderThreadsConcurrently() {
    // The documented contract: a handler thread runs the command methods while
    // the render thread decides, stages and commits. Nothing here can crash or
    // double-count — the store's own mutex is the only lock, and it calls
    // nothing back.
    SceneStore store;
    std::atomic<bool> done{false};
    std::atomic<bool> sawZeroId{false};
    std::atomic<bool> sawOverCap{false};
    std::atomic<bool> wentBackwards{false};
    std::atomic<std::uint64_t> lastPublished{0};

    std::thread commands([&] {
        for (int i = 0; i < 4000 && !done; ++i) {
            const std::uint64_t id = store.prepare(makeScene("c" + std::to_string(i)));
            if (id == 0) {
                sawZeroId = true;
            }
            store.present(id, (i & 1) ? SceneTransitionKind::kCut : SceneTransitionKind::kFade,
                          nullptr, nullptr);
            if (i % 97 == 0) {
                store.setLayerBit(1u << (i % 4), (i & 8) != 0, nullptr);
            }
            if (i % 211 == 0) {
                store.selectMonitor(i % 2, 2, nullptr);
            }
            if (store.prepared() > SceneStore::kMaxPrepared) {
                sawOverCap = true;
            }
        }
    });
    std::thread render([&] {
        for (int i = 0; i < 4000 && !done; ++i) {
            SceneStore::Decision decision;
            store.decide(&decision);
            if (decision.commandsDue || decision.destinationDue) {
                store.landMask(decision.snapshot.mask);
                store.landMonitor(decision.snapshot.monitor);
                store.landScene(decision.snapshot.scene);
                const SceneStore::Published published =
                    store.commit(decision.snapshot.revision, false);
                // Published state may only ever move forward: it is the record
                // of frames that happened, and a frame cannot un-happen.
                const std::uint64_t previous = lastPublished.exchange(published.revision);
                if (published.revision < previous) {
                    wentBackwards = true;
                }
            }
        }
        done = true;
    });
    commands.join();
    render.join();

    check(!sawZeroId, "threads: 0 is never minted, even under contention");
    check(!sawOverCap, "threads: the cap holds while a command thread churns");
    check(!wentBackwards, "threads: published revision never moves backwards");
    check(store.published().revision > 0, "threads: frames did credit commands");
    check(store.prepared() <= SceneStore::kMaxPrepared, "threads: the final store is bounded");
    check(store.holds(store.desiredScene()),
          "threads: the newest accepted scene is still presentable");
}

}  // namespace

int main() {
    freshStoreIsHonestlyEmpty();
    onlyAHeldRevisionCanBePresented();
    layerBitsAndRevisions();
    destinationIsValidatedThenStaged();
    publishedStateOnlyMovesWithAPresentedFrame();
    aFrameOfTheOldSceneDoesNotCreditTheNewOne();
    transitionKindTravelsWithTheChange();
    evictionExemptsWhatCanBePutOnAir();
    walkVisitsExactlyWhatIsHeld();
    pinnedContentOutlivesEviction();
    commandAndRenderThreadsConcurrently();

    if (gFailures == 0) {
        std::printf("scene_store_tests: all rules held\n");
        return 0;
    }
    std::fprintf(stderr, "scene_store_tests: %d failure(s)\n", gFailures);
    return 1;
}
