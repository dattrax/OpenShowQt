/*
 * The two-stack half of History — see history.h. The apply() template
 * lives in the header; everything that touches the observed store is
 * here.
 */

#include "openshow/domain/history.h"

#include <algorithm>

namespace openshow::domain {

namespace {

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

History::History(Documents* documents) : fDocuments(documents) {
    bumpWatermark();
}

bool History::undo(std::string* error) {
    bumpWatermark();  // absorb any counter movement before restoring
    if (fUndo.empty()) {
        fail(error, "there is nothing to undo");
        return false;
    }
    fRedo.push_back(*fDocuments);
    *fDocuments = std::move(fUndo.back());
    fUndo.pop_back();
    // The restored snapshot carries the counter of its time; raise it
    // back so no id this session already handed out comes around again.
    fDocuments->fNextOrdinal = fWatermark;
    return true;
}

bool History::redo(std::string* error) {
    bumpWatermark();
    if (fRedo.empty()) {
        fail(error, "there is nothing to redo");
        return false;
    }
    fUndo.push_back(*fDocuments);
    *fDocuments = std::move(fRedo.back());
    fRedo.pop_back();
    fDocuments->fNextOrdinal = fWatermark;
    return true;
}

void History::clear() {
    fUndo.clear();
    fRedo.clear();
    bumpWatermark();
}

void History::adoptAssets(const std::vector<AssetRebind>& rebinds) {
    for (Documents& snapshot : fUndo) {
        snapshot.adoptAssets(rebinds);
    }
    for (Documents& snapshot : fRedo) {
        snapshot.adoptAssets(rebinds);
    }
}

void History::bumpWatermark() {
    fWatermark = std::max(fWatermark, fDocuments->fNextOrdinal);
}

}  // namespace openshow::domain
