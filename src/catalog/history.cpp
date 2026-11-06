#include "openshow/catalog/history.h"

#include <algorithm>

namespace openshow::catalog {

namespace {

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

History::History(Catalog* catalog) : fCatalog(catalog) { bumpWatermark(); }

bool History::undo(std::string* error) {
    bumpWatermark();  // absorb any counter movement before restoring
    if (fUndo.empty()) {
        fail(error, "there is nothing to undo in the Library");
        return false;
    }
    fRedo.push_back(*fCatalog);
    *fCatalog = std::move(fUndo.back());
    fUndo.pop_back();
    fCatalog->fNextOrdinal = fWatermark;
    return true;
}

bool History::redo(std::string* error) {
    bumpWatermark();
    if (fRedo.empty()) {
        fail(error, "there is nothing to redo in the Library");
        return false;
    }
    fUndo.push_back(*fCatalog);
    *fCatalog = std::move(fRedo.back());
    fRedo.pop_back();
    fCatalog->fNextOrdinal = fWatermark;
    return true;
}

void History::recordApplied(Catalog before) {
    bumpWatermark();
    fUndo.push_back(std::move(before));
    if (fUndo.size() > kMaxDepth) {
        fUndo.pop_front();
    }
    fRedo.clear();
    bumpWatermark();
}

void History::clear() {
    fUndo.clear();
    fRedo.clear();
    bumpWatermark();
}

void History::bumpWatermark() {
    fWatermark = std::max(fWatermark, fCatalog->fNextOrdinal);
}

}  // namespace openshow::catalog
