/*
 * SceneStore: the output's one scene truth (task 6.18a, review R1/R4/R10/R13).
 *
 * Everything an operator's command can change about WHAT the audience sees —
 * the prepared scenes and the ids minted for them, the revision counter that
 * acknowledges commands, the desired layer mask and destination, the queued
 * present and the transition it asked for — lives in this one object. Before
 * the extraction OutputSession held the same facts in nine members plus a
 * mirror of each in its Status, and the review's findings were all shadows of
 * that duplication: status published a scene before a frame drew it (R4)
 * because two fields claimed to be "the current scene" and the write order
 * decided which one won; a preview selection could resurrect a revision nobody
 * owned (R5) because the lookup lived three functions from the eviction that
 * removed it; preparation blocked the RPC thread (R10) because the store's
 * lock was the session's lock.
 *
 * The rules this class owns, in its own words:
 *
 *  - Revision 0 is never minted and never reused. It means "nothing" in every
 *    report, so a number the frontend was once given can never come back to
 *    mean a different scene.
 *  - Three kinds of state, never collapsed into one word. DESIRED is what the
 *    newest accepted command asked for. LANDED is what the current frame has
 *    staged: the renderer has been told, but the panel has not taken the
 *    frame. PUBLISHED is what a successfully presented frame proved, and it
 *    only ever moves in commit(). Deriving landed from published (or published
 *    from desired) is exactly the bug R4 describes: after a failed acquire the
 *    audience still has the old picture while the renderer already has the new
 *    one, and a component that cannot see the difference either credits a
 *    frame that was never shown or re-applies a scene the next frame needlessly.
 *  - Commands do not present. prepare() mints an id and moves no output
 *    revision; present() queues and moves no picture. Only the render loop's
 *    frame moves pixels, and only its present moves published state.
 *  - The store is bounded (kMaxPrepared), and eviction exempts exactly what can
 *    still be put on air or is being drawn right now: the desired scene and the
 *    landed scene. Anything else may go, and a caller holding a pinned copy is
 *    unaffected.
 *
 * Thread rules: every method takes the store's own mutex, so a command thread
 * and the render thread may call concurrently. The staged/landed fields are
 * written only by the thread the session designates
 * as the render thread (decide() and the land/commit pair); the command
 * methods are called from whatever thread the RPC server is on. The session
 * calls these methods while holding its own session mutex, so the lock order
 * is session mutex -> store mutex, never the reverse. The visitor passed to
 * forEachPrepared() must not re-enter the store or acquire the session mutex.
 * Command methods never call caller code, which lets a command thread hold
 * the store cheaply while the session's heavier work (decoding, transport
 * setup) happens OUTSIDE both locks, the way R10 asks for.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

#include "openshow/scene/scene.h"

#include "render/Layer.h"  // ActiveScene, SceneTransitionKind

namespace openshow::worker {

class SceneStore {
public:
    // How many prepared scenes the store keeps. The frontend's own history is
    // longer than this, so a lost revision means re-prepare rather than wait
    // for imagery that is never coming; the audience's scene never is a victim.
    static constexpr std::size_t kMaxPrepared = 16;

    // One immutable command/frame snapshot: everything owed as of the instant
    // decide() ran, and nothing that arrived later. The frame loop draws
    // exactly these values and its successful present credits exactly
    // snapshot.revision (task 6.4, review R4). Captured by the render thread,
    // read by nothing else.
    struct Snapshot {
        uint64_t revision = 0;
        uint32_t mask = 0;
        uint64_t scene = 0;  // store id, 0 = nothing presented
        SceneTransitionKind kind = SceneTransitionKind::kFade;
        int monitor = -1;
        // The scene's immutable content, shared with the store and previews.
        // Without the pin a prepare after this capture could
        // evict the snapshot's entry before the frame applied it — the store
        // race R4 hid behind an apply-time read of the desired scene.
        ActiveScene content;
    };

    // Why the loop owes a frame, and what that frame must draw. The two
    // questions are answered in one critical section on purpose: deciding from
    // one instant and capturing from another is how a command arrives between
    // the two and gets credited to a frame that drew its predecessor.
    struct Decision {
        // The newest accepted command is newer than the last presented frame.
        bool commandsDue = false;
        // The wanted destination is not the one the window is on. R4's own
        // reason to exist: a destination change repositions the window, which
        // is only on air when its frame presents.
        bool destinationDue = false;
        Snapshot snapshot;
    };

    // What a presented frame proved (task 6.4, review R4). This is the set
    // status() reports, and the only reason a frontend's "wait until
    // revision >= N" is a real wait for the compositor rather than a wait for
    // the worker to notice.
    struct Published {
        uint64_t revision = 0;   // the newest presented frame's revision
        uint32_t mask = 0;       // the layers that frame composited
        int monitor = -1;        // the destination that frame was drawn onto
        uint64_t activeScene = 0;  // the scene that frame carried (0 = black)
        uint64_t pendingScene = 0; // an accepted present no frame has carried yet
        bool transitioning = false;  // that frame was mid-crossfade
        std::size_t prepared = 0;    // how many scenes the store holds now
    };

    SceneStore() = default;

    SceneStore(const SceneStore&) = delete;
    SceneStore& operator=(const SceneStore&) = delete;

    // --- prepared scenes (command threads) ---

    // Compiles nothing and changes nothing on screen: it files the scene under
    // a freshly minted id and returns that id. No output revision moves and no
    // frame is provoked — only prepared() counts differ, which is honest: the
    // audience cannot see a scene that was merely handed over.
    uint64_t prepare(const openshow::scene::Scene& scene);

    // True when the id names a scene this store still holds. 0 never does.
    bool holds(uint64_t revision) const;
    // Shares the content of a held revision out (*out is null when it is not
    // held). A caller that intends to DRAW the revision takes a pin: entries
    // are immutable per id, so it shares the content the store has, and
    // it outlives any later eviction of this id.
    bool pin(uint64_t revision, ActiveScene* out) const;
    // Hands every held scene to `visit`, oldest id first, under the store lock.
    // The visitor must not re-enter this store or acquire the session mutex.
    // For a caller that must derive something from the whole held set (the
    // image-pin union, task 6.10) rather than look one id up.
    using SceneVisitor = std::function<void(const openshow::scene::Scene&)>;
    void forEachPrepared(const SceneVisitor& visit) const;
    std::size_t prepared() const;

    // --- command state (command threads) ---

    // Names a prepared revision as the output's scene. Refuses 0 and any id the
    // store does not hold — to go black, the frontend prepares the cleared
    // scene, which is an ordinary presentation. Queued state (pending) names
    // the accepted command until a presented frame carries its scene, so
    // re-presenting exactly what is confirmed on air starts no frame and queues
    // nothing. The transition kind travels with the change to the frame that
    // reveals it. *revisionOut always reports the current output revision, and
    // *changedOut says whether this ask moved the desired scene: a session that
    // keeps the store's contents pinned elsewhere (task 6.10) re-derives on a
    // change, not on every repeat of one that was already wanted.
    bool present(uint64_t preparedRevision, SceneTransitionKind kind, uint64_t* revisionOut,
                 bool* changedOut = nullptr);

    // Turns one layer on or off and bumps the output revision the
    // acknowledgement reports; the change lands on the next frame's mask,
    // never mid-frame.
    void setLayerBit(uint32_t bit, bool enabled, uint64_t* revisionOut);

    // Moves the output to a display: -1 is windowed and always valid, a panel
    // index must exist in `displayCount` (the snapshot GLFW refreshed on the
    // render thread). Refuses out-of-range indices, and an index the output is
    // already wanted on starts nothing — a repeat ask is not a new revision.
    bool selectMonitor(int index, std::size_t displayCount, uint64_t* revisionOut);

    // --- render thread: decide, stage, commit ---

    // The frame decision plus the snapshot it must draw, from one instant.
    void decide(Decision* out);

    // Seeds the desired/landed/published state at open(), when the first frame
    // has nothing to catch up with: the renderer's own layer mask and the
    // destination the window was mapped onto are true from the start.
    void seed(uint32_t mask, int monitor);

    // Stages what this frame applied. The renderer has been told; the panel has
    // not answered yet, so nothing here publishes.
    void landMask(uint32_t mask);
    void landMonitor(int monitor);
    // The destination vanished between validation and apply (a hotplug pulled
    // the panel): drop the request instead of retrying a doomed one every
    // frame. The published destination is untouched — the audience keeps the
    // window where the last presented frame has it.
    void abandonMonitor();
    void landScene(uint64_t revision);

    // The commit: a frame reached the compositor, so its snapshot's revision
    // and the state this frame staged are now what can be reported.
    // `transitioning` comes from the renderer (the blend this frame drew had
    // not settled), and pending settles only when the audience really has the
    // newest accepted scene. Returns the new published set for the caller to
    // report — this is the only function that moves published state.
    Published commit(uint64_t drawnRevision, bool transitioning);

    // --- reads ---

    uint64_t desiredScene() const;
    uint64_t landedScene() const;
    uint32_t desiredMask() const;
    uint32_t landedMask() const;
    int desiredMonitor() const;
    int landedMonitor() const;
    Published published() const;

private:
    mutable std::mutex fMutex;

    // The bounded store and the id counter. 0 is skipped forever: it is the
    // sentinel for "nothing" in every report, so a minted id must never be 0
    // and, once handed out, must never describe a different scene.
    std::map<uint64_t, ActiveScene> fScenes;
    uint64_t fNextSceneId = 1;

    // Desired: what the newest accepted command asked for.
    uint64_t fDesiredScene = 0;
    SceneTransitionKind fDesiredKind = SceneTransitionKind::kFade;
    uint32_t fDesiredMask = 0;
    int fDesiredMonitor = -1;
    uint64_t fRevision = 0;

    // Landed: what the current frame staged with the renderer.
    uint64_t fLandedScene = 0;
    uint32_t fLandedMask = 0;
    int fLandedMonitor = -1;

    // Published: what a presented frame proved.
    Published fPublished;
};

}  // namespace openshow::worker
