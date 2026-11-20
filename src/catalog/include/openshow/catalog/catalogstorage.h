#pragma once

#include <filesystem>
#include <map>
#include <string_view>
#include <vector>

#include "openshow/catalog/catalog.h"
#include "openshow/domain/thumbnails.h"

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
// TC.9: the Library's derived card pictures. They live beside the catalog
// rather than inside its model, so a cache arrival can bump no record/content
// revision, add no history entry and trigger no re-preparation. Identity is the
// render fingerprint alone; a prepared service's new slide ids are bound when
// the bridge copies a variant into that service.
struct CatalogThumbnailCache {
    unsigned long long format = 1;
    std::map<Id, std::vector<openshow::domain::ThumbnailCacheEntry>> variants;
};

bool saveCatalog(const Catalog& catalog, const std::filesystem::path& directory,
                 std::string* error = nullptr,
                 const CatalogThumbnailCache* variants = nullptr);

bool loadCatalog(Catalog* catalog, const std::filesystem::path& directory,
                 std::string* error = nullptr, CatalogThumbnailCache* variants = nullptr);

// The authored half's identity (canonical, thumbnail-block-free). A Library
// cache flush compares it so an external catalog edit is never overwritten.
std::string catalogAuthoredDigest(const Catalog& catalog);
// Replaces ONLY the catalog's thumbnailVariants block through the same staged
// commit, refusing when the authored digest moved and writing nothing when the
// block would be unchanged. The Catalog model — and every revision — is not
// involved at all.
bool mergeCatalogThumbnails(const std::filesystem::path& directory,
                            const CatalogThumbnailCache& variants,
                            std::string_view expectedAuthoredDigest,
                            std::string* error = nullptr);

}  // namespace openshow::catalog
