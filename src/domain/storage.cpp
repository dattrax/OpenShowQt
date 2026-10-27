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

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace openshow::domain {

namespace {

// service.json's version. Format 1 held one text box per slide, written as
// the slide's own "lines" plus x/y/width/height; format 2 (task 6.17a, review
// R17) holds an ORDERED LIST of boxes, each with its own id, and a slide can
// no longer say what it means in the format-1 shape. The reader still loads 1
// and migrates it; the writer only ever emits kServiceFormat, and a 2-file
// read by an older build refuses the whole package rather than quietly
// showing the first box of a slide whose author had composed four.
constexpr unsigned long long kServiceFormat = 2;
// The one older version this build reads: single-box slides.
constexpr unsigned long long kServiceFormatLegacyBoxes = 1;
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

/*
 * Test seam (task 6.13a, review R8): with OPENSHOW_STORAGE_FAULT set to
 * one of the commit step names below, the next commit attempt fails at
 * exactly that step — before the step's effect happens, the way a device
 * refuses rather than half-does and denies. The variable is read at each
 * step, not cached, so a fixture can set it, watch one save fail, clear
 * it, and save for real, all in one process. The steps: asset-copy,
 * asset-fsync, asset-rename, assets-fsync, manifest-write,
 * manifest-fsync, manifest-close, manifest-rename, dir-fsync. Every
 * failure message names its step, which is how the fixtures tell an
 * injected refusal from real disk trouble. This is the only purpose of
 * the variable; an operator who sets it makes their own saves fail.
 */
bool storageFault(const char* step) {
    const char* fault = std::getenv("OPENSHOW_STORAGE_FAULT");
    return fault != nullptr && std::string_view(step) == fault;
}

/*
 * Test seam (task 6.14b, review R14): OPENSHOW_STORAGE_DELAY_MS holds
 * every save at its start for the given milliseconds (capped at 3000,
 * garbage means no delay). Slow storage is not an error to inject but a
 * WINDOW to create: a fixture can hold one autosave write in flight
 * while the operator saves or discards — the exact race between an old
 * in-flight write and a newer decision that R14 says must never resurrect
 * or overwrite. Production runs with the variable unset: no sleep.
 */
int storageDelayMs() {
    const char* value = std::getenv("OPENSHOW_STORAGE_DELAY_MS");
    if (value == nullptr) {
        return 0;
    }
    const long ms = std::strtol(value, nullptr, 10);
    if (ms <= 0) {
        return 0;
    }
    return ms > 3000 ? 3000 : static_cast<int>(ms);
}

// Stat identity of an asset source for the AssetReuse ledger: the
// canonical path plus size and modification time, the triple storage.h
// defines as "the same bytes". Any stat failure means NO stamp: the safe
// answer to not knowing is to copy again.
bool stampOf(const std::filesystem::path& source, openshow::domain::AssetReuse::Stamp* out) {
    std::error_code ec;
    const std::filesystem::path canonicalSource = std::filesystem::canonical(source, ec);
    if (!ec) {
        const std::uintmax_t size = std::filesystem::file_size(canonicalSource, ec);
        if (!ec) {
            const auto modified = std::filesystem::last_write_time(canonicalSource, ec);
            if (!ec) {
                *out = {canonicalSource, size, modified};
                return true;
            }
        }
    }
    return false;
}

std::string osError(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

// Durability barrier over a directory (task 6.13a): a rename — of a
// staged asset or of the manifest — is durable only once the directory
// holding it has been synced. Faulted syncs report the refusal and do
// not run, so a fixture proves a barrier is present by proving its
// absence makes the save succeed where it must not.
bool syncDirectory(const std::filesystem::path& directory, const char* faultStep,
                   std::string* error) {
    if (storageFault(faultStep)) {
        fail(error, "syncing the directory of '" + directory.string() +
                        "' failed: simulated fault at '" + faultStep + "'");
        return false;
    }
    const int fd = ::open(directory.c_str(), O_RDONLY);
    if (fd < 0) {
        fail(error, osError("cannot open '" + directory.string() + "' for syncing"));
        return false;
    }
    const int syncStatus = ::fsync(fd);
    const int syncErrno = errno;
    const int closeStatus = ::close(fd);
    if (syncStatus != 0) {
        fail(error, osError("syncing directory '" + directory.string() + "'"));
        return false;
    }
    if (closeStatus != 0) {
        fail(error, osError("closing directory '" + directory.string() + "' after syncing"));
        return false;
    }
    return true;
}

// Copy one asset's bytes into the package WITHOUT ever touching the file
// the CURRENT manifest may be naming (task 6.13a, review R8): stream into
// a unique staging file inside assets/, sync it, then rename it onto the
// destination. The rename is atomic — a reader of the previous manifest
// sees either the whole old file or the whole new one — and an
// interrupted copy dies under a staging name nothing references. The old
// copy_file(overwrite_existing) smeared partial bytes over the file the
// still-valid manifest named; that is the defect this replaces.
bool stageAndInstallAsset(const Asset& asset, const std::filesystem::path& source,
                          const std::filesystem::path& destination, std::string* error) {
    static unsigned stagingCounter = 0;
    const std::string stagingName =
        "." + destination.filename().string() + ".openshow-staging-" +
        std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(++stagingCounter);
    const std::filesystem::path staging = destination.parent_path() / stagingName;
    const int in = ::open(source.c_str(), O_RDONLY);
    if (in < 0) {
        fail(error, "asset '" + asset.name + "' has no readable file at '" + asset.sourcePath +
                        "' (opening it failed)");
        return false;
    }
    const int out = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        ::close(in);
        fail(error, osError("cannot create staging file '" + staging.string() + "'"));
        return false;
    }
    const auto abandon = [out, &staging] {
        ::close(out);
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
    };
    const auto copyFailed = [&, error](const std::string& why) {
        abandon();
        fail(error, "copying asset '" + asset.name + "' into the package failed: " + why);
        return false;
    };
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        const ssize_t got = ::read(in, buffer.data(), buffer.size());
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return copyFailed(osError("reading '" + asset.sourcePath + "'"));
        }
        if (got == 0) {
            break;  // the whole file streamed; the staging name holds every byte
        }
        if (storageFault("asset-copy")) {
            return copyFailed("simulated fault at 'asset-copy' — the staged copy "
                              "stopped here and the destination was never touched");
        }
        ssize_t written = 0;
        while (written < got) {
            const ssize_t n = ::write(out, buffer.data() + written,
                                      static_cast<size_t>(got - written));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return copyFailed(osError("writing the staging copy of '" + asset.name + "'"));
            }
            written += n;
        }
    }
    ::close(in);
    // The staged bytes are durable BEFORE the rename puts them where a
    // committed manifest can point: that is the whole point of the sync.
    const bool fsyncFault = storageFault("asset-fsync");
    const int syncStatus = ::fsync(out);
    const int syncErrno = errno;
    if (fsyncFault) {
        return copyFailed("the staged copy could not be synced: simulated fault at "
                          "'asset-fsync'");
    }
    if (syncStatus != 0) {
        errno = syncErrno;
        return copyFailed(osError("syncing the staged copy of '" + asset.name + "'"));
    }
    const int closeStatus = ::close(out);
    if (closeStatus != 0) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        fail(error, "copying asset '" + asset.name + "' into the package failed: " +
                        osError("closing the staged copy"));
        return false;
    }
    if (storageFault("asset-rename")) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        fail(error, "installing the staged copy of asset '" + asset.name +
                        "' failed: simulated fault at 'asset-rename'");
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(staging, destination, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        fail(error, "installing the staged copy of asset '" + asset.name +
                        "' failed: " + ec.message());
        return false;
    }
    return true;
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

