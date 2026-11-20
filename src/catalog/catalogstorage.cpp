#include "openshow/catalog/catalogstorage.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "openshow/catalog/history.h"
#include "openshow/domain/jsoncanonical.h"

namespace openshow::catalog {

namespace {

constexpr const char* kManifestName = "catalog.json";
// Format 2 (task PT.2) adds the optional remembered look on a song. A
// format-1 file loads with no preference; a format-2 item omits the key when
// it remembers nothing, so a catalog that never uses the feature keeps the
// payload it always had apart from the format number.
// Format 3 (TC.9) adds the optional root "thumbnailVariants" block: bounded,
// independently versioned pictures of prepared looks. It changes no ITEM key,
// so a format-2 reader's strict item schema is untouched; an older build
// refuses the file by format, as it always has.
constexpr unsigned long long kCatalogFormat = 3;
constexpr unsigned long long kCatalogFormatLegacy = 1;
constexpr unsigned long long kCatalogFormatRememberedLook = 2;
constexpr unsigned long long kCatalogFormatRenderVariants = 3;
constexpr unsigned long long kThumbnailVariantsFormat = 1;

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string osError(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

bool catalogFault(const char* step) {
    const char* fault = std::getenv("OPENSHOW_CATALOG_FAULT");
    return fault != nullptr && std::string_view(step) == fault;
}

bool syncDirectory(const std::filesystem::path& directory, std::string* error) {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fail(error, osError("cannot open '" + directory.string() + "' for syncing"));
        return false;
    }
    const int status = ::fsync(fd);
    const int syncErrno = errno;
    const int closeStatus = ::close(fd);
    if (status != 0) {
        errno = syncErrno;
        fail(error, osError("syncing directory '" + directory.string() + "'"));
        return false;
    }
    if (closeStatus != 0) {
        fail(error, osError("closing directory '" + directory.string() + "' after syncing"));
        return false;
    }
    return true;
}

// An id is a NAME, never a path: letters, digits, '-' and '_'. The catalog
// codec issues "itemN"; a hand-edited file may not smuggle anything else in.
bool safeOpaqueId(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool wantString(const boost::json::object& obj, const char* key, std::string* out,
                const std::string& owner, std::string* error) {
    const boost::json::value* value = obj.if_contains(key);
    if (value == nullptr || !value->is_string()) {
        fail(error, owner + ": missing or non-string '" + key + "'");
        return false;
    }
    *out = value->as_string().c_str();
    return true;
}

bool wantOptionalString(const boost::json::object& obj, const char* key, std::string* out,
                        const std::string& owner, std::string* error) {
    const boost::json::value* value = obj.if_contains(key);
    if (value == nullptr) {
        out->clear();
        return true;
    }
    if (!value->is_string()) {
        fail(error, owner + ": '" + key + "' is not a string");
        return false;
    }
    *out = value->as_string().c_str();
    return true;
}

bool wantInteger(const boost::json::object& obj, const char* key,
                 unsigned long long* out, const std::string& owner,
                 std::string* error) {
    const boost::json::value* value = obj.if_contains(key);
    if (value == nullptr || !value->is_int64()) {
        fail(error, owner + ": missing or non-integer '" + key + "'");
        return false;
    }
    const long long number = value->as_int64();
    if (number < 0) {
        fail(error, owner + ": '" + key + "' may not be negative");
        return false;
    }
    *out = static_cast<unsigned long long>(number);
    return true;
}

bool wantArray(const boost::json::object& obj, const char* key,
               const boost::json::array** out, const std::string& owner,
               std::string* error) {
    const boost::json::value* value = obj.if_contains(key);
    if (value == nullptr || !value->is_array()) {
        fail(error, owner + ": missing or non-array '" + key + "'");
        return false;
    }
    *out = &value->as_array();
    return true;
}

// Reject keys this build does not know (a newer catalog's meaning is not
// something an older reader may silently drop). Keys the reader ignores are
// therefore impossible; every known key is named in one list per object.
bool onlyKeys(const boost::json::object& obj, std::initializer_list<const char*> allowed,
              const std::string& owner, std::string* error) {
    for (const boost::json::key_value_pair& pair : obj) {
        const std::string_view key = pair.key();
        bool known = false;
        for (const char* name : allowed) {
            if (key == name) {
                known = true;
                break;
            }
        }
        if (!known) {
            fail(error, owner + ": unknown key '" + std::string(key) + "'");
            return false;
        }
    }
    return true;
}

}  // namespace

// The codec is the Catalog's friend (catalog.h), so it can reconstruct a
// loaded model wholesale while operations stay the only mutation path for
// everything else.
boost::json::object variantJson(const openshow::domain::ThumbnailCacheEntry& entry) {
    boost::json::object record;
    record.emplace("fingerprint", entry.fingerprint);
    record.emplace("renderer", entry.renderer);
    record.emplace("width", entry.width);
    record.emplace("height", entry.height);
    record.emplace("pngBytes", entry.pngBytes);
    record.emplace("checksum", entry.checksum);
    record.emplace("pngBase64", entry.pngBase64);
    return record;
}

// The Library block, written last so every authored member keeps its order.
boost::json::object variantsBlock(const CatalogThumbnailCache& variants) {
    boost::json::object block;
    block.emplace("format", variants.format);
    boost::json::array items;
    for (const auto& [id, list] : variants.variants) {
        if (list.empty()) {
            continue;
        }
        boost::json::object item;
        item.emplace("id", id);
        boost::json::array records;
        for (const openshow::domain::ThumbnailCacheEntry& entry : list) {
            records.push_back(variantJson(entry));
        }
        item.emplace("variants", std::move(records));
        items.push_back(std::move(item));
    }
    block.emplace("items", std::move(items));
    return block;
}

class CatalogCodec {
public:
    static boost::json::value toJson(const Catalog& catalog,
                                     const CatalogThumbnailCache* variants = nullptr) {
    boost::json::object root;
    root.emplace("format", kCatalogFormat);
    root.emplace("nextOrdinal", catalog.fNextOrdinal);
    boost::json::array items;
    for (const CatalogItem& item : catalog.fItems) {
        boost::json::object entry;
        entry.emplace("id", item.id);
        entry.emplace("category", categoryName(item.category));
        entry.emplace("title", item.title);
        entry.emplace("recordRevision", item.recordRevision);
        entry.emplace("contentRevision", item.contentRevision);
        entry.emplace("kind", contentKindName(item.kind));
        boost::json::object metadata;
        metadata.emplace("ccli", item.metadata.ccli);
        metadata.emplace("authors", item.metadata.authors);
        metadata.emplace("copyright", item.metadata.copyright);
        entry.emplace("metadata", std::move(metadata));
        boost::json::array stanzas;
        for (const Stanza& stanza : item.stanzas) {
            boost::json::object stanzaJson;
            stanzaJson.emplace("label", stanza.label);
            boost::json::array lines;
            for (const std::string& line : stanza.lines) {
                lines.emplace_back(line);
            }
            stanzaJson.emplace("lines", std::move(lines));
            stanzas.push_back(std::move(stanzaJson));
        }
        entry.emplace("stanzas", std::move(stanzas));
        // Written only when it exists: absence is the pre-PT history of this
        // key, and an item that remembers nothing keeps its byte shape.
        if (item.themeSnapshot.has_value()) {
            entry.emplace("themeSnapshot",
                          themeSnapshotToJson(*item.themeSnapshot));
        }
        items.push_back(std::move(entry));
    }
    root.emplace("items", std::move(items));
    if (variants != nullptr) {
        CatalogThumbnailCache clean;
        clean.format = kThumbnailVariantsFormat;
        for (const auto& [id, list] : variants->variants) {
            std::vector<openshow::domain::ThumbnailCacheEntry> kept = list;
            openshow::domain::sanitizeThumbnailVariants(&kept);
            if (!kept.empty()) {
                clean.variants.emplace(id, std::move(kept));
            }
        }
        if (!clean.variants.empty()) {
            root.emplace("thumbnailVariants", variantsBlock(clean));
        }
    }
    return root;
}

static bool fromJson(const boost::json::value& value, Catalog* out, std::string* error,
                     CatalogThumbnailCache* variants) {
    if (!value.is_object()) {
        fail(error, "catalog.json: the root is not an object");
        return false;
    }
    const boost::json::object& root = value.as_object();
    std::optional<unsigned long long> declaredFormat;
    if (const boost::json::value* formatValue = root.if_contains("format");
        formatValue != nullptr && formatValue->is_int64()) {
        declaredFormat = static_cast<unsigned long long>(formatValue->as_int64());
    }
    const bool rootCarriesVariants =
        declaredFormat.has_value() && *declaredFormat >= kCatalogFormatRenderVariants;
    if (rootCarriesVariants
            ? !onlyKeys(root,
                        {"format", "nextOrdinal", "items", "thumbnailVariants"},
                        "catalog.json", error)
            : !onlyKeys(root, {"format", "nextOrdinal", "items"}, "catalog.json", error)) {
        return false;
    }
    unsigned long long format = 0;
    if (!wantInteger(root, "format", &format, "catalog.json", error)) {
        return false;
    }
    if (format != kCatalogFormat && format != kCatalogFormatRememberedLook &&
        format != kCatalogFormatLegacy) {
        fail(error, "catalog.json: unsupported format " + std::to_string(format) +
                        " (this build reads formats " +
                        std::to_string(kCatalogFormatLegacy) + " through " +
                        std::to_string(kCatalogFormat) + ")");
        return false;
    }
    const bool hasRememberedLook = format >= kCatalogFormatRememberedLook;
    const bool hasRenderVariants = format >= kCatalogFormatRenderVariants;
    unsigned long long nextOrdinal = 1;
    if (!wantInteger(root, "nextOrdinal", &nextOrdinal, "catalog.json", error)) {
        return false;
    }
    if (nextOrdinal == 0) {
        fail(error, "catalog.json: nextOrdinal must be at least 1");
        return false;
    }
    const boost::json::array* items = nullptr;
    if (!wantArray(root, "items", &items, "catalog.json", error)) {
        return false;
    }

    Catalog loaded;
    loaded.fItems.clear();
    std::vector<std::string> seen;
    for (std::size_t i = 0; i < items->size(); ++i) {
        const std::string owner = "catalog.json item " + std::to_string(i + 1);
        if (!(*items)[i].is_object()) {
            fail(error, owner + " is not an object");
            return false;
        }
        const boost::json::object& entry = (*items)[i].as_object();
        if (hasRememberedLook) {
            if (!onlyKeys(
                    entry,
                    {"id", "category", "title", "recordRevision", "contentRevision",
                     "kind", "metadata", "stanzas", "themeSnapshot"},
                    owner, error)) {
                return false;
            }
        } else if (!onlyKeys(
                       entry,
                       {"id", "category", "title", "recordRevision", "contentRevision",
                        "kind", "metadata", "stanzas"},
                       owner, error)) {
            // A format-1 file that names the newer key is refused by name:
            // it carries a meaning that format did not have.
            return false;
        }
        CatalogItem item;
        if (!wantString(entry, "id", &item.id, owner, error)) {
            return false;
        }
        if (!safeOpaqueId(item.id)) {
            fail(error, owner + ": unsafe id '" + item.id + "'");
            return false;
        }
        for (const std::string& previous : seen) {
            if (previous == item.id) {
                fail(error, owner + ": duplicate id '" + item.id + "'");
                return false;
            }
        }
        seen.push_back(item.id);
        std::string category;
        if (!wantString(entry, "category", &category, owner, error)) {
            return false;
        }
        if (!categoryFromName(category, &item.category)) {
            fail(error, owner + ": unknown category '" + category + "'");
            return false;
        }
        std::string kind;
        if (!wantString(entry, "kind", &kind, owner, error)) {
            return false;
        }
        if (!contentKindFromName(kind, &item.kind)) {
            fail(error, owner + ": unknown content kind '" + kind + "'");
            return false;
        }
        if (!wantString(entry, "title", &item.title, owner, error) ||
            !wantInteger(entry, "recordRevision", &item.recordRevision, owner, error) ||
            !wantInteger(entry, "contentRevision", &item.contentRevision, owner, error)) {
            return false;
        }
        if (item.recordRevision == 0 || item.contentRevision == 0) {
            fail(error, owner + ": revisions must be at least 1");
            return false;
        }
        const boost::json::value* metadataValue = entry.if_contains("metadata");
        if (metadataValue == nullptr || !metadataValue->is_object()) {
            fail(error, owner + ": missing or non-object 'metadata'");
            return false;
        }
        const boost::json::object& metadata = metadataValue->as_object();
        if (!onlyKeys(metadata, {"ccli", "authors", "copyright"},
                      owner + " metadata", error)) {
            return false;
        }
        if (!wantOptionalString(metadata, "ccli", &item.metadata.ccli,
                                owner + " metadata", error) ||
            !wantOptionalString(metadata, "authors", &item.metadata.authors,
                                owner + " metadata", error) ||
            !wantOptionalString(metadata, "copyright", &item.metadata.copyright,
                                owner + " metadata", error)) {
            return false;
        }
        const boost::json::array* stanzas = nullptr;
        if (!wantArray(entry, "stanzas", &stanzas, owner, error)) {
            return false;
        }
        for (std::size_t s = 0; s < stanzas->size(); ++s) {
            const std::string stanzaOwner =
                owner + " stanza " + std::to_string(s + 1);
            if (!(*stanzas)[s].is_object()) {
                fail(error, stanzaOwner + " is not an object");
                return false;
            }
            const boost::json::object& stanzaJson = (*stanzas)[s].as_object();
            if (!onlyKeys(stanzaJson, {"label", "lines"}, stanzaOwner, error)) {
                return false;
            }
            Stanza stanza;
            if (!wantOptionalString(stanzaJson, "label", &stanza.label,
                                    stanzaOwner, error)) {
                return false;
            }
            const boost::json::array* lines = nullptr;
            if (!wantArray(stanzaJson, "lines", &lines, stanzaOwner, error)) {
                return false;
            }
            for (const boost::json::value& line : *lines) {
                if (!line.is_string()) {
                    fail(error, stanzaOwner + ": a line is not a string");
                    return false;
                }
                stanza.lines.emplace_back(line.as_string().c_str());
            }
            item.stanzas.push_back(std::move(stanza));
        }
        if (!validSong(item.title, item.stanzas, item.metadata, error)) {
            std::string reason = error != nullptr ? *error : "invalid song";
            fail(error, owner + ": " + reason);
            return false;
        }
        if (const boost::json::value* remembered = entry.if_contains("themeSnapshot");
            remembered != nullptr) {
            if (!hasRememberedLook) {
                fail(error, owner + ": 'themeSnapshot' needs catalog format 2");
                return false;
            }
            openshow::domain::ThemeSnapshot snapshot;
            std::string why;
            if (!openshow::domain::themeSnapshotFromJson(*remembered, &snapshot,
                                                         &why)) {
                fail(error, owner + " themeSnapshot: " + why);
                return false;
            }
            item.themeSnapshot = std::move(snapshot);
        }
        loaded.fItems.push_back(std::move(item));
    }

    // The counter must never reissue an id the file already used.
    unsigned long long highest = 0;
    for (const CatalogItem& item : loaded.fItems) {
        unsigned long long ordinal = 0;
        const std::string prefix = "item";
        if (item.id.rfind(prefix, 0) == 0) {
            for (std::size_t i = prefix.size(); i < item.id.size(); ++i) {
                const char c = item.id[i];
                if (c < '0' || c > '9') {
                    ordinal = 0;
                    break;
                }
                ordinal = ordinal * 10 + static_cast<unsigned long long>(c - '0');
            }
        }
        highest = std::max(highest, ordinal);
    }
    loaded.fNextOrdinal = std::max(nextOrdinal, highest + 1);

    // TC.9: the derived Library block. Unusable variants are a miss, never a
    // refused catalog: the authored songs load whatever this says. Ids that no
    // longer exist are dropped with the item they described.
    if (variants != nullptr) {
        variants->format = kThumbnailVariantsFormat;
        variants->variants.clear();
        if (hasRenderVariants) {
            const boost::json::value* block = root.if_contains("thumbnailVariants");
            if (block != nullptr && block->is_object()) {
                const boost::json::object& cache = block->as_object();
                const boost::json::value* blockFormat = cache.if_contains("format");
                if (blockFormat != nullptr && blockFormat->is_int64() &&
                    blockFormat->as_int64() ==
                        static_cast<std::int64_t>(kThumbnailVariantsFormat)) {
                    const boost::json::value* items = cache.if_contains("items");
                    if (items != nullptr && items->is_array()) {
                        std::set<std::string> known;
                        for (const CatalogItem& item : loaded.fItems) {
                            known.insert(item.id);
                        }
                        for (const boost::json::value& value : items->as_array()) {
                            if (!value.is_object()) {
                                continue;
                            }
                            const boost::json::object& item = value.as_object();
                            std::string id;
                            if (!wantOptionalString(item, "id", &id, "thumbnail block", error)) {
                                continue;
                            }
                            if (id.empty() || known.count(id) == 0) {
                                continue;
                            }
                            const boost::json::value* list = item.if_contains("variants");
                            if (list == nullptr || !list->is_array()) {
                                continue;
                            }
                            std::vector<openshow::domain::ThumbnailCacheEntry> records;
                            for (const boost::json::value& record : list->as_array()) {
                                if (!record.is_object()) {
                                    continue;
                                }
                                const boost::json::object& entry = record.as_object();
                                openshow::domain::ThumbnailCacheEntry variant;
                                std::uint64_t width = 0;
                                std::uint64_t height = 0;
                                std::uint64_t pngBytes = 0;
                                auto string = [&entry](const char* key, std::string* out) {
                                    const boost::json::value* found = entry.if_contains(key);
                                    if (found == nullptr || !found->is_string()) {
                                        return false;
                                    }
                                    *out = found->as_string().c_str();
                                    return true;
                                };
                                auto number = [&entry](const char* key, std::uint64_t* out) {
                                    const boost::json::value* found = entry.if_contains(key);
                                    if (found == nullptr || !found->is_int64() ||
                                        found->as_int64() < 0) {
                                        return false;
                                    }
                                    *out = static_cast<std::uint64_t>(found->as_int64());
                                    return true;
                                };
                                if (!string("fingerprint", &variant.fingerprint) ||
                                    !string("renderer", &variant.renderer) ||
                                    !number("width", &width) || !number("height", &height) ||
                                    !number("pngBytes", &pngBytes) ||
                                    !string("checksum", &variant.checksum) ||
                                    !string("pngBase64", &variant.pngBase64)) {
                                    continue;
                                }
                                if (width > 0xffffffffULL || height > 0xffffffffULL ||
                                    pngBytes > 0xffffffffULL) {
                                    continue;
                                }
                                variant.width = static_cast<std::uint32_t>(width);
                                variant.height = static_cast<std::uint32_t>(height);
                                variant.pngBytes = pngBytes;
                                records.push_back(std::move(variant));
                            }
                            openshow::domain::sanitizeThumbnailVariants(&records);
                            if (!records.empty()) {
                                variants->variants.emplace(id, std::move(records));
                            }
                        }
                    }
                }
            }
        }
    }
    *out = std::move(loaded);
    return true;
    }
};  // class CatalogCodec

// TC.9: the catalog commit, split out so a cache-only block replacement uses
// the same staged temp+fsync+rename and the same fault seams.
bool writeCatalogFile(const std::filesystem::path& directory, const std::string& manifest,
                      std::string* error) {
    const std::filesystem::path temporary =
        directory / (std::string(kManifestName) + ".tmp");
    std::error_code ec;
    if (catalogFault("write")) {
        fail(error, "writing '" + temporary.string() +
                        "' failed: simulated fault at 'write'");
        return false;
    }
    {
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fail(error, osError("cannot write '" + temporary.string() + "'"));
            return false;
        }
        const auto abandon = [&temporary, fd] {
            ::close(fd);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        };
        std::size_t offset = 0;
        while (offset < manifest.size()) {
            const ssize_t written = ::write(fd, manifest.data() + offset,
                                            manifest.size() - offset);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                abandon();
                fail(error, osError("writing '" + temporary.string() + "'"));
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }
        const bool syncFault = catalogFault("fsync");
        const int syncStatus = ::fsync(fd);
        const int syncErrno = errno;
        if (syncFault) {
            abandon();
            fail(error, "syncing the new catalog failed: simulated fault at 'fsync'");
            return false;
        }
        if (syncStatus != 0) {
            abandon();
            errno = syncErrno;
            fail(error, osError("syncing the new catalog"));
            return false;
        }
        const bool closeFault = catalogFault("close");
        const int closeStatus = ::close(fd);
        if (closeFault) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            fail(error, "closing '" + temporary.string() +
                            "' after syncing failed: simulated fault at 'close'");
            return false;
        }
        if (closeStatus != 0) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            fail(error, "closing '" + temporary.string() + "' after syncing failed: " +
                            osError("close"));
            return false;
        }
    }
    if (catalogFault("rename")) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        fail(error, "installing '" + (directory / kManifestName).string() +
                        "' failed: simulated fault at 'rename'");
        return false;
    }
    std::filesystem::rename(temporary, directory / kManifestName, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        fail(error, "installing '" + (directory / kManifestName).string() +
                        "' failed: " + ec.message());
        return false;
    }
    return syncDirectory(directory, error);
}



