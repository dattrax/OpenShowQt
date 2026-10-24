/*
 * service.json codec plus package IO — see storage.h for the contract.
 *
 * ServiceCodec is Documents' declared friend: loading is the one place
 * the model is filled wholesale (restoring ids exactly as written),
 * everything else goes through the operations. All reading validates:
 * a file either describes a complete, reference-clean service or the
 * load fails naming why, with the caller's Documents untouched.
 */

#include "openshow/domain/storage.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace openshow::domain {

namespace {

constexpr int kServiceFormat = 1;
constexpr const char* kManifestName = "service.json";
constexpr const char* kAssetsDir = "assets";

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string manifestPath(const std::filesystem::path& directory) {
    return (directory / std::string(kManifestName)).string();
}

// Collision-proof, traversal-proof package filename: the id prefix is
// unique by construction, and the name contributes only safe
// characters.
std::string assetFileName(const Asset& asset) {
    std::string safe;
    safe.reserve(asset.name.size());
    for (const char c : asset.name) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        safe.push_back(keep ? c : '_');
    }
    if (safe.empty() || safe.front() == '.') {
        safe.insert(safe.begin(), 'f');
    }
    return asset.id + "-" + safe;
}

// Trailing digits of a store-issued id ("slide12" -> 12); ids without a
// numeric tail count as 0, which can only lose the ordering check,
// never weaken uniqueness (unique strings are claimed separately).
unsigned long long ordinalOf(const std::string& id) {
    size_t begin = id.size();
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(id[begin - 1])) != 0) {
        --begin;
    }
    if (begin == id.size()) {
        return 0;
    }
    return std::strtoull(id.c_str() + begin, nullptr, 10);
}

// --- read-side field helpers ------------------------------------------------

const boost::json::value* field(const boost::json::object& obj, const char* key) {
    const auto found = obj.find(key);
    return found == obj.end() ? nullptr : &found->value();
}

bool wantString(const boost::json::object& obj, const char* key, std::string* out,
                std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_string()) {
        fail(error, std::string("service.json: '") + key + "' must be a string");
        return false;
    }
    *out = std::string(value->as_string());
    return true;
}

bool wantOptionalString(const boost::json::object& obj, const char* key, std::string* out,
                        std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        out->clear();
        return true;
    }
    if (!value->is_string()) {
        fail(error, std::string("service.json: '") + key + "' must be a string");
        return false;
    }
    *out = std::string(value->as_string());
    return true;
}

bool wantInteger(const boost::json::object& obj, const char* key, unsigned long long* out,
                 std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !(value->is_int64() || value->is_uint64())) {
        fail(error, std::string("service.json: '") + key + "' must be an integer");
        return false;
    }
    const long long signedValue =
        value->is_int64() ? value->as_int64() : static_cast<long long>(value->as_uint64());
    if (signedValue < 0) {
        fail(error, std::string("service.json: '") + key + "' must not be negative");
        return false;
    }
    *out = static_cast<unsigned long long>(signedValue);
    return true;
}

bool wantNumber(const boost::json::object& obj, const char* key, double* out,
                std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_number()) {
        fail(error, std::string("service.json: '") + key + "' must be a number");
        return false;
    }
    *out = value->to_number<double>();
    return true;
}

// An optional number that keeps its caller's current value when absent —
// the slide text box (task 4.3) arrived after the first services, and one
// without it reads as the authored default it was drawn with.
bool wantOptionalNumber(const boost::json::object& obj, const char* key, double* out,
                        std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_number()) {
        fail(error, std::string("service.json: '") + key + "' must be a number");
        return false;
    }
    *out = value->to_number<double>();
    return true;
}

bool wantStringArray(const boost::json::object& obj, const char* key,
                     std::vector<std::string>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_array()) {
        fail(error, std::string("service.json: '") + key + "' must be an array");
        return false;
    }
    out->clear();
    for (const boost::json::value& element : value->as_array()) {
        if (!element.is_string()) {
            fail(error, std::string("service.json: '") + key + "' holds a non-string");
            return false;
        }
        out->push_back(std::string(element.as_string()));
    }
    return true;
}

bool wantArray(const boost::json::object& obj, const char* key,
               const boost::json::array** out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr || !value->is_array()) {
        fail(error, std::string("service.json: '") + key + "' must be an array");
        return false;
    }
    *out = &value->as_array();
    return true;
}

}  // namespace