// A loaded id must be a NAME, not a path (task 6.13b, review R8): the
// store issues ids of the form "asset-12", loadService restores ids
// verbatim, and an asset's id is the prefix of the filename its bytes
// are written under. The charset rule is what makes that safe for
// packages from anywhere — the store itself can never mint a violation.
bool safeOpaqueId(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
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

// True when `candidate` sits at `root` or inside it, compared COMPONENT
// by component (task 6.13b, review R8): a string prefix is not a
// directory boundary, and "/tmp/pkg-evil" string-starts-with "/tmp/pkg".
// Inputs must be canonical or at least lexically normal.
bool insidePackage(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() || *candidatePart != *rootPart) {
            return false;
        }
    }
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
    // `reuse`, when given, may skip copying an asset whose installed
    // bytes are still identified (task 6.14b; see storage.h). Callers
    // that own a package wholesale (saveService, saveServiceAndAdopt)
    // pass nothing and always copy.
    static bool save(const Documents& documents, const std::filesystem::path& directory,
                     std::string* error, AssetReuse* reuse = nullptr);
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
            // The slide's words, as a stack of boxes in draw order (task
            // 6.17a). Each box carries the id every later edit names it by,
            // which is why the list is written at all: order and identity are
            // the document's, not the loader's guess. A format-1 file's lone
            // "lines" plus slide-level geometry is read on the way in and
            // promoted to this shape on the way out.
            boost::json::array boxes;
            for (const TextBox& box : slide.items) {
                boost::json::object boxJson;
                boxJson.emplace("id", box.id);
                boost::json::array lines;
                for (const std::string& line : box.lines) {
                    lines.emplace_back(line);
                }
                boxJson.emplace("lines", std::move(lines));
                boxJson.emplace("x", box.x);
                boxJson.emplace("y", box.y);
                boxJson.emplace("width", box.width);
                boxJson.emplace("height", box.height);
                boxes.push_back(std::move(boxJson));
            }
            slideJson.emplace("items", std::move(boxes));
            slideJson.emplace("notes", slide.notes);
            slideJson.emplace("styleId", slide.styleId);
            slideJson.emplace("backgroundAssetId", slide.backgroundAssetId);
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
        // Typography (task 4.4) always travels; the loader defaults it
        // for styles written before it existed.
        entry.emplace("align", style.align);
        entry.emplace("lineSpacing", style.lineSpacing);
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
    // Format 1 — one text box per slide, written as the slide's own "lines"
    // plus x/y/width/height — is READ rather than rejected: it is what every
    // service on disk says until this build saves it again (task 6.17a). A
    // future version is refused naming both numbers, because guessing at a
    // format means quietly dropping whatever the newer build's operator wrote.
    if (format != kServiceFormat && format != kServiceFormatLegacyBoxes) {
        fail(error, "service.json: unsupported format " + std::to_string(format) +
                        " (this build reads format " + std::to_string(kServiceFormat) +
                        ", and format " + std::to_string(kServiceFormatLegacyBoxes) +
                        " with its single text box migrated to a list)");
        return false;
    }
    const bool singleBoxFile = format == kServiceFormatLegacyBoxes;
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
        if (!safeOpaqueId(id)) {
            fail(error, std::string("service.json: ") + what + " has an unsafe id '" + id +
                            "' (ids are opaque: letters, digits, '-' and '_')");
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
                !claimId(slide.id, "slide")) {
                return false;
            }
            // A box's fields are READ in whichever shape the file uses and
            // PLACED in the one the model keeps, so everything after this
            // paragraph is common to both formats and a migrated slide cannot
            // mean something slightly different from an authored one.
            if (singleBoxFile) {
                TextBox box;
                // The id is DERIVED, not issued. A load must be a function of
                // its file, so loading the same legacy service twice agrees
                // on which box is which — and the next save writes this id
                // into the file, making the migrated box's identity durable
                // from that moment. It embeds the slide id, claimed unique
                // immediately above, and "-text1" is a suffix no store-issued
                // id can take ("text7" has no slide prefix).
                box.id = slide.id + "-text1";
                if (!wantStringArray(jsonSlide, "lines", &box.lines, error) ||
                    !wantOptionalNumber(jsonSlide, "x", &box.x, error) ||
                    !wantOptionalNumber(jsonSlide, "y", &box.y, error) ||
                    !wantOptionalNumber(jsonSlide, "width", &box.width, error) ||
                    !wantOptionalNumber(jsonSlide, "height", &box.height, error)) {
                    return false;
                }
                if (!claimId(box.id, "text box")) {
                    return false;
                }
                slide.items.push_back(std::move(box));
            } else {
                // Format 2 names its boxes and their order; a slide with no
                // "items" is not a slide this build wrote, and a slide that
                // still carries format-1's "lines" is a file someone hand-
                // bumped. Loading the second as an empty slide would be the
                // quietest possible way to lose somebody's words.
                if (jsonSlide.contains("lines")) {
                    fail(error, "service.json: slide '" + slide.id +
                                    "' is format 1 in a format " +
                                    std::to_string(kServiceFormat) + " file");
                    return false;
                }
                const boost::json::array* boxArray = nullptr;
                if (!wantArray(jsonSlide, "items", &boxArray, error)) {
                    return false;
                }
                if (boxArray->size() > kMaxTextBoxes) {
                    fail(error, "service.json: slide '" + slide.id + "' holds " +
                                    std::to_string(boxArray->size()) +
                                    " text boxes and a slide takes at most " +
                                    std::to_string(kMaxTextBoxes));
                    return false;
                }
                for (const boost::json::value& boxValue : *boxArray) {
                    if (!boxValue.is_object()) {
                        fail(error, "service.json: slide '" + slide.id +
                                        "' holds a text box that is not an object");
                        return false;
                    }
                    const boost::json::object& jsonBox = boxValue.as_object();
                    TextBox box;
                    if (!wantString(jsonBox, "id", &box.id, error) ||
                        !claimId(box.id, "text box") ||
                        !wantStringArray(jsonBox, "lines", &box.lines, error) ||
                        !wantNumber(jsonBox, "x", &box.x, error) ||
                        !wantNumber(jsonBox, "y", &box.y, error) ||
                        !wantNumber(jsonBox, "width", &box.width, error) ||
                        !wantNumber(jsonBox, "height", &box.height, error)) {
                        return false;
                    }
                    slide.items.push_back(std::move(box));
                }
            }
            if (!wantOptionalString(jsonSlide, "group", &slide.group, error) ||
                !wantOptionalString(jsonSlide, "notes", &slide.notes, error) ||
                !wantOptionalString(jsonSlide, "styleId", &slide.styleId, error) ||
                !wantOptionalString(jsonSlide, "backgroundAssetId",
                                    &slide.backgroundAssetId, error)) {
                return false;
            }
            for (const TextBox& box : slide.items) {
                if (!std::isfinite(box.x) || !std::isfinite(box.y) ||
                    !std::isfinite(box.width) || !std::isfinite(box.height) ||
                    box.width <= 0.0 || box.height <= 0.0) {
                    fail(error, "service.json: slide '" + slide.id + "' holds text box '" +
                                    box.id + "' that cannot be placed");
                    return false;
                }
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
            !wantNumber(jsonStyle, "fontSize", &style.fontSize, error) ||
            !wantOptionalString(jsonStyle, "align", &style.align, error) ||
            !wantOptionalNumber(jsonStyle, "lineSpacing", &style.lineSpacing, error)) {
            return false;
        }
        // "align" arrived after the first services: wantOptionalString
        // blanks an absent key, and blank is how a pre-typography style
        // says "centred", the only drawing it ever knew.
        if (style.align.empty()) {
            style.align = "center";
        }
        // A loaded style must be one the renderer can honour: the same
        // contract createStyle/updateStyle enforce.
        if (!std::isfinite(style.fontSize) || style.fontSize < 0.0 ||
            !openshow::domain::isStyleAlign(style.align) ||
            !std::isfinite(style.lineSpacing) || style.lineSpacing < 0.25 ||
            style.lineSpacing > 8.0) {
            fail(error, "service.json: style '" + style.id +
                            "' holds typography the renderer cannot honour");
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
                        std::string* error, AssetReuse* reuse) {
    // Slow-storage seam (task 6.14b): the hold happens BEFORE any effect,
    // so the whole save is one in-flight window a decision must order
    // against — not a torn half.
    const int delayMs = storageDelayMs();
    if (delayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        fail(error, "cannot create '" + directory.string() + "': " + ec.message());
        return false;
    }

    // Assets first, staged: a service.json that exists must name only
    // files that exist and are COMPLETE (task 6.13a, review R8). Each
    // copy lands through its own staging file and atomic rename, so the
    // file the previous manifest names is never written in place.
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
        // Asset copy reuse (task 6.14b, review R14): text edits must not
        // re-copy unchanged media into the scratch package. Skip only
        // when the destination still exists AND the source still
        // identifies as the bytes last installed there; anything else —
        // replaced file, new name, deleted destination — copies fresh.
        AssetReuse::Stamp stamp;
        const bool stamped = reuse != nullptr && stampOf(source, &stamp);
        if (stamped) {
            const auto previous = reuse->byDestination.find(destination.string());
            std::error_code haveDest;
            if (previous != reuse->byDestination.end() && previous->second == stamp &&
                std::filesystem::is_regular_file(destination, haveDest) && !haveDest) {
                continue;
            }
        }
        if (!stageAndInstallAsset(asset, source, destination, error)) {
            return false;
        }
        if (stamped) {
            reuse->byDestination[destination.string()] = std::move(stamp);
        }
    }
    // The renames above changed the assets directory's CONTENTS; those
    // directory entries must be durable before any manifest that names
    // them is committed (task 6.13a: "commit the manifest only after all
    // referenced bytes are durable").
    if (!documents.fAssets.empty() &&
        !syncDirectory(directory / kAssetsDir, "assets-fsync", error)) {
        return false;
    }

    // The manifest commits through a temporary in the same directory, so
    // the rename is atomic against readers of service.json — and it is
    // synced before that rename. Raw writes mean there is no user-space
    // buffer left unsent when write() returns (the flush, in std::stream
    // terms); fsync is the durability barrier; close is where deferred
    // write-back errors surface, and its status is REPORTED — the old
    // code checked good() before the destructor silently swallowed the
    // close failure (task 6.13a, review R8).
    const std::filesystem::path temporary =
        directory / (std::string(kManifestName) + ".tmp");
    const std::string manifest = boost::json::serialize(toJson(documents));
    if (storageFault("manifest-write")) {
        fail(error, "writing '" + temporary.string() +
                        "' failed: simulated fault at 'manifest-write'");
        return false;
    }
    {
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fail(error, "cannot write '" + temporary.string() + "': " + std::strerror(errno));
            return false;
        }
        const auto abandon = [&temporary, fd] {
            ::close(fd);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        };
        size_t offset = 0;
        while (offset < manifest.size()) {
            const ssize_t n = ::write(fd, manifest.data() + offset, manifest.size() - offset);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                abandon();
                fail(error, osError("writing '" + temporary.string() + "'"));
                return false;
            }
            offset += static_cast<size_t>(n);
        }
        const bool fsyncFault = storageFault("manifest-fsync");
        const int syncStatus = ::fsync(fd);
        const int syncErrno = errno;
        if (fsyncFault) {
            abandon();
            fail(error, "syncing the new manifest failed: simulated fault at "
                        "'manifest-fsync'");
            return false;
        }
        if (syncStatus != 0) {
            abandon();
            errno = syncErrno;
            fail(error, osError("syncing the new manifest"));
            return false;
        }
        // POSIX frees the file descriptor even when close() reports a
        // failure, so closing once and reporting the status matches what
        // a real refusing device leaves behind.
        const bool closeFault = storageFault("manifest-close");
        const int closeStatus = ::close(fd);
        if (closeFault) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            fail(error, "closing '" + temporary.string() +
                            "' after syncing failed: simulated fault at 'manifest-close'");
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
    if (storageFault("manifest-rename")) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        fail(error, "installing '" + manifestPath(directory) +
                        "' failed: simulated fault at 'manifest-rename'");
        return false;
    }
    std::filesystem::rename(temporary, directory / kManifestName, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        fail(error, "installing '" + manifestPath(directory) + "' failed: " + ec.message());
        return false;
    }
    // The final barrier: the rename is durable only when the containing
    // directory's entry change is. This is the ONE reported failure that
    // can leave the new manifest installed — an un-renaming would itself
    // rewrite the committed name — so the save says exactly where it
    // stands, and the package is consistent either way: the rename was
    // atomic, so a crash here leaves a reader on the old manifest or the
    // new one, and both name files whose bytes and directory entries are
    // already synced.
    return syncDirectory(directory, "dir-fsync", error);
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

    // Package-relative containment (task 6.13b, review R8): the recorded
    // file names bytes INSIDE this package. Absolute paths and paths
    // that climb out are refused as a matter of name — before any
    // filesystem is asked about them — and the final word is canonical:
    // every symlink inside the package is followed to where it actually
    // leads, and a file that resolves outside (assets/link -> /etc/passwd,
    // or -> the sibling directory whose name starts with this one) is a
    // package that is not self-contained, refused without touching the
    // target. An authorised IMPORT still copies external bytes in at
    // save; that is the operator's choice, not a manifest's.
    std::error_code rootEc;
    const std::filesystem::path root = std::filesystem::canonical(directory, rootEc);
    if (rootEc) {
        fail(error, "cannot resolve the package directory '" + directory.string() +
                        "': " + rootEc.message());
        return false;
    }
    for (Asset& asset : loaded.fAssets) {
        const std::filesystem::path recorded(asset.sourcePath);
        if (recorded.is_absolute()) {
            fail(error, "service.json: asset '" + asset.name + "' names an absolute file '" +
                            asset.sourcePath + "'; asset files are package-relative");
            return false;
        }
        for (const std::filesystem::path& part : recorded) {
            if (part == "..") {
                fail(error, "service.json: asset '" + asset.name +
                                "' names a file that climbs out of the package '" +
                                asset.sourcePath + "'");
                return false;
            }
        }
        const std::filesystem::path file =
            (directory / asset.sourcePath).lexically_normal();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec) || ec) {
            fail(error, "service.json: asset '" + asset.name + "' file '" + file.string() +
                            "' is missing from the package");
            return false;
        }
        std::error_code linkEc;
        const std::filesystem::path resolved = std::filesystem::canonical(file, linkEc);
        if (linkEc) {
            fail(error, "service.json: asset '" + asset.name + "' file '" + file.string() +
                            "' cannot be resolved: " + linkEc.message());
            return false;
        }
        if (!insidePackage(root, resolved)) {
            fail(error, "service.json: asset '" + asset.name + "' resolves outside the package ('" +
                            file.string() + "' -> '" + resolved.string() +
                            "'); a package must be self-contained");
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

bool saveServiceAndAdopt(Documents& documents, const std::filesystem::path& directory,
                         std::vector<AssetRebind>* rebinds, std::string* error) {
    if (!ServiceCodec::save(documents, directory, error)) {
        return false;  // nothing was copied: nothing has a new home
    }
    // A successful save means every asset's bytes are in the package under
    // the name the codec picks; the adoption is the same computation, and
    // the model changes only for assets that were NOT already there.
    std::vector<AssetRebind> moved;
    for (const Id& id : documents.assetIds()) {
        const Asset* asset = documents.findAsset(id);
        const std::filesystem::path packaged =
            (directory / kAssetsDir / assetFileName(*asset)).lexically_normal();
        std::error_code ec;
        if (std::filesystem::equivalent(std::filesystem::path(asset->sourcePath), packaged, ec) &&
            !ec) {
            continue;  // already package-owned: a re-save of a loaded service
        }
        moved.push_back({asset->id, asset->sourcePath, packaged.string()});
    }
    documents.adoptAssets(moved);
    if (rebinds != nullptr) {
        *rebinds = std::move(moved);
    }
    return true;
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
    if (!ServiceCodec::save(documents, fDirectory, error, &fReuse)) {
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
    // The recovered package IS the installed state; its assets now point
    // inside the scratch tree, so the equivalent-source rule covers them
    // and no stale external stamp may survive.
    fReuse.byDestination.clear();
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
    fReuse.byDestination.clear();  // the destinations are gone with it
    return true;
}

bool Autosave::exists(const std::filesystem::path& directory) {
    std::error_code ec;
    return std::filesystem::is_regular_file(directory / kManifestName, ec);
}

}  // namespace openshow::domain
