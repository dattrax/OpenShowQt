/*
 * SceneStore implementation — see SceneStore.h for the rules and the thread
 * contract. Every function here is the whole of its subject: there is no other
 * place in the worker where a scene id is minted, an output revision advanced,
 * or published state moves.
 */

#include "session/SceneStore.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace openshow::worker {

uint64_t SceneStore::prepare(const openshow::scene::Scene& scene) {
    std::lock_guard<std::mutex> lock(fMutex);
    const uint64_t id = fNextSceneId++;
    fScenes[id] = scene;
    // Bounded store: evict the oldest revision nobody can present again. The
    // desired id is exempt because its command has been accepted and the frame
    // that reveals it has not run yet; the landed id is exempt because it is
    // what the renderer is drawing — evicting it would drop the content copy
    // a fade may still be blending from. Nothing else is sacred: a frontend
    // that lost its revision number has to re-prepare anyway.
    while (fScenes.size() > kMaxPrepared) {
        const auto victim = std::find_if(fScenes.begin(), fScenes.end(),
                                         [&](const auto& entry) {
                                             return entry.first != fDesiredScene &&
                                                    entry.first != fLandedScene;
                                         });
        if (victim == fScenes.end()) {
            break;  // every entry is exempt: the cap is exceeded, never violated by force
        }
        fScenes.erase(victim);
    }
    // The count is what a caller can see of a prepare: it moved no output
    // revision and provoked no frame, and status().scene.prepared is the only
    // number that changed.
    fPublished.prepared = fScenes.size();
    return id;
}

bool SceneStore::holds(uint64_t revision) const {
    std::lock_guard<std::mutex> lock(fMutex);
    return revision != 0 && fScenes.find(revision) != fScenes.end();
}

bool SceneStore::pin(uint64_t revision, ActiveScene* out) const {
    std::lock_guard<std::mutex> lock(fMutex);
    const auto entry = fScenes.find(revision);
    if (revision == 0 || entry == fScenes.end()) {
        if (out != nullptr) {
            *out = nullptr;
        }
        return false;
    }
    // A copy of the entry, shared with whoever asks: entries are immutable per
    // id, so this is the same content, and it survives this store letting the
    // id go.
    if (out != nullptr) {
        *out = std::make_shared<const openshow::scene::Scene>(entry->second);
    }
    return true;
}

void SceneStore::forEachPrepared(const SceneVisitor& visit) const {
    std::lock_guard<std::mutex> lock(fMutex);
    for (const auto& entry : fScenes) {
        visit(entry.second);
    }
}

std::size_t SceneStore::prepared() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fScenes.size();
}

bool SceneStore::present(uint64_t preparedRevision, SceneTransitionKind kind,
                         uint64_t* revisionOut, bool* changedOut) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (preparedRevision == 0 || fScenes.find(preparedRevision) == fScenes.end()) {
        return false;
    }
    // Queued state (task 6.4): pending names the accepted command until a
    // presented frame carries its scene. Re-presenting exactly what is
    // confirmed on air starts no frame — no revision, no blend, no queue entry.
    const bool alreadyConfirmed = fPublished.activeScene == preparedRevision &&
                                  fDesiredScene == preparedRevision && !fPublished.transitioning;
    fPublished.pendingScene = alreadyConfirmed ? 0 : preparedRevision;
    const bool changed = preparedRevision != fDesiredScene;
    if (changed) {
        fDesiredScene = preparedRevision;
        fDesiredKind = kind;  // travels with the change to the frame that reveals it
        ++fRevision;
    }
    if (changedOut != nullptr) {
        *changedOut = changed;
    }
    if (revisionOut != nullptr) {
        *revisionOut = fRevision;
    }
    return true;
}

void SceneStore::setLayerBit(uint32_t bit, bool enabled, uint64_t* revisionOut) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (enabled) {
        fDesiredMask |= bit;
    } else {
        fDesiredMask &= ~bit;
    }
    // The revision moves even when the bit was already in the asked-for state:
    // an operator's command is acknowledged with the revision whose frame will
    // carry it, and "it was already on" is not a reason to hand back a revision
    // the audience reached before the ask.
    ++fRevision;
    if (revisionOut != nullptr) {
        *revisionOut = fRevision;
    }
}