// Documents' friend: the only code allowed to read the private store
// wholesale (save) or fill it wholesale (load). The public saveService/
// loadService functions are thin delegates, so the fs work below can
// touch fAssets/fShows/... without any of it becoming API.
class ServiceCodec {
public:
    static boost::json::value toJson(const Documents& documents);
    static bool fromJson(const boost::json::value& value, Documents* out, std::string* error);
    static bool save(const Documents& documents, const std::filesystem::path& directory,
                     std::string* error);
    static bool load(Documents* documents, const std::filesystem::path& directory,
                     std::string* error);
};

boost::json::value ServiceCodec::toJson(const Documents& documents) {
    boost::json::object root;
    root.emplace("format", kServiceFormat);
    root.emplace("nextOrdinal", documents.fNextOrdinal);

    boost::json::array shows;
    for (const Show& show : documents.fShows) {
        boost::json::object entry;
        entry.emplace("id", show.id);
        entry.emplace("name", show.name);
        entry.emplace("notes", show.notes);
        entry.emplace("defaultStyleId", show.defaultStyleId);
        boost::json::array slides;
        for (const Slide& slide : show.slides) {
            boost::json::object slideJson;
            slideJson.emplace("id", slide.id);
            slideJson.emplace("group", slide.group);
            boost::json::array lines;
            for (const std::string& line : slide.lines) {
                lines.emplace_back(line);
            }
            slideJson.emplace("lines", std::move(lines));
            slideJson.emplace("notes", slide.notes);
            slideJson.emplace("styleId", slide.styleId);
            slideJson.emplace("backgroundAssetId", slide.backgroundAssetId);
            // The authored text box (task 4.3); the loader defaults these
            // so a service written before geometry existed still reads.
            slideJson.emplace("x", slide.x);
            slideJson.emplace("y", slide.y);
            slideJson.emplace("width", slide.width);
            slideJson.emplace("height", slide.height);
            slides.push_back(std::move(slideJson));
        }
        entry.emplace("slides", std::move(slides));
        shows.push_back(std::move(entry));
    }
    root.emplace("shows", std::move(shows));

    boost::json::array styles;
    for (const Style& style : documents.fStyles) {
        boost::json::object entry;
        entry.emplace("id", style.id);
        entry.emplace("name", style.name);
        entry.emplace("textColor", style.textColor);
        entry.emplace("backgroundColor", style.backgroundColor);
        entry.emplace("fontFamily", style.fontFamily);
        entry.emplace("fontSize", style.fontSize);
        styles.push_back(std::move(entry));
    }
    root.emplace("styles", std::move(styles));

    boost::json::array assets;
    for (const Asset& asset : documents.fAssets) {
        boost::json::object entry;
        entry.emplace("id", asset.id);
        entry.emplace("name", asset.name);
        // Package-relative, always: the loaded sourcePath never leaks
        // into the file, which is what makes re-saves byte-stable.
        entry.emplace("file", std::string(kAssetsDir) + "/" + assetFileName(asset));
        assets.push_back(std::move(entry));
    }
    root.emplace("assets", std::move(assets));

    boost::json::array playlists;
    for (const Playlist& playlist : documents.fPlaylists) {
        boost::json::object entry;
        entry.emplace("id", playlist.id);
        entry.emplace("name", playlist.name);
        boost::json::array jsonEntries;
        for (const PlaylistEntry& row : playlist.entries) {
            boost::json::object rowJson;
            rowJson.emplace("id", row.id);
            rowJson.emplace("showId", row.showId);
            rowJson.emplace("section", row.section);
            jsonEntries.push_back(std::move(rowJson));
        }
        entry.emplace("entries", std::move(jsonEntries));
        playlists.push_back(std::move(entry));
    }
    root.emplace("playlists", std::move(playlists));

    return root;
}

