/*
 * Undo/redo for the shared catalog (task SL.2).
 *
 * The same two-stack, whole-state discipline domain::History uses for a
 * service, for the same reasons: Catalog is plain value data, a snapshot
 * cannot drift from the model, and a refused edit is invisible to both
 * stacks. Ids stay monotonic across undo through the watermark below, so
 * an undone creation can never have its id reissued to a later item.
 *
 * The catalog keeps its own history, separate from the current service's:
 * editing the Library must not put an undo step on the running order, and
 * editing a service copy must not silently rewrite the Library. Callers
 * choose the store, and this is the Library's.
 */

#pragma once

#include <cstddef>
#include <deque>
#include <string>

#include "openshow/catalog/catalog.h"

namespace openshow::catalog {

class History {
public:
    // Generous for text-only records; the oldest entries drop first.
    static constexpr std::size_t kMaxDepth = 128;

    // Catalog must outlive its History (it holds a pointer).
    explicit History(Catalog* catalog);

    // Runs one edit; success pushes the pre-edit state and clears redo.
    template <typename Edit>
    bool apply(Edit edit) {
        Catalog before = *fCatalog;
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

    bool undo(std::string* error = nullptr);
    bool redo(std::string* error = nullptr);
    // Records an edit whose RESULT is already in the observed catalog and
    // whose pre-edit state the caller kept. The durable-write adapter uses
    // this: it applies the edit to a copy, commits that copy to disk, and
    // only then publishes it (and this history entry) — so a failed write
    // never leaves an undo step for a change the file does not hold.
    void recordApplied(Catalog before);
    bool canUndo() const { return !fUndo.empty(); }
    bool canRedo() const { return !fRedo.empty(); }
    std::size_t undoDepth() const { return fUndo.size(); }
    std::size_t redoDepth() const { return fRedo.size(); }

    // Drops both stacks (loading a different catalog/restarting a session).
    void clear();
    // Permanent Library deletion must not be reversed by an older snapshot.
    void forgetItems(const std::vector<Id>& ids);

private:
    void bumpWatermark();

    Catalog* fCatalog;
    std::deque<Catalog> fUndo;
    std::deque<Catalog> fRedo;
    unsigned long long fWatermark = 1;
};

}  // namespace openshow::catalog