bool saveCatalog(const Catalog& catalog, const std::filesystem::path& directory,
                 std::string* error, const CatalogThumbnailCache* variants) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        fail(error, "cannot create '" + directory.string() + "': " + ec.message());
        return false;
    }
    return writeCatalogFile(directory,
                            boost::json::serialize(CatalogCodec::toJson(catalog, variants)),
                            error);
}

bool loadCatalog(Catalog* catalog, const std::filesystem::path& directory,
                 std::string* error, CatalogThumbnailCache* variants) {
    std::ifstream in(directory / kManifestName, std::ios::binary);
    if (!in) {
        fail(error, "cannot open '" + (directory / kManifestName).string() + "'");
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        fail(error, "reading '" + (directory / kManifestName).string() + "' failed");
        return false;
    }
    boost::system::error_code parseError;
    const boost::json::value value =
        boost::json::parse(buffer.str(), parseError);
    if (parseError) {
        fail(error, "catalog.json: " + parseError.message());
        return false;
    }
    return CatalogCodec::fromJson(value, catalog, error, variants);
}

std::string catalogAuthoredDigest(const Catalog& catalog) {
    return openshow::domain::canonicalJsonDigest(CatalogCodec::toJson(catalog));
}

bool mergeCatalogThumbnails(const std::filesystem::path& directory,
                            const CatalogThumbnailCache& variants,
                            std::string_view expectedAuthoredDigest, std::string* error) {
    const std::filesystem::path manifest = directory / kManifestName;
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        fail(error, "cannot open '" + manifest.string() + "'");
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        fail(error, "reading '" + manifest.string() + "' failed");
        return false;
    }
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(buffer.str(), parseError);
    if (parseError || !parsed.is_object()) {
        fail(error, "catalog.json: " +
                        std::string(parseError ? parseError.message() : "not an object"));
        return false;
    }
    boost::json::object root = parsed.as_object();
    root.erase("thumbnailVariants");
    if (openshow::domain::canonicalJsonDigest(boost::json::value(root)) !=
        expectedAuthoredDigest) {
        fail(error, "catalog.json changed after it was loaded; the thumbnail variants"
                    " were not written");
        return false;
    }
    CatalogThumbnailCache clean;
    clean.format = kThumbnailVariantsFormat;
    for (const auto& [id, list] : variants.variants) {
        std::vector<openshow::domain::ThumbnailCacheEntry> kept = list;
        openshow::domain::sanitizeThumbnailVariants(&kept);
        if (!kept.empty()) {
            clean.variants.emplace(id, std::move(kept));
        }
    }
    const bool hadBlock = root.contains("thumbnailVariants");
    if (clean.variants.empty()) {
        if (!hadBlock) {
            return true;
        }
        root.erase("thumbnailVariants");
    } else {
        const boost::json::object block = variantsBlock(clean);
        const boost::json::value* existing = root.if_contains("thumbnailVariants");
        if (existing != nullptr &&
            boost::json::serialize(*existing) == boost::json::serialize(block)) {
            return true;
        }
        root["thumbnailVariants"] = block;
    }
    return writeCatalogFile(directory, boost::json::serialize(root), error);
}

}  // namespace openshow::catalog