bool ServiceCodec::fromJson(const boost::json::value& value, Documents* out,
                            std::string* error) {
    if (!value.is_object()) {
        fail(error, "service.json: top level must be an object");
        return false;
    }
    const boost::json::object& root = value.as_object();

    unsigned long long format = 0;
    if (!wantInteger(root, "format", &format, error)) {
        return false;
    }
    if (format != static_cast<unsigned long long>(kServiceFormat)) {
        fail(error, "service.json: unsupported format " + std::to_string(format) +
                        " (this build reads format " + std::to_string(kServiceFormat) + ")");
        return false;
    }
    unsigned long long nextOrdinal = 0;
    if (!wantInteger(root, "nextOrdinal", &nextOrdinal, error)) {
        return false;
    }

    // One id space across all kinds, mirroring the store's guarantee.
    std::set<std::string> claimedIds;
    unsigned long long highestOrdinal = 0;
    auto claimId = [&](const Id& id, const char* what) -> bool {
        if (id.empty()) {
            fail(error, std::string("service.json: ") + what + " has an empty id");
            return false;
        }
        if (!claimedIds.insert(id).second) {
            fail(error, "service.json: duplicate id '" + id + "'");
            return false;
        }
        highestOrdinal = std::max(highestOrdinal, ordinalOf(id));
        return true;
    };

    const boost::json::array* showArray = nullptr;
    if (!wantArray(root, "shows", &showArray, error)) {
        return false;
    }
    std::vector<Show> shows;
    for (const boost::json::value& element : *showArray) {
        if (!element.is_object()) {
            fail(error, "service.json: 'shows' holds a non-object");
            return false;
        }
        const boost::json::object& jsonShow = element.as_object();
        Show show;
        if (!wantString(jsonShow, "id", &show.id, error) || !claimId(show.id, "show") ||
            !wantString(jsonShow, "name", &show.name, error) || show.name.empty()) {
            if (error != nullptr && error->empty()) {
                fail(error, "service.json: a show has no name");
            }
            return false;
        }
        if (!wantOptionalString(jsonShow, "notes", &show.notes, error) ||
            !wantOptionalString(jsonShow, "defaultStyleId", &show.defaultStyleId, error)) {
            return false;
        }
        const boost::json::array* slideArray = nullptr;
        if (!wantArray(jsonShow, "slides", &slideArray, error)) {
            return false;
        }
        for (const boost::json::value& slideValue : *slideArray) {
            if (!slideValue.is_object()) {
                fail(error, "service.json: 'slides' holds a non-object");
                return false;
            }
            const boost::json::object& jsonSlide = slideValue.as_object();
            Slide slide;
            if (!wantString(jsonSlide, "id", &slide.id, error) ||
                !claimId(slide.id, "slide") ||
                !wantStringArray(jsonSlide, "lines", &slide.lines, error) ||
                !wantOptionalString(jsonSlide, "group", &slide.group, error) ||
                !wantOptionalString(jsonSlide, "notes", &slide.notes, error) ||
                !wantOptionalString(jsonSlide, "styleId", &slide.styleId, error) ||
                !wantOptionalString(jsonSlide, "backgroundAssetId",
                                    &slide.backgroundAssetId, error) ||
                !wantOptionalNumber(jsonSlide, "x", &slide.x, error) ||
                !wantOptionalNumber(jsonSlide, "y", &slide.y, error) ||
                !wantOptionalNumber(jsonSlide, "width", &slide.width, error) ||
                !wantOptionalNumber(jsonSlide, "height", &slide.height, error)) {
                return false;
            }
            if (!std::isfinite(slide.x) || !std::isfinite(slide.y) ||
                !std::isfinite(slide.width) || !std::isfinite(slide.height) ||
                slide.width <= 0.0 || slide.height <= 0.0) {
                fail(error, "service.json: slide '" + slide.id +
                                "' holds a text box that cannot be placed");
                return false;
            }
            show.slides.push_back(std::move(slide));
        }
        shows.push_back(std::move(show));
    }

    const boost::json::array* styleArray = nullptr;
    if (!wantArray(root, "styles", &styleArray, error)) {
        return false;
    }
    std::vector<Style> styles;
    std::set<Id> styleIds;
    for (const boost::json::value& element : *styleArray) {
        if (!element.is_object()) {
            fail(error, "service.json: 'styles' holds a non-object");
            return false;
        }
        const boost::json::object& jsonStyle = element.as_object();
        Style style;
        if (!wantString(jsonStyle, "id", &style.id, error) || !claimId(style.id, "style") ||
            !wantString(jsonStyle, "name", &style.name, error) ||
            !wantString(jsonStyle, "textColor", &style.textColor, error) ||
            !wantString(jsonStyle, "backgroundColor", &style.backgroundColor, error) ||
            !wantOptionalString(jsonStyle, "fontFamily", &style.fontFamily, error) ||
            !wantNumber(jsonStyle, "fontSize", &style.fontSize, error)) {
            return false;
        }
        styleIds.insert(style.id);
        styles.push_back(std::move(style));
    }

    const boost::json::array* assetArray = nullptr;
    if (!wantArray(root, "assets", &assetArray, error)) {
        return false;
    }
    std::vector<Asset> assets;
    std::set<Id> assetIds;
    for (const boost::json::value& element : *assetArray) {
        if (!element.is_object()) {
            fail(error, "service.json: 'assets' holds a non-object");
            return false;
        }
        const boost::json::object& jsonAsset = element.as_object();
        Asset asset;
        std::string file;
        if (!wantString(jsonAsset, "id", &asset.id, error) || !claimId(asset.id, "asset") ||
            !wantString(jsonAsset, "name", &asset.name, error) ||
            !wantString(jsonAsset, "file", &file, error) || file.empty()) {
            if (error != nullptr && error->empty()) {
                fail(error, "service.json: an asset has no file");
            }
            return false;
        }
        // During validation sourcePath carries the package-relative
        // path; loadService() rewrites it to a usable location last.
        asset.sourcePath = file;
        assetIds.insert(asset.id);
        assets.push_back(std::move(asset));
    }

    const boost::json::array* playlistArray = nullptr;
    if (!wantArray(root, "playlists", &playlistArray, error)) {
        return false;
    }
    std::vector<Playlist> playlists;
    std::set<Id> showIds;
    for (const Show& show : shows) {
        showIds.insert(show.id);
    }
    for (const boost::json::value& element : *playlistArray) {
        if (!element.is_object()) {
            fail(error, "service.json: 'playlists' holds a non-object");
            return false;
        }
        const boost::json::object& jsonPlaylist = element.as_object();
        Playlist playlist;
        if (!wantString(jsonPlaylist, "id", &playlist.id, error) ||
            !claimId(playlist.id, "playlist") ||
            !wantString(jsonPlaylist, "name", &playlist.name, error)) {
            return false;
        }
        const boost::json::array* entryArray = nullptr;
        if (!wantArray(jsonPlaylist, "entries", &entryArray, error)) {
            return false;
        }
        for (const boost::json::value& entryValue : *entryArray) {
            if (!entryValue.is_object()) {
                fail(error, "service.json: 'entries' holds a non-object");
                return false;
            }
            const boost::json::object& jsonEntry = entryValue.as_object();
            PlaylistEntry entry;
            if (!wantString(jsonEntry, "id", &entry.id, error) ||
                !claimId(entry.id, "entry") ||
                !wantString(jsonEntry, "showId", &entry.showId, error) ||
                !wantOptionalString(jsonEntry, "section", &entry.section, error)) {
                return false;
            }
            playlist.entries.push_back(std::move(entry));
        }
        playlists.push_back(std::move(playlist));
    }

    // Reference pass: a file we wrote always satisfies this; anything
    // else is corruption and the load says so.
    for (const Show& show : shows) {
        if (!show.defaultStyleId.empty() && styleIds.count(show.defaultStyleId) == 0) {
            fail(error, "service.json: show '" + show.id + "' defaults to unknown style '" +
                            show.defaultStyleId + "'");
            return false;
        }
        for (const Slide& slide : show.slides) {
            if (!slide.styleId.empty() && styleIds.count(slide.styleId) == 0) {
                fail(error, "service.json: slide '" + slide.id + "' names unknown style '" +
                                slide.styleId + "'");
                return false;
            }
            if (!slide.backgroundAssetId.empty() &&
                assetIds.count(slide.backgroundAssetId) == 0) {
                fail(error, "service.json: slide '" + slide.id +
                                "' names unknown asset '" + slide.backgroundAssetId + "'");
                return false;
            }
        }
    }
    for (const Playlist& playlist : playlists) {
        for (const PlaylistEntry& entry : playlist.entries) {
            if (showIds.count(entry.showId) == 0) {
                fail(error, "service.json: entry '" + entry.id +
                                "' names unknown show '" + entry.showId + "'");
                return false;
            }
        }
    }

    if (nextOrdinal <= highestOrdinal) {
        fail(error, "service.json: nextOrdinal " + std::to_string(nextOrdinal) +
                        " does not follow the highest id in use (" +
                        std::to_string(highestOrdinal) + ")");
        return false;
    }

    out->fShows = std::move(shows);
    out->fStyles = std::move(styles);
    out->fAssets = std::move(assets);
    out->fPlaylists = std::move(playlists);
    out->fNextOrdinal = nextOrdinal;
    return true;
}

