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
// no longer say what it means in the format-1 shape. Format 3 (task 6.17b,
// review R17) gives every item in that list a "kind": the stack is text and
// pictures in one draw order, a picture names its asset by id, and a
// format-2 box — words, always — is that item with kind "text". Format 4
// (task 6.17c1, review R17) gives every SLIDE a "fit": how its background
// picture sits on the canvas, where cover is what every earlier file drew.
// Format 5 (task 6.17c3, review R17) gives every THEME a
// "backgroundGradient": the second colour of a gradient background, where
// the empty string is the flat fill every earlier file drew. Format 6
// (task TH.1, themes plan) names the appearance entity THEME throughout:
// the array is "themes", a slide's assignment is "themeId" and a show's
// default is "defaultThemeId". The reader still loads 1 through 5 and
// migrates them — a 5-file has no gradient to lose and a pre-6 file's
// three old key names are chosen by the declared format — and it refuses
// a file that MIXES the spellings: a document half-renamed in a text
// editor is ambiguity to refuse, not intent to guess, the same rule the
// gradient stated. The writer only ever emits kServiceFormat, and a
// 6-file read by an older build refuses the whole package rather than
// quietly drawing what it cannot name.
// Format 7 (task TF.1) adds the theme's named text contexts
// ("themes"[i]["contexts"], typography and box overrides only — there is
// no line-count field to store) and a slide's "textContext" name. Both are
// additive: a 6-file loads with no contexts and every slide untextured,
// drawing exactly the pixels it drew before, and the keys are era-guarded
// so a context cannot hide in a file that predates the concept. The writer
// only ever emits kServiceFormat, and a
// 6-file read by an older build refuses the whole package rather than
// quietly drawing what it cannot name.
// Format 9 (task TR.1, transition contract) adds the TRANSITION record: the
// service's own answer under "transition", and an optional whole-record
// override of the same shape on a theme (the Songs view) and on a named text
// context (the Bible view). Nothing moves and nothing is inferred: a 8-file
// loads with the service holding the 500 ms words / 800 ms media crossfade it
// always performed and every theme and context silent, so an untouched show
// resolves — and lands — exactly as it did before the setting existed. The
// keys are era-guarded like every earlier meaning: a "transition" inside a
// 8-file is somebody else's build meaning something by it.
constexpr unsigned long long kServiceFormat = 9;
// The older versions this build reads: single-box slides (1), a stack whose
// every item was necessarily text (2), a kinded stack whose slides had no
// fit choice yet — cover was the only thing the worker knew (3) — a fitted
// stack whose themes had no gradient yet — flat was the only thing it knew
// (4) — a gradient stack that still spelled the appearance entity
// "style" in its key names (5) — a themed stack whose themes carried no
// text contexts yet (6) — a contexted stack that knew nothing of text
// style (7) — and a styled stack that knew nothing of transitions (8).
constexpr unsigned long long kServiceFormatLegacyBoxes = 1;
constexpr unsigned long long kServiceFormatLegacyTextOnly = 2;
constexpr unsigned long long kServiceFormatLegacyUnfit = 3;
constexpr unsigned long long kServiceFormatLegacyFlat = 4;
constexpr unsigned long long kServiceFormatLegacyStyleNamed = 5;
constexpr unsigned long long kServiceFormatLegacyContextless = 6;
// The FORMAT that introduced the context spellings — the guards quote the
// introducer, never the moving kServiceFormat (the fit's rule), because
// format 8 is current while "contexts" is 7's own meaning.
constexpr unsigned long long kServiceFormatContexts = 7;
// And 8 introduces the text style fields. Named for the same reason: the
// day TS.3 or TR.1 claims 9, every style guard must still read "8", the
// format where these keys got their meaning.
constexpr unsigned long long kServiceFormatTextStyle = 8;
// And 9 introduces the transition record (task TR.1). Named so a later leaf
// claiming 10 cannot weaken the guard that says where "transition" means
// what this build reads.
constexpr unsigned long long kServiceFormatTransition = 9;
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

// The OPTIONAL-number reader for theme members that answer with silence
// (task TS.1): absent keeps the theme's non-answer (resolution then walks
// to the next layer), unlike wantOptionalNumber's default-on-absent.
bool wantOptionalNumberField(const boost::json::object& obj, const char* key,
                             std::optional<double>* out, std::string* error) {
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

// Sparse-field readers for the customization set (task TH.2): absent is
// "not customized", present is that slide's own answer — even "" — and
// anything of the wrong shape is refused naming the field.
bool wantCustomString(const boost::json::object& obj, const char* key,
                      std::optional<std::string>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_string()) {
        fail(error, std::string("service.json: customization '") + key + "' must be a string");
        return false;
    }
    *out = std::string(value->as_string());
    return true;
}

bool wantCustomNumber(const boost::json::object& obj, const char* key,
                      std::optional<double>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_number()) {
        fail(error, std::string("service.json: customization '") + key + "' must be a number");
        return false;
    }
    *out = value->to_number<double>();
    return true;
}

// An optional INTEGER (task TS.1): the CSS weight scale is whole numbers,
// and "400.5" is a hand-edit to refuse, not a weight to round. An absent
// key keeps the caller's silence.
bool wantOptionalInteger(const boost::json::object& obj, const char* key,
                         std::optional<int>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;
    }
    double number = 0.0;
    if (value->is_int64() || value->is_uint64()) {
        number = value->to_number<double>();
    } else if (value->is_number()) {
        number = value->to_number<double>();
        if (number != std::floor(number)) {
            fail(error, std::string("service.json: '") + key + "' must be an integer");
            return false;
        }
    } else {
        fail(error, std::string("service.json: '") + key + "' must be an integer");
        return false;
    }
    *out = static_cast<int>(number);
    return true;
}

bool wantOptionalJsonString(const boost::json::object& obj, const char* key,
                            std::optional<std::string>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_string()) {
        fail(error, std::string("service.json: '") + key + "' must be a string");
        return false;
    }
    *out = std::string(value->as_string());
    return true;
}

bool wantCustomInteger(const boost::json::object& obj, const char* key,
                       std::optional<int>* out, std::string* error) {
    return wantOptionalInteger(obj, key, out, error);
}

bool wantCustomJsonString(const boost::json::object& obj, const char* key,
                          std::optional<std::string>* out, std::string* error) {
    return wantOptionalJsonString(obj, key, out, error);
}

// The effect records on the wire (task TS.1). Both travel WHOLE: a record
// is `enabled` plus its complete look, never a fragment, because half an
// effect has no meaning to resolve — the theme-editor review's atomicity
// rule. The closed member set refuses a stray key naming itself, and the
// bands are the shared domain sentence (validStroke / validShadow), so
// what loads is exactly what a verb would have accepted.
boost::json::object strokeJson(const TextStroke& stroke) {
    boost::json::object out;
    out.emplace("enabled", stroke.enabled);
    out.emplace("color", stroke.color);
    out.emplace("width", stroke.width);
    return out;
}

boost::json::object shadowJson(const TextShadow& shadow) {
    boost::json::object out;
    out.emplace("enabled", shadow.enabled);
    out.emplace("color", shadow.color);
    out.emplace("opacity", shadow.opacity);
    out.emplace("offsetX", shadow.offsetX);
    out.emplace("offsetY", shadow.offsetY);
    out.emplace("blurSigma", shadow.blurSigma);
    return out;
}