bool SceneStore::selectMonitor(int index, std::size_t displayCount, uint64_t* revisionOut) {
    std::lock_guard<std::mutex> lock(fMutex);
    // -1 is the windowed destination and always valid; a panel index must exist
    // in the snapshot (which GLFW refreshed on the render thread).
    if (index < -1 || index >= static_cast<int>(displayCount)) {
        return false;
    }
    if (index != fDesiredMonitor) {
        fDesiredMonitor = index;
        ++fRevision;
    }
    if (revisionOut != nullptr) {
        *revisionOut = fRevision;
    }
    return true;
}

void SceneStore::decide(Decision* out) {
    std::lock_guard<std::mutex> lock(fMutex);
    // Both questions answered from the same instant as the capture below: a
    // decision from one moment and a snapshot from the next is how a command
    // gets credited to a frame that drew its predecessor.
    out->commandsDue = fRevision != fPublished.revision;
    out->destinationDue = fDesiredMonitor != fLandedMonitor;
    out->snapshot.revision = fRevision;
    out->snapshot.mask = fDesiredMask;
    out->snapshot.scene = fDesiredScene;
    out->snapshot.kind = fDesiredKind;
    out->snapshot.monitor = fDesiredMonitor;
    if (out->snapshot.scene != fCapturedSceneId) {
        // Entries are immutable per id and the desired id is eviction-exempt,
        // so the lookup under this same lock is the one pin the snapshot needs
        // (shared across all captures of one scene id).
        const auto entry = fScenes.find(out->snapshot.scene);
        fCapturedScene = entry != fScenes.end()
                             ? std::make_shared<const openshow::scene::Scene>(entry->second)
                             : nullptr;
        fCapturedSceneId = out->snapshot.scene;
    }
    out->snapshot.content = fCapturedScene;
}

void SceneStore::seed(uint32_t mask, int monitor) {
    std::lock_guard<std::mutex> lock(fMutex);
    fDesiredMask = mask;
    fLandedMask = mask;
    fDesiredMonitor = monitor;
    fLandedMonitor = monitor;
    fPublished.mask = mask;
    fPublished.monitor = monitor;
    // The published revision/scene stay 0: open() has not presented anything,
    // and "nothing on air at revision 0" is exactly what the first frame has to
    // beat.
}

void SceneStore::landMask(uint32_t mask) {
    std::lock_guard<std::mutex> lock(fMutex);
    fLandedMask = mask;
}

void SceneStore::landMonitor(int monitor) {
    std::lock_guard<std::mutex> lock(fMutex);
    fLandedMonitor = monitor;
}

void SceneStore::abandonMonitor() {
    std::lock_guard<std::mutex> lock(fMutex);
    fDesiredMonitor = fLandedMonitor;
}

void SceneStore::landScene(uint64_t revision) {
    std::lock_guard<std::mutex> lock(fMutex);
    fLandedScene = revision;
}

SceneStore::Published SceneStore::commit(uint64_t drawnRevision, bool transitioning) {
    std::lock_guard<std::mutex> lock(fMutex);
    // The commit (task 6.4, review R4): applied state is published only from a
    // frame that reached the compositor, and it describes exactly what that
    // frame drew — the staged landed fields, all written by this frame's apply
    // pass, plus the snapshot's revision. Queued state is recomputed against the
    // newest accepted command, so it settles only when the audience really has
    // it; transitioning says the committed switch has not visually settled.
    fPublished.revision = drawnRevision;
    fPublished.mask = fLandedMask;
    fPublished.monitor = fLandedMonitor;
    fPublished.activeScene = fLandedScene;
    fPublished.transitioning = transitioning;
    fPublished.pendingScene = fDesiredScene == fLandedScene ? 0 : fDesiredScene;
    fPublished.prepared = fScenes.size();
    return fPublished;
}

uint64_t SceneStore::desiredScene() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fDesiredScene;
}

uint64_t SceneStore::landedScene() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fLandedScene;
}

uint32_t SceneStore::desiredMask() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fDesiredMask;
}

uint32_t SceneStore::landedMask() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fLandedMask;
}

int SceneStore::desiredMonitor() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fDesiredMonitor;
}

int SceneStore::landedMonitor() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fLandedMonitor;
}

SceneStore::Published SceneStore::published() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fPublished;
}

}  // namespace openshow::worker