// --- package IO ---------------------------------------------------------------

bool ServiceCodec::save(const Documents& documents, const std::filesystem::path& directory,
                        std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        fail(error, "cannot create '" + directory.string() + "': " + ec.message());
        return false;
    }

    // Assets first: a service.json that exists must name only files
    // that exist.
    if (!documents.fAssets.empty()) {
        std::filesystem::create_directories(directory / kAssetsDir, ec);
        if (ec && !std::filesystem::is_directory(directory / kAssetsDir)) {
            fail(error, "cannot create '" + (directory / kAssetsDir).string() +
                            "': " + ec.message());
            return false;
        }
    }
    for (const Asset& asset : documents.fAssets) {
        const std::filesystem::path source =
            std::filesystem::path(asset.sourcePath).lexically_normal();
        const std::filesystem::path destination =
            directory / kAssetsDir / assetFileName(asset);
        if (!std::filesystem::is_regular_file(source, ec) || ec) {
            fail(error, "asset '" + asset.name + "' has no readable file at '" +
                            asset.sourcePath + "'");
            return false;
        }
        if (std::filesystem::equivalent(source, destination, ec) && !ec) {
            continue;  // already inside the package
        }
        std::filesystem::copy_file(source, destination,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            fail(error, "copying asset '" + asset.name + "' into the package failed: " +
                            ec.message());
            return false;
        }
    }

    // The manifest through a temporary in the same directory, so the
    // rename is atomic against readers of service.json.
    const std::filesystem::path temporary =
        directory / (std::string(kManifestName) + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail(error, "cannot write '" + temporary.string() + "'");
            return false;
        }
        out << boost::json::serialize(toJson(documents));
        if (!out.good()) {
            fail(error, "writing '" + temporary.string() + "' failed");
            std::filesystem::remove(temporary);
            return false;
        }
    }
    std::filesystem::rename(temporary, directory / kManifestName, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        fail(error, "installing '" + manifestPath(directory) + "' failed: " + ec.message());
        return false;
    }
    return true;
}