bool wantStroke(const boost::json::object& obj, const char* key, const std::string& owner,
                std::optional<TextStroke>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;  // no opinion; the layers under it answer
    }
    if (!value->is_object()) {
        fail(error, "service.json: " + owner + " carries \"stroke\" that is not an effect record");
        return false;
    }
    const boost::json::object& record = value->as_object();
    for (const auto& entry : record) {
        const std::string name(entry.key());
        if (name != "enabled" && name != "color" && name != "width") {
            fail(error, "service.json: " + owner + " stroke has no field '" + name + "'");
            return false;
        }
    }
    TextStroke stroke;
    const boost::json::value* enabled = field(record, "enabled");
    const boost::json::value* color = field(record, "color");
    const boost::json::value* width = field(record, "width");
    if (enabled == nullptr || !enabled->is_bool() || color == nullptr || !color->is_string() ||
        width == nullptr || !width->is_number()) {
        fail(error, "service.json: " + owner +
                        " stroke is a whole record or nothing: it needs a boolean \"enabled\", "
                        "a string \"color\" and a number \"width\"");
        return false;
    }
    stroke.enabled = enabled->as_bool();
    stroke.color = std::string(color->as_string());
    stroke.width = width->to_number<double>();
    // Bands and colours are NOT re-derived here: the customization contract
    // and the theme's own load-time re-vet (the domain's single sentence,
    // validStroke included) run on the object this reader produced.
    *out = stroke;
    return true;
}

bool wantShadow(const boost::json::object& obj, const char* key, const std::string& owner,
                std::optional<TextShadow>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_object()) {
        fail(error, "service.json: " + owner + " carries \"shadow\" that is not an effect record");
        return false;
    }
    const boost::json::object& record = value->as_object();
    for (const auto& entry : record) {
        const std::string name(entry.key());
        if (name != "enabled" && name != "color" && name != "opacity" && name != "offsetX" &&
            name != "offsetY" && name != "blurSigma") {
            fail(error, "service.json: " + owner + " shadow has no field '" + name + "'");
            return false;
        }
    }
    TextShadow shadow;
    const boost::json::value* enabled = field(record, "enabled");
    const boost::json::value* color = field(record, "color");
    const boost::json::value* opacity = field(record, "opacity");
    const boost::json::value* offsetX = field(record, "offsetX");
    const boost::json::value* offsetY = field(record, "offsetY");
    const boost::json::value* blur = field(record, "blurSigma");
    if (enabled == nullptr || !enabled->is_bool() || color == nullptr || !color->is_string() ||
        opacity == nullptr || !opacity->is_number() || offsetX == nullptr ||
        !offsetX->is_number() || offsetY == nullptr || !offsetY->is_number() ||
        blur == nullptr || !blur->is_number()) {
        fail(error, "service.json: " + owner +
                        " shadow is a whole record or nothing: it needs a boolean \"enabled\", "
                        "a string \"color\" and numbers \"opacity\", \"offsetX\", \"offsetY\" "
                        "and \"blurSigma\"");
        return false;
    }
    shadow.enabled = enabled->as_bool();
    shadow.color = std::string(color->as_string());
    shadow.opacity = opacity->to_number<double>();
    shadow.offsetX = offsetX->to_number<double>();
    shadow.offsetY = offsetY->to_number<double>();
    shadow.blurSigma = blur->to_number<double>();
    // Like the stroke: shape here, bands right after — the customization
    // contract re-vets the whole set, the theme's own load re-vets its
    // style block, and both quote the domain's single sentence.
    *out = shadow;
    return true;
}

// The transition record on the wire (task TR.1): {kind, textDurationMs,
// mediaDurationMs}. It is ATOMIC like the effect records — half a transition
// has nothing to resolve — so the closed member set refuses a stray key
// naming itself, the kind must be one of the two names, and the durations
// must be whole milliseconds ("500.5" is a hand-edit, not a duration to
// round, wantOptionalInteger's rule). The bands are not re-derived here: the
// shared domain sentence, Documents::validTransitionOptions, runs on the
// record this reader produced, so a file that loads is a file a verb would
// have accepted and a renderer will honour.
boost::json::object transitionJson(const TransitionOptions& options) {
    boost::json::object out;
    out.emplace("kind", transitionKindName(options.kind));
    out.emplace("textDurationMs", options.textDurationMs);
    out.emplace("mediaDurationMs", options.mediaDurationMs);
    return out;
}

bool transitionRecord(const boost::json::object& record, const std::string& owner,
                      TransitionOptions* out, std::string* error) {
    for (const auto& entry : record) {
        const std::string name(entry.key());
        if (name != "kind" && name != "textDurationMs" && name != "mediaDurationMs") {
            fail(error, "service.json: " + owner + " transition has no field '" + name + "'");
            return false;
        }
    }
    const boost::json::value* kind = field(record, "kind");
    const boost::json::value* text = field(record, "textDurationMs");
    const boost::json::value* media = field(record, "mediaDurationMs");
    if (kind == nullptr || !kind->is_string() || text == nullptr || !text->is_number() ||
        media == nullptr || !media->is_number()) {
        fail(error, "service.json: " + owner +
                        " transition is a whole record or nothing: it needs a string \"kind\""
                        " and whole numbers \"textDurationMs\" and \"mediaDurationMs\"");
        return false;
    }
    if (!transitionKindFromName(std::string(kind->as_string()), &out->kind)) {
        fail(error, "service.json: " + owner + " transition kind is '" +
                        std::string(kind->as_string()) + "', not \"fade\" or \"cut\"");
        return false;
    }
    auto wholeMs = [&](const boost::json::value* value, const char* key, int* destination) -> bool {
        const double number = value->to_number<double>();
        if (number != std::floor(number)) {
            fail(error, "service.json: " + owner + " transition '" + key +
                            "' is not a whole number of milliseconds");
            return false;
        }
        *destination = static_cast<int>(number);
        return true;
    };
    if (!wholeMs(text, "textDurationMs", &out->textDurationMs) ||
        !wholeMs(media, "mediaDurationMs", &out->mediaDurationMs)) {
        return false;
    }
    std::string detail;
    if (!Documents::validTransitionOptions(*out, &detail)) {
        fail(error, "service.json: " + owner + " carries a transition the renderer cannot"
                        " honour: " + detail);
        return false;
    }
    return true;
}

// The OPTIONAL site: a theme or a context that says nothing about movement.
// Absent leaves the caller's silence, which is what makes the layers under it
// answer; present must be a complete record.
bool wantTransition(const boost::json::object& obj, const char* key, const std::string& owner,
                    std::optional<TransitionOptions>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;  // no opinion; the layers under it answer
    }
    if (!value->is_object()) {
        fail(error, "service.json: " + owner + " carries \"transition\" that is not a"
                        " transition record");
        return false;
    }
    TransitionOptions options;
    if (!transitionRecord(value->as_object(), owner, &options, error)) {
        return false;
    }
    *out = options;
    return true;
}

// The lyric-box placement on the wire (themes plan amendment 2026-11-01).
// A theme's placement and a slide's placement CUSTOMIZATION are the same
// shape — {x,y,width,height} in the 1920x1080 authored canvas — and both
// are OPTIONAL: an absent key is the statement "no opinion about where
// words go", never a zero-sized box. A present rectangle gets the authored
// box's own vetting at load time, because a box this build could not place
// must fail at load naming its owner rather than at present time.
boost::json::object boxRectJson(const BoxRect& rect) {
    boost::json::object out;
    out.emplace("x", rect.x);
    out.emplace("y", rect.y);
    out.emplace("width", rect.width);
    out.emplace("height", rect.height);
    return out;
}

