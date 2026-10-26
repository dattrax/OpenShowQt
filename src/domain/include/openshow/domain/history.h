/*
 * Undo/redo for the document model (task 2.3).
 *
 * Strategy: whole-state snapshots. Documents is plain value-semantic
 * data, so copying it is cheap at this scale (a service is text, not
 * media — asset bytes live in files), and a snapshot cannot drift out
 * of sync with the model the way hand-written inverse operations do.
 * The stack is bounded (kMaxDepth); the oldest entries drop.
 *
 * History observes one Documents through a two-stack discipline:
 * apply() runs an edit and, only if the edit reports success, pushes
 * the pre-edit state and clears the redo stack — an edit made after
 * undoing branches the history and the abandoned branch is discarded,
 * matching every text editor an operator has ever used. A refused
 * edit (the 2.1 failure contract: false, model untouched) is invisible
 * to both stacks.
 *
 * Ids: restoring a snapshot rolls the store's id counter back, which
 * would let an undone creation's id be reissued. History keeps a
 * monotonic watermark of the counter instead, so the 2.1 promise that
 * ids never recycle holds across undo — an undone edit vanishes
 * without leaving a stale id that some later object could resurrect.
 * (For this to hold, every edit on the observed store goes through
 * apply(); direct operation calls bypass history entirely.)
 */

#pragma once

#include <cstddef>
#include <deque>
#include <string>

#include "openshow/domain/documents.h"

namespace openshow::domain {

class History {
public:
    // States older than this are dropped (oldest first). Generous for a
    // text-only model: a whole service is a few kilobytes.
    static constexpr std::size_t kMaxDepth = 128;

    // History must outlive its Documents (it holds a pointer, and
    // snapshots are independent copies).
    explicit History(Documents* documents);

    // Runs one edit against the observed store. `edit` returns true
    // when it changed something — the domain operations do exactly
    // that, so wrapping is `history.apply([&] { return docs.foo(); })`.
    // Success pushes the pre-edit state and clears the redo stack;
    // refusal changes nothing, including the stacks.
    template <typename Edit>
    bool apply(Edit edit) {
        Documents before = *fDocuments;
        if (!edit()) {
            return false;
        }
        bumpWatermark();
        fUndo.push_back(std::move(before));
        if (fUndo.size() > kMaxDepth) {
            fUndo.pop_front();
        }
        fRedo.clear();
        return true;
    }

    // Reverse / replay the most recent (un)done edit. Both fail with an
    // error when there is nothing to do.
    bool undo(std::string* error = nullptr);
    bool redo(std::string* error = nullptr);

    // Retargets asset paths inside every snapshot (task 6.12, review R7).
    // A save that adopted the package-owned asset files deletes the scratch
    // tree it may have loaded from; an undo afterwards must not hand the
    // model back a path into storage that no longer exists. Snapshots follow
    // the same rule as Documents::adoptAssets: same id, same old path.
    void adoptAssets(const std::vector<AssetRebind>& rebinds);

    bool canUndo() const { return !fUndo.empty(); }
    bool canRedo() const { return !fRedo.empty(); }
    std::size_t undoDepth() const { return fUndo.size(); }
    std::size_t redoDepth() const { return fRedo.size(); }

    // Drops both stacks (opening a different service). The id watermark
    // survives and absorbs the current counter, so ids stay monotonic
    // across a load of a file whose ids happen to be lower.
    void clear();

private:
    void bumpWatermark();

    Documents* fDocuments;
    std::deque<Documents> fUndo;  // pre-edit states, newest last
    std::deque<Documents> fRedo;  // states pushed aside by undo, newest last
    unsigned long long fWatermark = 1;  // highest counter ever observed
};

}  // namespace openshow::domain