bool ServiceCodec::load(Documents* documents, const std::filesystem::path& directory,
                        std::string* error) {
    std::ifstream in(directory / kManifestName, std::ios::binary);
    if (!in) {
        fail(error, "cannot open '" + manifestPath(directory) + "'");
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        fail(error, "reading '" + manifestPath(directory) + "' failed");
        return false;
    }

    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(buffer.str(), parseError);
    if (parseError) {
        fail(error, std::string("service.json: malformed JSON: ") + parseError.message());
        return false;
    }

    Documents loaded;  // the target keeps its old contents on failure
    if (!fromJson(parsed, &loaded, error)) {
        return false;
    }

    for (Asset& asset : loaded.fAssets) {
        const std::filesystem::path file =
            (directory / asset.sourcePath).lexically_normal();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec) || ec) {
            fail(error, "service.json: asset '" + asset.name + "' file '" + file.string() +
                            "' is missing from the package");
            return false;
        }
        asset.sourcePath = file.string();
    }

    *documents = std::move(loaded);
    return true;
}

// --- public entry points --------------------------------------------------------

bool saveService(const Documents& documents, const std::filesystem::path& directory,
                 std::string* error) {
    return ServiceCodec::save(documents, directory, error);
}

bool loadService(Documents* documents, const std::filesystem::path& directory,
                 std::string* error) {
    if (documents == nullptr) {
        fail(error, "loadService needs a target");
        return false;
    }
    return ServiceCodec::load(documents, directory, error);
}

// --- autosave -----------------------------------------------------------------

Autosave::Autosave(std::filesystem::path directory) : fDirectory(std::move(directory)) {}

bool Autosave::tick(const Documents& documents, std::string* error) {
    // The codec is deterministic, so manifest bytes are a fingerprint:
    // equal means the service is unchanged and no disk access follows.
    const std::string manifest = boost::json::serialize(ServiceCodec::toJson(documents));
    if (manifest == fLastManifest) {
        return true;
    }
    if (!ServiceCodec::save(documents, fDirectory, error)) {
        return false;
    }
    fLastManifest = std::move(manifest);
    return true;
}

bool Autosave::recover(Documents* documents, std::string* error) {
    if (documents == nullptr) {
        fail(error, "recover needs a target");
        return false;
    }
    if (!ServiceCodec::load(documents, fDirectory, error)) {
        return false;
    }
    fLastManifest = boost::json::serialize(ServiceCodec::toJson(*documents));
    return true;
}

bool Autosave::clear(std::string* error) {
    std::error_code ec;
    std::filesystem::remove_all(fDirectory, ec);
    if (ec) {
        fail(error, "removing '" + fDirectory.string() + "' failed: " + ec.message());
        return false;
    }
    fLastManifest.clear();
    return true;
}

bool Autosave::exists(const std::filesystem::path& directory) {
    std::error_code ec;
    return std::filesystem::is_regular_file(directory / kManifestName, ec);
}

}  // namespace openshow::domain