// A theme's named text context on the wire (task TF.1): the name always
// travels (it IS the entry), and every override only when the context
// carries it — a context silent on a field falls through exactly like a
// theme silent on it, and spelling silences as nulls would make the
// reader guess between two kinds of nothing (placement's rule). The
// transition override (task TR.1) follows the same discipline: present
// exactly when this context speaks for itself.
boost::json::object textContextJson(const TextContext& context) {
    boost::json::object out;
    out.emplace("name", context.name);
    if (context.fontFamily.has_value()) {
        out.emplace("fontFamily", *context.fontFamily);
    }
    if (context.fontSize.has_value()) {
        out.emplace("fontSize", *context.fontSize);
    }
    if (context.lineSpacing.has_value()) {
        out.emplace("lineSpacing", *context.lineSpacing);
    }
    if (context.align.has_value()) {
        out.emplace("align", *context.align);
    }
    if (context.box.has_value()) {
        out.emplace("box", boxRectJson(*context.box));
    }
    if (context.inset.has_value()) {
        out.emplace("inset", *context.inset);
    }
    if (context.transition.has_value()) {
        out.emplace("transition", transitionJson(*context.transition));
    }
    return out;
}

bool wantBoxRect(const boost::json::object& obj, const char* key, const std::string& owner,
                 std::optional<BoxRect>* out, std::string* error) {
    const boost::json::value* value = field(obj, key);
    if (value == nullptr) {
        return true;  // no opinion, and that is a complete answer
    }
    if (!value->is_object()) {
        fail(error, "service.json: " + owner + " carries \"placement\" that is not an object");
        return false;
    }
    const boost::json::object& jsonRect = value->as_object();
    BoxRect rect;
    double* parts[4] = {&rect.x, &rect.y, &rect.width, &rect.height};
    const char* const names[4] = {"x", "y", "width", "height"};
    for (size_t i = 0; i < 4; ++i) {
        const boost::json::value* part = field(jsonRect, names[i]);
        if (part == nullptr || !part->is_number()) {
            fail(error, "service.json: " + owner + " placement needs a number \"" + names[i] +
                            "\"");
            return false;
        }
        *parts[i] = part->to_number<double>();
    }
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height) || rect.width <= 0.0 || rect.height <= 0.0) {
        fail(error, "service.json: " + owner + " placement cannot be placed: it needs finite "
                        "coordinates and positive width and height");
        return false;
    }
    *out = rect;
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
    // The service's one current theme (task TH.2). Written even when it is
    // "" — "no current theme" is today's answer to tomorrow's question,
    // not a key to be missed.
    root.emplace("currentThemeId", documents.fCurrentThemeId);
    // The service's transition answer (task TR.1) travels like currentThemeId:
    // written even when it is the default, because "500 ms words, 800 ms
    // media, crossfade" is today's answer to tomorrow's question rather than a
    // key to be missed. It is the ONE place a service says how its audience
    // moves, and a resolver never has to guess what an absent one meant.
    root.emplace("transition", transitionJson(documents.fTransition));

    boost::json::array shows;
    for (const Show& show : documents.fShows) {
        boost::json::object entry;
        entry.emplace("id", show.id);
        entry.emplace("name", show.name);
        entry.emplace("notes", show.notes);
        entry.emplace("defaultThemeId", show.defaultThemeId);
        // The schedule-owned flag and the scripture provenance (task BI.3)
        // are format-6 members written only when they speak: a Library
        // show's entry gains no keys, so files that pre-date the importer
        // save byte-for-byte as they did.
        if (show.scheduleOwned) {
            entry.emplace("scheduleOwned", true);
        }
        if (show.scripture.has_value()) {
            boost::json::object scriptureJson;
            scriptureJson.emplace("reference", show.scripture->reference);
            scriptureJson.emplace("translation", show.scripture->translation);
            scriptureJson.emplace("attribution", show.scripture->attribution);
            entry.emplace("scripture", std::move(scriptureJson));
        }
        boost::json::array slides;
        for (const Slide& slide : show.slides) {
            boost::json::object slideJson;
            slideJson.emplace("id", slide.id);
            slideJson.emplace("group", slide.group);
            // The slide's content, as a stack in draw order (tasks
            // 6.17a/6.17b). Each item carries the id every later edit names
            // it by, which is why the list is written at all: order and
            // identity are the document's, not the loader's guess. A
            // format-1 file's lone "lines" plus slide-level geometry is read
            // on the way in and promoted to this shape on the way out. The
            // kind is written even for text items: kind is what says which
            // content fields mean something, and leaving "text" to inference
            // would let a future default redefine what an old item drew.
            boost::json::array boxes;
            for (const SlideItem& item : slide.items) {
                boost::json::object itemJson;
                itemJson.emplace("id", item.id);
                itemJson.emplace("kind", item.kind == ItemKind::Image ? "image" : "text");
                if (item.kind == ItemKind::Image) {
                    itemJson.emplace("assetId", item.assetId);
                } else {
                    boost::json::array lines;
                    for (const std::string& line : item.lines) {
                        lines.emplace_back(line);
                    }
                    itemJson.emplace("lines", std::move(lines));
                }
                itemJson.emplace("x", item.x);
                itemJson.emplace("y", item.y);
                itemJson.emplace("width", item.width);
                itemJson.emplace("height", item.height);
                boxes.push_back(std::move(itemJson));
            }
            slideJson.emplace("items", std::move(boxes));
            slideJson.emplace("notes", slide.notes);
            slideJson.emplace("themeId", slide.themeId);
            // The customization set (task TH.2): the object is ALWAYS
            // there — it answers "what did this slide change for itself" —
            // and a field is in it exactly when the slide customized that
            // field. The empty object is a complete answer ("nothing"),
            // read with no inference, the fit's and kind's own discipline.
            boost::json::object customizations;
            const SlideCustomization& changed = slide.customizations;
            if (changed.textColor.has_value()) {
                customizations.emplace("textColor", *changed.textColor);
            }
            if (changed.backgroundColor.has_value()) {
                customizations.emplace("backgroundColor", *changed.backgroundColor);
            }
            if (changed.backgroundGradient.has_value()) {
                customizations.emplace("backgroundGradient", *changed.backgroundGradient);
            }
            if (changed.fontFamily.has_value()) {
                customizations.emplace("fontFamily", *changed.fontFamily);
            }
            if (changed.fontSize.has_value()) {
                customizations.emplace("fontSize", *changed.fontSize);
            }
            if (changed.align.has_value()) {
                customizations.emplace("align", *changed.align);
            }
            if (changed.lineSpacing.has_value()) {
                customizations.emplace("lineSpacing", *changed.lineSpacing);
            }
            // One unit, four numbers (amendment 2026-11-01): a slide that
            // dragged its lyric box owns the whole rectangle, and reverting
            // hands the whole rectangle back to the theme.
            if (changed.placement.has_value()) {
                customizations.emplace("placement", boxRectJson(*changed.placement));
            }
            // The TS.1 style fields: written exactly when the slide speaks,
            // silence written as silence. An effect is one whole record —
            // `enabled:false` travels too, because it IS the slide's answer
            // ("no outline here, theme"), not an absence.
            if (changed.fontWeight.has_value()) {
                customizations.emplace("fontWeight", *changed.fontWeight);
            }
            if (changed.fontStyle.has_value()) {
                customizations.emplace("fontStyle", *changed.fontStyle);
            }
            if (changed.fitMode.has_value()) {
                customizations.emplace("fitMode", *changed.fitMode);
            }
            if (changed.minFontSize.has_value()) {
                customizations.emplace("minFontSize", *changed.minFontSize);
            }
            if (changed.stroke.has_value()) {
                customizations.emplace("stroke", strokeJson(*changed.stroke));
            }
            if (changed.shadow.has_value()) {
                customizations.emplace("shadow", shadowJson(*changed.shadow));
            }
            slideJson.emplace("customizations", std::move(customizations));
            slideJson.emplace("backgroundAssetId", slide.backgroundAssetId);
            // Fit on EVERY slide, cover included — 6.17b1's rule for the
            // item kind: leaving the default to inference is how a future
            // change silently redefines what an old file drew.
            slideJson.emplace("fit", backgroundFitName(slide.backgroundFit));
            // The named text context (task TF.1) travels only when the
            // slide wears one: an absent key is the same silence as an
            // empty name — a song slide said nothing and carries nothing.
            if (!slide.textContext.empty()) {
                slideJson.emplace("textContext", slide.textContext);
            }
            slides.push_back(std::move(slideJson));
        }
        entry.emplace("slides", std::move(slides));
        shows.push_back(std::move(entry));
    }
    root.emplace("shows", std::move(shows));

    boost::json::array themes;
    for (const Theme& theme : documents.fThemes) {
        boost::json::object entry;
        entry.emplace("id", theme.id);
        entry.emplace("name", theme.name);
        entry.emplace("textColor", theme.textColor);
        entry.emplace("backgroundColor", theme.backgroundColor);
        // The gradient colour always travels (6.17c3): the empty string IS
        // the statement "flat", so a flat theme saved by this build reads
        // back flat instead of depending on an absent key.
        entry.emplace("backgroundGradient", theme.backgroundGradient);
        entry.emplace("fontFamily", theme.fontFamily);
        entry.emplace("fontSize", theme.fontSize);
        // Typography (task 4.4) always travels; the loader defaults it
        // for themes written before it existed.
        entry.emplace("align", theme.align);
        entry.emplace("lineSpacing", theme.lineSpacing);
        // Placement is the ONE theme member that stays absent rather than
        // spelling its silence (amendment 2026-11-01): "this theme has no
        // opinion about where the words go" is what every theme written
        // before it meant, and an explicit null would make the reader
        // guess between two kinds of nothing. Present, it is the rectangle
        // a wearing slide's PRIMARY lyric box lands in.
        if (theme.placement.has_value()) {
            entry.emplace("placement", boxRectJson(*theme.placement));
        }
        // Contexts share placement's discipline of silence: the key is
        // present exactly when the theme carries any, and an empty or
        // absent array is the plain pre-TF theme.
        if (!theme.contexts.empty()) {
            boost::json::array contexts;
            for (const TextContext& context : theme.contexts) {
                contexts.push_back(textContextJson(context));
            }
            entry.emplace("contexts", std::move(contexts));
        }
        // The theme's own transition (task TR.1) shares placement's
        // discipline of silence: the key is present exactly when this view
        // says how it moves. A theme that never mentions movement saves with
        // no new key and inherits whatever the resolver walks to next.
        if (theme.transition.has_value()) {
            entry.emplace("transition", transitionJson(*theme.transition));
        }
        // The TS.1 style fields (task TS.1) share the same discipline:
        // present exactly when the theme speaks. A theme that carries no
        // style opinion saves without a single new key, so a show nobody
        // styled round-trips through this build byte-comparably, and the
        // reader's silence means silence — never a guessed override.
        if (theme.fontWeight.has_value()) {
            entry.emplace("fontWeight", *theme.fontWeight);
        }
        if (theme.fontStyle.has_value()) {
            entry.emplace("fontStyle", *theme.fontStyle);
        }
        if (theme.fitMode.has_value()) {
            entry.emplace("fitMode", *theme.fitMode);
        }
        if (theme.minFontSize.has_value()) {
            entry.emplace("minFontSize", *theme.minFontSize);
        }
        if (theme.stroke.has_value()) {
            entry.emplace("stroke", strokeJson(*theme.stroke));
        }
        if (theme.shadow.has_value()) {
            entry.emplace("shadow", shadowJson(*theme.shadow));
        }
        themes.push_back(std::move(entry));
    }
    root.emplace("themes", std::move(themes));

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
    // service on disk says until this build saves it again (task 6.17a).
    // Format 2 — a stack whose items are necessarily text — is read the same
    // way, its items gaining kind "text" on the way in (task 6.17b), and
    // format 3 — a kinded stack whose slides all drew their background
    // cover — gains fit "cover" the same way (task 6.17c1), and format 4 —
    // a fitted stack whose themes all drew a flat background — gains
    // backgroundGradient "" the same way (task 6.17c3). A
    // future version is refused naming both numbers, because guessing at a
    // format means quietly dropping whatever the newer build's operator wrote.
    if (format != kServiceFormat && format != kServiceFormatLegacyBoxes &&
        format != kServiceFormatLegacyTextOnly && format != kServiceFormatLegacyUnfit &&
        format != kServiceFormatLegacyFlat && format != kServiceFormatLegacyStyleNamed &&
        format != kServiceFormatLegacyContextless && format != kServiceFormatContexts &&
        format != kServiceFormatTextStyle) {
        fail(error, "service.json: unsupported format " + std::to_string(format) +
                        " (this build reads format " + std::to_string(kServiceFormat) +
                        " and formats " + std::to_string(kServiceFormatLegacyBoxes) +
                        " through " + std::to_string(kServiceFormatTextStyle) +
                        " with their text boxes migrated to a kinded list, their"
                        " backgrounds to cover and their themes to flat)");
        return false;
    }
    const bool singleBoxFile = format == kServiceFormatLegacyBoxes;
    // A format-2 file's items carry no "kind" — kind was format 3's
    // addition — so 2 means "every item is text, and saying so is lying".
    const bool unkindedList = format == kServiceFormatLegacyTextOnly;
    // Format 6 (task TH.1) spells the appearance entity "theme" in three
    // key names; every earlier file spelled it "style". The declared format
    // picks the key set, and a file that mixes the spellings is refused:
    // half a rename is ambiguity, not intent (the gradient's rule). The
    // test is against the format that INTRODUCED the spelling (6, kept
    // readable as kServiceFormatLegacyContextless), not kServiceFormat —
    // TF.1's 7 changes no key names.
    const bool themeNamed = format >= kServiceFormatLegacyContextless;
    const char* const themesKey = themeNamed ? "themes" : "styles";
    const char* const slideThemeKey = themeNamed ? "themeId" : "styleId";
    const char* const showDefaultThemeKey = themeNamed ? "defaultThemeId" : "defaultStyleId";
    const char* const otherThemeKey = themeNamed ? "theme" : "style";
    auto refuseMixedNaming = [&](const boost::json::object& obj, const char* key,
                                 const std::string& where) -> bool {
        if (!obj.contains(key)) {
            return true;
        }
        fail(error, "service.json: " + where + " carries \"" + key + "\" — a format " +
                        std::to_string(format) + " file names the appearance entity \"" +
                        otherThemeKey + "\", not both");
        return false;
    };
    if (!refuseMixedNaming(root, themeNamed ? "styles" : "themes", "the package")) {
        return false;
    }
    // The service's one current theme (task TH.2) is format-6's to carry.
    // It is read before anything that could report a later problem first:
    // an era violation outranks a missing key, and the naming says which.
    Id currentThemeId;
    if (themeNamed) {
        if (!wantString(root, "currentThemeId", &currentThemeId, error)) {
            return false;
        }
    } else if (root.contains("currentThemeId")) {
        fail(error, "service.json: \"currentThemeId\" is present in a format " +
                        std::to_string(format) + " file, where that key does not exist");
        return false;
    }
    // The service's transition answer (task TR.1) is format 9's to carry, and
    // format 9 always writes it: a 9-file with no "transition" is not a
    // package this build produced, and guessing which fade an unnamed key
    // meant is how a document starts lying about its own output. Absent in an
    // 8-file is no hand-edit either — the 500/800 crossfade those builds
    // performed is exactly what the model loads — while a "transition" in an
    // 8-file is another build's meaning of a key this writer never emitted.
    TransitionOptions transition;
    if (format >= kServiceFormatTransition) {
        const boost::json::value* value = field(root, "transition");
        if (value == nullptr) {
            fail(error, "service.json: a format " + std::to_string(format) +
                            " package has no \"transition\" record of its own");
            return false;
        }
        if (!value->is_object()) {
            fail(error, "service.json: the package carries \"transition\" that is not a"
                        " transition record");
            return false;
        }
        if (!transitionRecord(value->as_object(), "the package", &transition, error)) {
            return false;
        }
    } else if (root.contains("transition")) {
        fail(error, "service.json: \"transition\" is present in a format " +
                        std::to_string(format) + " file, where that key does not exist");
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
            !wantOptionalString(jsonShow, showDefaultThemeKey, &show.defaultThemeId, error)) {
            return false;
        }
        if (!refuseMixedNaming(jsonShow, themeNamed ? "defaultStyleId" : "defaultThemeId",
                               "show '" + show.id + "'")) {
            return false;
        }
        // The schedule-owned flag and the scripture provenance (task BI.3)
        // are format-6 members: a pre-6 file carrying either is another
        // build meaning something by it, and that key is refused, not
        // guessed (the placement rule, applied to shows).
        if (jsonShow.contains("scheduleOwned")) {
            if (!themeNamed) {
                fail(error, "service.json: a \"scheduleOwned\" flag in a format-" +
                                std::to_string(format) + " file, where that key"
                                " does not exist");
                return false;
            }
            const boost::json::value& flag = jsonShow.at("scheduleOwned");
            if (!flag.is_bool()) {
                fail(error, "service.json: show '" + show.id +
                                "' scheduleOwned is not true or false");
                return false;
            }
            show.scheduleOwned = flag.as_bool();
        }
        if (jsonShow.contains("scripture")) {
            if (!themeNamed) {
                fail(error, "service.json: a \"scripture\" block in a format-" +
                                std::to_string(format) + " file, where that key"
                                " does not exist");
                return false;
            }
            const boost::json::value& scriptureValue = jsonShow.at("scripture");
            if (!scriptureValue.is_object()) {
                fail(error, "service.json: show '" + show.id +
                                "' carries \"scripture\" that is not an object");
                return false;
            }
            const boost::json::object& scriptureJson = scriptureValue.as_object();
            ScriptureProvenance scripture;
            if (!wantString(scriptureJson, "reference", &scripture.reference, error) ||
                scripture.reference.empty() ||
                !wantString(scriptureJson, "translation", &scripture.translation, error) ||
                scripture.translation.empty()) {
                if (error != nullptr && error->empty()) {
                    fail(error, "service.json: show '" + show.id +
                                    "' scripture needs a reference and a translation");
                }
                return false;
            }
            if (!wantOptionalString(scriptureJson, "attribution", &scripture.attribution,
                                    error)) {
                return false;
            }
            // Provenance without schedule ownership is a file that cannot
            // say where the item is allowed to be seen; the writer can
            // never produce the pair, so an editor that did is refused.
            if (!show.scheduleOwned) {
                fail(error, "service.json: show '" + show.id +
                                "' carries scripture provenance without being schedule-owned");
                return false;
            }
            show.scripture = std::move(scripture);
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
                SlideItem item;
                // The id is DERIVED, not issued. A load must be a function of
                // its file, so loading the same legacy service twice agrees
                // on which box is which — and the next save writes this id
                // into the file, making the migrated box's identity durable
                // from that moment. It embeds the slide id, claimed unique
                // immediately above, and "-text1" is a suffix no store-issued
                // id can take ("text7" has no slide prefix).
                item.id = slide.id + "-text1";
                if (!wantStringArray(jsonSlide, "lines", &item.lines, error) ||
                    !wantOptionalNumber(jsonSlide, "x", &item.x, error) ||
                    !wantOptionalNumber(jsonSlide, "y", &item.y, error) ||
                    !wantOptionalNumber(jsonSlide, "width", &item.width, error) ||
                    !wantOptionalNumber(jsonSlide, "height", &item.height, error)) {
                    return false;
                }
                if (!claimId(item.id, "slide item")) {
                    return false;
                }
                slide.items.push_back(std::move(item));
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
                if (boxArray->size() > kMaxSlideItems) {
                    fail(error, "service.json: slide '" + slide.id + "' holds " +
                                    std::to_string(boxArray->size()) +
                                    " items and a slide takes at most " +
                                    std::to_string(kMaxSlideItems));
                    return false;
                }
                for (const boost::json::value& boxValue : *boxArray) {
                    if (!boxValue.is_object()) {
                        fail(error, "service.json: slide '" + slide.id +
                                        "' holds a slide item that is not an object");
                        return false;
                    }
                    const boost::json::object& jsonItem = boxValue.as_object();
                    SlideItem item;
                    if (!wantString(jsonItem, "id", &item.id, error) ||
                        !claimId(item.id, "slide item")) {
                        return false;
                    }
                    // Kind, per format (task 6.17b). Format 3 names it on
                    // every item; format 2 has no such key — its stack is
                    // necessarily text — so a "kind" in a 2-file is a
                    // hand-bump, and loading one as a text item would guess
                    // at a picture someone else's build wrote as something
                    // else entirely.
                    if (unkindedList) {
                        if (jsonItem.contains("kind")) {
                            fail(error, "service.json: item '" + item.id +
                                            "' carries a 'kind' in a format-2 file");
                            return false;
                        }
                        item.kind = ItemKind::Text;
                    } else {
                        std::string kind;
                        if (!wantString(jsonItem, "kind", &kind, error)) {
                            return false;
                        }
                        if (kind == "text") {
                            item.kind = ItemKind::Text;
                        } else if (kind == "image") {
                            item.kind = ItemKind::Image;
                        } else {
                            fail(error, "service.json: item '" + item.id +
                                            "' has kind '" + kind +
                                            "', neither 'text' nor 'image'");
                            return false;
                        }
                    }
                    if (item.kind == ItemKind::Image) {
                        // The same discipline addItem enforces: kind says
                        // which content fields mean something, so words on
                        // an image, or an image naming no asset, are
                        // corruption naming itself — not a half-item.
                        if (jsonItem.contains("lines")) {
                            fail(error, "service.json: image item '" + item.id +
                                            "' carries 'lines'; words belong to a text item");
                            return false;
                        }
                        if (!wantString(jsonItem, "assetId", &item.assetId, error)) {
                            return false;
                        }
                        if (item.assetId.empty()) {
                            fail(error, "service.json: image item '" + item.id +
                                            "' names no asset");
                            return false;
                        }
                    } else {
                        if (jsonItem.contains("assetId")) {
                            fail(error, "service.json: text item '" + item.id +
                                            "' names an asset; only images do");
                            return false;
                        }
                        if (!wantStringArray(jsonItem, "lines", &item.lines, error)) {
                            return false;
                        }
                    }
                    if (!wantNumber(jsonItem, "x", &item.x, error) ||
                        !wantNumber(jsonItem, "y", &item.y, error) ||
                        !wantNumber(jsonItem, "width", &item.width, error) ||
                        !wantNumber(jsonItem, "height", &item.height, error)) {
                        return false;
                    }
                    slide.items.push_back(std::move(item));
                }
            }
            if (!wantOptionalString(jsonSlide, "group", &slide.group, error) ||
                !wantOptionalString(jsonSlide, "notes", &slide.notes, error) ||
                !wantOptionalString(jsonSlide, slideThemeKey, &slide.themeId, error) ||
                !wantOptionalString(jsonSlide, "backgroundAssetId",
                                    &slide.backgroundAssetId, error)) {
                return false;
            }
            if (!refuseMixedNaming(jsonSlide, themeNamed ? "styleId" : "themeId",
                                   "slide '" + slide.id + "'")) {
                return false;
            }
            // The customization set (task TH.2): a format-6 slide names
            // it — the writer always writes it, so its absence from a 6
            // file is a hand-edit, the backgroundGradient's rule — and a
            // pre-6 file cannot hold one at all. Inside, each field is
            // present exactly when the slide customized it; the slide's
            // answers then face the theme's own contract, so no file can
            // customize itself into an appearance no theme could carry.
            if (themeNamed) {
                const boost::json::value* changed = field(jsonSlide, "customizations");
                if (changed == nullptr) {
                    fail(error, "service.json: slide '" + slide.id + "' carries no "
                                    "\"customizations\"; this build's writer always "
                                    "writes it, so its absence is a hand-edit");
                    return false;
                }
                if (!changed->is_object()) {
                    fail(error, "service.json: slide '" + slide.id +
                                    "' customizations must be an object");
                    return false;
                }
                const boost::json::object& changedObject = changed->as_object();
                SlideCustomization& c = slide.customizations;
                if (!wantCustomString(changedObject, "textColor", &c.textColor, error) ||
                    !wantCustomString(changedObject, "backgroundColor", &c.backgroundColor,
                                      error) ||
                    !wantCustomString(changedObject, "backgroundGradient", &c.backgroundGradient,
                                      error) ||
                    !wantCustomString(changedObject, "fontFamily", &c.fontFamily, error) ||
                    !wantCustomNumber(changedObject, "fontSize", &c.fontSize, error) ||
                    !wantCustomString(changedObject, "align", &c.align, error) ||
                    !wantCustomNumber(changedObject, "lineSpacing", &c.lineSpacing, error) ||
                    !wantBoxRect(changedObject, "placement", "slide '" + slide.id + "'",
                                 &c.placement, error)) {
                    return false;
                }
                // The TS.1 style fields are format-8 meanings inside the
                // (format-6) customization object: a 6- or 7-file that
                // carries one came from a build whose "stroke" this loader
                // cannot interpret, and the whole package is refused naming
                // the key (the placement rule again, at one level deeper).
                if (format >= kServiceFormatTextStyle) {
                    if (!wantCustomInteger(changedObject, "fontWeight", &c.fontWeight,
                                           error) ||
                        !wantCustomJsonString(changedObject, "fontStyle", &c.fontStyle,
                                              error) ||
                        !wantCustomJsonString(changedObject, "fitMode", &c.fitMode, error) ||
                        !wantCustomNumber(changedObject, "minFontSize", &c.minFontSize,
                                          error) ||
                        !wantStroke(changedObject, "stroke", "slide '" + slide.id +
                                                                "' customization",
                                    &c.stroke, error) ||
                        !wantShadow(changedObject, "shadow", "slide '" + slide.id +
                                                                 "' customization",
                                    &c.shadow, error)) {
                        return false;
                    }
                } else {
                    for (const char* key : {"fontWeight", "fontStyle", "fitMode",
                                            "minFontSize", "stroke", "shadow"}) {
                        if (changedObject.contains(key)) {
                            fail(error, "service.json: slide '" + slide.id + "' carries "
                                            "customization '" + key + "' in a format " +
                                            std::to_string(format) + " file, where that "
                                            "field does not exist");
                            return false;
                        }
                    }
                }
                for (const auto& entry : changedObject) {
                    if (!isCustomizableField(std::string(entry.key()))) {
                        fail(error, "service.json: slide '" + slide.id + "' carries "
                                        "customization '" + std::string(entry.key()) +
                                        "', which is not a field");
                        return false;
                    }
                }
                std::string detail;
                if (!Documents::validCustomizations(c, &detail)) {
                    fail(error, "service.json: slide '" + slide.id + "' carries a "
                                    "customization the renderer cannot honour: " + detail);
                    return false;
                }
            } else if (jsonSlide.contains("customizations")) {
                fail(error, "service.json: slide '" + slide.id +
                                "' carries \"customizations\" in a format " +
                                std::to_string(format) + " file, where that key"
                                " does not exist");
                return false;
            }
            // The fit (task 6.17c1): a format-4-or-newer slide names it,
            // always one of the supported spellings — an old build's name
            // is refused, never approximated, because "what the newer
            // build drew" is a fact this renderer cannot know. A
            // format-1/2/3 slide has no key and cover is the ONLY thing
            // those files ever drew, so the default is not an inference
            // but their own history. A "fit" in a pre-4 file means
            // somebody else's build meant something by it (the format-2
            // "kind" rule, applied again). The test is against the format
            // that INTRODUCED the key, not kServiceFormat: a format-5 file
            // still carries the fit 4 added.
            if (format >= kServiceFormatLegacyFlat) {
                std::string fitName;
                if (!wantString(jsonSlide, "fit", &fitName, error)) {
                    return false;
                }
                if (!backgroundFitFromName(fitName, &slide.backgroundFit)) {
                    fail(error, "service.json: slide '" + slide.id + "' has fit '" +
                                    fitName + "', not a fit this build can draw");
                    return false;
                }
            } else {
                if (jsonSlide.contains("fit")) {
                    fail(error, "service.json: slide '" + slide.id +
                                    "' carries \"fit\" in a format " +
                                    std::to_string(format) + " file, where that key"
                                    " does not exist");
                    return false;
                }
                slide.backgroundFit = BackgroundFit::Cover;
            }
            // The named text context (task TF.1): format 7 knows the key,
            // earlier files do not — a "textContext" in one of those is
            // somebody else's meaning, the fit's and placement's era rule
            // applied again. Absent, or a name no theme carries, the slide
            // resolves with no context layer.
            if (format >= kServiceFormatContexts) {
                if (!wantOptionalString(jsonSlide, "textContext", &slide.textContext, error)) {
                    return false;
                }
            } else if (jsonSlide.contains("textContext")) {
                fail(error, "service.json: slide '" + slide.id +
                                "' carries \"textContext\" in a format " +
                                std::to_string(format) + " file, where that key"
                                " does not exist");
                return false;
            }
            for (const SlideItem& item : slide.items) {
                if (!std::isfinite(item.x) || !std::isfinite(item.y) ||
                    !std::isfinite(item.width) || !std::isfinite(item.height) ||
                    item.width <= 0.0 || item.height <= 0.0) {
                    fail(error, "service.json: slide '" + slide.id + "' holds item '" +
                                    item.id + "' that cannot be placed");
                    return false;
                }
            }
            show.slides.push_back(std::move(slide));
        }
        shows.push_back(std::move(show));
    }

    if (!refuseMixedNaming(root, themeNamed ? "styles" : "themes", "the package")) {
        return false;
    }
    const boost::json::array* themeArray = nullptr;
    if (!wantArray(root, themesKey, &themeArray, error)) {
        return false;
    }
    std::vector<Theme> themes;
    std::set<Id> themeIds;
    for (const boost::json::value& element : *themeArray) {
        if (!element.is_object()) {
            fail(error, "service.json: 'themes' holds a non-object");
            return false;
        }
        const boost::json::object& jsonTheme = element.as_object();
        Theme theme;
        if (!wantString(jsonTheme, "id", &theme.id, error) || !claimId(theme.id, "theme") ||
            !wantString(jsonTheme, "name", &theme.name, error) ||
            !wantString(jsonTheme, "textColor", &theme.textColor, error) ||
            !wantString(jsonTheme, "backgroundColor", &theme.backgroundColor, error) ||
            !wantOptionalString(jsonTheme, "fontFamily", &theme.fontFamily, error) ||
            !wantNumber(jsonTheme, "fontSize", &theme.fontSize, error) ||
            !wantOptionalString(jsonTheme, "align", &theme.align, error) ||
            !wantOptionalNumber(jsonTheme, "lineSpacing", &theme.lineSpacing, error)) {
            return false;
        }
        // The gradient colour (task 6.17c3): a format-5-or-newer theme names
        // it, and "" is a real answer — flat. Its absence in a pre-5 file is
        // not an inference but that file's own history: flat was the only
        // background those builds could draw. A "backgroundGradient" in a
        // pre-5 file means somebody else's build meant something by it
        // (the format-2 "kind" rule, applied again), and the lie is
        // refused naming the key. The string itself is not colour-checked
        // here, exactly like backgroundColor one line up: a bad colour is
        // the scene compile's to refuse, once, with the theme named.
        if (format >= kServiceFormatLegacyStyleNamed) {
            if (!wantString(jsonTheme, "backgroundGradient", &theme.backgroundGradient, error)) {
                return false;
            }
        } else {
            if (jsonTheme.contains("backgroundGradient")) {
                fail(error, "service.json: theme '" + theme.id +
                                "' carries \"backgroundGradient\" in a format " +
                                std::to_string(format) + " file, where that key"
                                " does not exist");
                return false;
            }
        }
        // Placement (amendment 2026-11-01) is a format-6 member and the
        // only OPTIONAL one: absent is exactly what every theme written
        // before it meant, so its absence is not a hand-edit and needs no
        // inference. A "placement" in a pre-6 file is another build's
        // meaning of a key this build's writer never emitted there, and is
        // refused naming it — the backgroundGradient rule, applied again.
        if (themeNamed) {
            if (!wantBoxRect(jsonTheme, "placement", "theme '" + theme.id + "'", &theme.placement,
                             error)) {
                return false;
            }
        } else if (jsonTheme.contains("placement")) {
            fail(error, "service.json: theme '" + theme.id +
                            "' carries \"placement\" in a format " +
                            std::to_string(format) + " file, where that key"
                            " does not exist");
            return false;
        }
        // "align" arrived after the first services: wantOptionalString
        // blanks an absent key, and blank is how a pre-typography theme
        // says "centred", the only drawing it ever knew.
        if (theme.align.empty()) {
            theme.align = "center";
        }
        // A loaded theme must be one the renderer can honour: the same
        // contract createTheme/updateTheme enforce.
        if (!std::isfinite(theme.fontSize) || theme.fontSize < 0.0 ||
            !openshow::domain::isThemeAlign(theme.align) ||
            !std::isfinite(theme.lineSpacing) || theme.lineSpacing < 0.25 ||
            theme.lineSpacing > 8.0) {
            fail(error, "service.json: theme '" + theme.id +
                            "' holds typography the renderer cannot honour");
            return false;
        }
        // Named text contexts (task TF.1) are a format-7 member: absent
        // is the plain theme, and a "contexts" key in an earlier file is
        // somebody else's meaning (the placement rule, applied again).
        // Inside an entry the vetting is STRICTER than elsewhere in the
        // package: an unknown field is refused naming itself, because the
        // one thing a context must never grow is a line count — the line
        // count is whatever the measurement fits, and a file that says
        // otherwise came from somewhere this build answers nowhere to.
        if (format >= kServiceFormatContexts) {
            const boost::json::value* contextsValue = field(jsonTheme, "contexts");
            if (contextsValue != nullptr) {
                if (!contextsValue->is_array()) {
                    fail(error, "service.json: 'contexts' must be an array");
                    return false;
                }
                std::set<std::string> seenNames;
                for (const boost::json::value& element : contextsValue->as_array()) {
                    if (!element.is_object()) {
                        fail(error, "service.json: theme '" + theme.id +
                                        "' holds a non-object context");
                        return false;
                    }
                    const boost::json::object& jsonContext = element.as_object();
                    TextContext context;
                    const boost::json::value* nameValue = field(jsonContext, "name");
                    if (nameValue == nullptr || !nameValue->is_string() ||
                        nameValue->as_string().empty()) {
                        fail(error, "service.json: a context of theme '" + theme.id +
                                        "' has no name");
                        return false;
                    }
                    context.name = std::string(nameValue->as_string());
                    if (!seenNames.insert(context.name).second) {
                        fail(error, "service.json: theme '" + theme.id +
                                        "' names context '" + context.name + "' twice");
                        return false;
                    }
                    // The transition override (task TR.1) is format 9's to
                    // carry. Before that era a context held typography and a
                    // box and nothing else, so the key is refused with the
                    // era named rather than read as some other build's field.
                    if (format < kServiceFormatTransition && jsonContext.contains("transition")) {
                        fail(error, "service.json: context '" + context.name +
                                        "' carries \"transition\" in a format " +
                                        std::to_string(format) + " file, where a context"
                                        " holds only typography and a box");
                        return false;
                    }
                    for (const auto& member : jsonContext) {
                        const std::string_view key = member.key();
                        if (key == "name" || key == "fontFamily" || key == "fontSize" ||
                            key == "lineSpacing" || key == "align" || key == "box" ||
                            key == "inset" || key == "transition") {
                            continue;
                        }
                        fail(error, "service.json: context '" + context.name +
                                        "' carries '" + std::string(key) +
                                        "', and a context holds only typography and a box"
                                        " — never a line count or a colour");
                        return false;
                    }
                    // Read the optionals into the struct through small
                    // locals: a present value is exactly an override.
                    const boost::json::value* fontFamily = field(jsonContext, "fontFamily");
                    if (fontFamily != nullptr) {
                        if (!fontFamily->is_string()) {
                            fail(error, "service.json: context '" + context.name +
                                            "' fontFamily must be a string");
                            return false;
                        }
                        context.fontFamily = std::string(fontFamily->as_string());
                    }
                    const boost::json::value* fontSize = field(jsonContext, "fontSize");
                    if (fontSize != nullptr) {
                        if (!fontSize->is_number()) {
                            fail(error, "service.json: context '" + context.name +
                                            "' fontSize must be a number");
                            return false;
                        }
                        context.fontSize = fontSize->to_number<double>();
                    }
                    const boost::json::value* lineSpacing = field(jsonContext, "lineSpacing");
                    if (lineSpacing != nullptr) {
                        if (!lineSpacing->is_number()) {
                            fail(error, "service.json: context '" + context.name +
                                            "' lineSpacing must be a number");
                            return false;
                        }
                        context.lineSpacing = lineSpacing->to_number<double>();
                    }
                    const boost::json::value* align = field(jsonContext, "align");
                    if (align != nullptr) {
                        if (!align->is_string()) {
                            fail(error, "service.json: context '" + context.name +
                                            "' align must be a string");
                            return false;
                        }
                        context.align = std::string(align->as_string());
                    }
                    if (!wantBoxRect(jsonContext, "box", "context '" + context.name + "'",
                                     &context.box, error)) {
                        return false;
                    }
                    const boost::json::value* inset = field(jsonContext, "inset");
                    if (inset != nullptr) {
                        if (!inset->is_number()) {
                            fail(error, "service.json: context '" + context.name +
                                            "' inset must be a number");
                            return false;
                        }
                        context.inset = inset->to_number<double>();
                    }
                    if (context.box.has_value() && context.inset.has_value()) {
                        fail(error, "service.json: context '" + context.name +
                                        "' gives both a box and an inset; the inset would"
                                        " shrink nothing the box did not already say");
                        return false;
                    }
                    if (context.fontFamily.has_value() && context.fontFamily->size() > 256) {
                        fail(error, "service.json: context '" + context.name +
                                        "' names an implausibly long font family");
                        return false;
                    }
                    if (context.fontSize.has_value() && (!std::isfinite(*context.fontSize) ||
                                                         *context.fontSize < 0.0)) {
                        fail(error, "service.json: context '" + context.name +
                                        "' carries a font size the renderer cannot honour");
                        return false;
                    }
                    if (context.lineSpacing.has_value() && (!std::isfinite(*context.lineSpacing) ||
                                                            *context.lineSpacing < 0.25 ||
                                                            *context.lineSpacing > 8.0)) {
                        fail(error, "service.json: context '" + context.name +
                                        "' carries a line spacing the renderer cannot honour");
                        return false;
                    }
                    if (context.align.has_value() && !isThemeAlign(*context.align)) {
                        fail(error, "service.json: context '" + context.name +
                                        "' has align '" + *context.align +
                                        "', not an alignment this build can draw");
                        return false;
                    }
                    if (context.inset.has_value() && (!std::isfinite(*context.inset) ||
                                                      *context.inset < 0.0)) {
                        fail(error, "service.json: context '" + context.name +
                                        "' carries an inset that shrinks nothing sane");
                        return false;
                    }
                    // The context's own movement (task TR.1): the era was
                    // settled with the member set above, so the shape and the
                    // band belong to the shared record reader.
                    if (!wantTransition(jsonContext, "transition",
                                        "context '" + context.name + "'", &context.transition,
                                        error)) {
                        return false;
                    }
                    theme.contexts.push_back(std::move(context));
                }
            }
        } else if (jsonTheme.contains("contexts")) {
            fail(error, "service.json: theme '" + theme.id +
                            "' carries \"contexts\" in a format " +
                            std::to_string(format) + " file, where that key"
                            " does not exist");
            return false;
        }
        // The TS.1 style fields (task TS.1) are format-8 members and share
        // placement's discipline of silence: absent is "this theme has no
        // style opinion", which resolves to the concrete defaults the
        // renderer always applied — old files keep their exact pixels.
        // A style key in a pre-8 file is somebody else's build meaning
        // something by it (the backgroundGradient rule, applied to a set).
        if (format >= kServiceFormatTextStyle) {
            if (!wantOptionalInteger(jsonTheme, "fontWeight", &theme.fontWeight, error) ||
                !wantOptionalJsonString(jsonTheme, "fontStyle", &theme.fontStyle, error) ||
                !wantOptionalJsonString(jsonTheme, "fitMode", &theme.fitMode, error) ||
                !wantOptionalNumberField(jsonTheme, "minFontSize", &theme.minFontSize,
                                         error) ||
                !wantStroke(jsonTheme, "stroke", "theme '" + theme.id + "'", &theme.stroke,
                            error) ||
                !wantShadow(jsonTheme, "shadow", "theme '" + theme.id + "'", &theme.shadow,
                            error)) {
                return false;
            }
            // A loaded theme must be one this build could have written:
            // the shape was the reader's, the MEANING is the domain's one
            // sentence — the same validThemeTextStyle setThemeTextStyle
            // quotes, so a file cannot store what a verb would refuse.
            ThemeTextStyle style;
            style.fontWeight = theme.fontWeight;
            style.fontStyle = theme.fontStyle;
            style.fitMode = theme.fitMode;
            style.minFontSize = theme.minFontSize;
            style.stroke = theme.stroke;
            style.shadow = theme.shadow;
            std::string detail;
            if (!Documents::validThemeTextStyle(style, &detail)) {
                fail(error, "service.json: theme '" + theme.id +
                                "' carries text style the renderer cannot honour: " + detail);
                return false;
            }
        } else {
            for (const char* key : {"fontWeight", "fontStyle", "fitMode", "minFontSize",
                                    "stroke", "shadow"}) {
                if (jsonTheme.contains(key)) {
                    fail(error, "service.json: theme '" + theme.id + "' carries \"" + key +
                                    "\" in a format " + std::to_string(format) +
                                    " file, where that key does not exist");
                    return false;
                }
            }
        }
        // The theme's transition (task TR.1) is a format-9 member with the
        // same silence: absent means this view says nothing about how it
        // moves, which is exactly what every theme before it meant. The
        // record's own validity was checked by the shared sentence inside
        // wantTransition, so a theme that loads is a theme a verb would have
        // set.
        if (format >= kServiceFormatTransition) {
            if (!wantTransition(jsonTheme, "transition", "theme '" + theme.id + "'",
                                &theme.transition, error)) {
                return false;
            }
        } else if (jsonTheme.contains("transition")) {
            fail(error, "service.json: theme '" + theme.id +
                            "' carries \"transition\" in a format " +
                            std::to_string(format) + " file, where that key"
                            " does not exist");
            return false;
        }
        themeIds.insert(theme.id);
        themes.push_back(std::move(theme));
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
        if (!show.defaultThemeId.empty() && themeIds.count(show.defaultThemeId) == 0) {
            fail(error, "service.json: show '" + show.id + "' defaults to unknown theme '" +
                            show.defaultThemeId + "'");
            return false;
        }
        for (const Slide& slide : show.slides) {
            if (!slide.themeId.empty() && themeIds.count(slide.themeId) == 0) {
                fail(error, "service.json: slide '" + slide.id + "' names unknown theme '" +
                                slide.themeId + "'");
                return false;
            }
            if (!slide.backgroundAssetId.empty() &&
                assetIds.count(slide.backgroundAssetId) == 0) {
                fail(error, "service.json: slide '" + slide.id +
                                "' names unknown asset '" + slide.backgroundAssetId + "'");
                return false;
            }
            // A foreground picture's asset goes the same way (task 6.17b):
            // an item that names a missing asset can never draw, and a load
            // that kept it would hand the compiler a slide it must refuse.
            for (const SlideItem& item : slide.items) {
                if (item.kind == ItemKind::Image && assetIds.count(item.assetId) == 0) {
                    fail(error, "service.json: slide '" + slide.id + "' image item '" +
                                    item.id + "' names unknown asset '" + item.assetId + "'");
                    return false;
                }
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

    // The service's one current theme (task TH.2) must be one the service
    // actually has: an unknown name is the same lie as a show defaulting
    // to one, only stated at the top of the file.
    if (!currentThemeId.empty() && themeIds.count(currentThemeId) == 0) {
        fail(error, "service.json: the current theme '" + currentThemeId + "' is unknown");
        return false;
    }

    out->fShows = std::move(shows);
    out->fThemes = std::move(themes);
    out->fCurrentThemeId = std::move(currentThemeId);
    out->fTransition = transition;
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
    if (!ServiceCodec::load(documents, directory, error)) {
        return false;
    }
    // A service written before the Default theme existed — or hand-edited
    // into its absence — opens on the embedded one (amendment 2026-11-02).
    // After decoding, so the loader's wholesale rebuild cannot drop it.
    documents->ensureDefaultTheme();
    return true;
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
