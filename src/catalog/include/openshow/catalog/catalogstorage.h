#pragma once

#include <filesystem>

#include "openshow/catalog/catalog.h"

namespace openshow::catalog {

// The catalog's own package: `<directory>/catalog.json`. It lives under the
// application's data directory, never inside a service package, so a
// service can be saved, loaded, copied or played without it while the
// Library survives.
//
// The write is a COMMIT in the domain storage's sense: the manifest is
// written to a temporary in the same directory, synced, renamed over
// catalog.json, and the directory is synced before success is reported.
// A failed save names the step it failed at and leaves the previous catalog
// readable. `OPENSHOW_CATALOG_FAULT` (write/rename/sync) injects per-step
// refusals so fixtures can prove the failure contract without killing a
// process.
//
// The read is strict: an unknown format, an unknown key, a bad id, a
// duplicate id, an unsafe category/kind or invalid song content fails with
// a message naming the problem and leaves *catalog untouched. A catalog the
// read accepts is a catalog every operation can honour.
bool saveCatalog(const Catalog& catalog, const std::filesystem::path& directory,
                 std::string* error = nullptr);

bool loadCatalog(Catalog* catalog, const std::filesystem::path& directory,
                 std::string* error = nullptr);

}  // namespace openshow::catalog
