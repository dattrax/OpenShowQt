#include "openshow/catalog/catalogstorage.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "openshow/catalog/history.h"

namespace openshow::catalog {

namespace {

constexpr const char* kManifestName = "catalog.json";
constexpr unsigned long long kCatalogFormat = 1;

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
class CatalogCodec {
public:
    static boost::json::value toJson(const Catalog& catalog) {
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
        items.push_back(std::move(entry));
    }
    root.emplace("items", std::move(items));
    return root;
}

static bool fromJson(const boost::json::value& value, Catalog* out, std::string* error) {
    if (!value.is_object()) {
        fail(error, "catalog.json: the root is not an object");
        return false;
    }
    const boost::json::object& root = value.as_object();
    if (!onlyKeys(root, {"format", "nextOrdinal", "items"}, "catalog.json", error)) {
        return false;
    }
    unsigned long long format = 0;
    if (!wantInteger(root, "format", &format, "catalog.json", error)) {
        return false;
    }
    if (format != kCatalogFormat) {
        fail(error, "catalog.json: unsupported format " + std::to_string(format) +
                        " (this build reads format " +
                        std::to_string(kCatalogFormat) + ")");
        return false;
    }
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
        if (!onlyKeys(
                entry,
                {"id", "category", "title", "recordRevision", "contentRevision",
                 "kind", "metadata", "stanzas"},
                owner, error)) {
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
    *out = std::move(loaded);
    return true;
    }
};  // class CatalogCodec

bool saveCatalog(const Catalog& catalog, const std::filesystem::path& directory,
                 std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        fail(error, "cannot create '" + directory.string() + "': " + ec.message());
        return false;
    }
    const std::filesystem::path temporary =
        directory / (std::string(kManifestName) + ".tmp");
    const std::string manifest = boost::json::serialize(CatalogCodec::toJson(catalog));
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

bool loadCatalog(Catalog* catalog, const std::filesystem::path& directory,
                 std::string* error) {
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
    return CatalogCodec::fromJson(value, catalog, error);
}

}  // namespace openshow::catalog
