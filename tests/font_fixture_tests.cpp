/*
 * Task TS.5: the pinned font corpus, rendered for real.
 *
 * TS.1–TS.4 shipped a typography contract, a styled renderer and styled
 * measurement, and their raster fixtures measured whatever faces the machine
 * happened to have. That is fine for "does a stroke grow the ink" and not
 * nearly good enough for "is this the typography we say it is": a fixture that
 * measures fontconfig's mood today cannot tell a weight ramp from a fallback,
 * and the review's claims about Inter, Lora and Oswald cannot be re-checked by
 * anyone once /tmp is gone.
 *
 * So this suite renders the THREE families the theme-editor review pinned, at
 * the revision and SHA-256 it recorded, plus a fourth pinned face to fall back
 * through, inside a fontconfig configuration that can see NOTHING ELSE. Family
 * resolution is asserted before any pixel is measured, because every pixel
 * claim below is a claim about a specific face:
 *
 *   - the corpus is tests/fixtures/fonts/manifest.json, and every file's bytes
 *     are hashed here before they are trusted;
 *   - ctest never downloads. tools/setup-font-fixtures.sh fills the cache, by
 *     hand, from the network or from a directory (--from). An unprepared
 *     corpus is a VISIBLE SKIP (exit 77, which ctest reports as "Skipped")
 *     naming the command to run; a corpus whose bytes disagree with the
 *     manifest is a FAILURE, because a face that quietly changed would make
 *     every number here a lie about a different font;
 *   - the run gets a private FONTCONFIG_FILE pointing only at the corpus
 *     directory plus a private fontconfig cache, so the machine's own fonts
 *     can neither help nor be touched. Nothing is installed.
 *
 * argv[1] is the manifest and argv[2] the review document that pinned it: the
 * manifest is a copy of that decision, so the two are compared — revision and
 * every TTF hash — before any skip decision, since a drifted manifest is wrong
 * whether or not this machine holds the bytes.
 *
 * What gets measured: that the four families are four different faces; that
 * the numeric weight ramp resolves a different face without re-wrapping;
 * extreme legal stroke and shadow (negative offsets included) against the
 * TS.3 ink bounds; fixed against shrink; the deliberate missing family and
 * where it lands; per-family Unicode coverage, claimed only where the face
 * really has the glyph; a midpoint fade on every effect at once; and the
 * setup and shaping costs and the cache bounds, printed as the record.
 *
 * Tolerances, not goldens: CPU Skia is deterministic on one machine, but
 * antialiasing and fontconfig ordering are not portable promises. Geometry and
 * colour-class assertions carry explicit pixel tolerances, and the only
 * byte-equality assertions are between two renders made in the same run on the
 * same engine.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <boost/json.hpp>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkRect.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"

#include "openshow/scene/scene.h"

#include "render/SceneText.h"

namespace scene = openshow::scene;
namespace worker = openshow::worker;
namespace fs = std::filesystem;

namespace {

int failures = 0;

constexpr int kWidth = 960;  // authored 1920x1080 at the 0.5 snapshot scale
constexpr int kHeight = 540;
constexpr double kScale = 0.5;
constexpr int kSlack = 2;  // device px of antialiasing generosity

// The three colours the effect fixtures separate ink BY, so a stroke band, a
// shadow copy and a fill can each be counted in one frame without guessing
// which pass a pixel came from.
constexpr std::uint32_t kFill = 0xFFFFFFu;
constexpr const char* kStrokeColour = "#ff0000";
constexpr const char* kShadowColour = "#00ffff";
constexpr std::uint32_t kStrokeRgb = 0xFF0000u;
constexpr std::uint32_t kShadowRgb = 0x00FFFFu;
constexpr std::uint32_t kBackdrop = 0x102030u;

bool check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
    return condition;
}

/* --- sha256 ---------------------------------------------------------------
 * The corpus check has to hash the bytes it is about to render, and this suite
 * is Qt-free, so QCryptographicHash is not on the menu. It is written here
 * rather than shelling out to sha256sum because a missing coreutils binary
 * must not turn a real font problem into a confusing pass. Its own correctness
 * is the first fixture: a broken hash would skip or fail for the wrong reason.
 */

constexpr std::uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

std::uint32_t rotateRight(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t length) {
        fBits += static_cast<std::uint64_t>(length) * 8u;
        while (length > 0) {
            const std::size_t take = std::min(length, 64u - fCount);
            std::copy_n(data, take, fBuffer + fCount);
            fCount += take;
            data += take;
            length -= take;
            if (fCount == 64u) {
                transform(fBuffer);
                fCount = 0;
            }
        }
    }

    std::string hex() {
        // The padding is length-dependent, so the true length is stashed
        // before the pad bytes themselves are counted.
        const std::uint64_t bits = fBits;
        const std::uint8_t pad = 0x80u;
        raw(&pad, 1);
        const std::uint8_t zero = 0u;
        while (fCount != 56u) {
            raw(&zero, 1);
        }
        std::uint8_t tail[8];
        for (int i = 0; i < 8; ++i) {
            tail[i] = static_cast<std::uint8_t>((bits >> (56 - 8 * i)) & 0xFFu);
        }
        raw(tail, 8);

        char out[65];
        for (int word = 0; word < 8; ++word) {
            for (int byte = 0; byte < 4; ++byte) {
                std::snprintf(out + word * 8 + byte * 2, 3, "%02x",
                              (fH[word] >> (24 - 8 * byte)) & 0xFFu);
            }
        }
        out[64] = '\0';
        return std::string(out);
    }

private:
    // update() but without touching the message length: padding only.
    void raw(const std::uint8_t* data, std::size_t length) {
        while (length > 0) {
            const std::size_t take = std::min(length, 64u - fCount);
            std::copy_n(data, take, fBuffer + fCount);
            fCount += take;
            data += take;
            length -= take;
            if (fCount == 64u) {
                transform(fBuffer);
                fCount = 0;
            }
        }
    }

    void transform(const std::uint8_t* block) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotateRight(w[i - 15], 7) ^ rotateRight(w[i - 15], 18) ^
                                     (w[i - 15] >> 3);
            const std::uint32_t s1 =
                rotateRight(w[i - 2], 17) ^ rotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = fH[0], b = fH[1], c = fH[2], d = fH[3];
        std::uint32_t e = fH[4], f = fH[5], g = fH[6], h = fH[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + S1 + ch + kRound[i] + w[i];
            const std::uint32_t S0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        fH[0] += a;
        fH[1] += b;
        fH[2] += c;
        fH[3] += d;
        fH[4] += e;
        fH[5] += f;
        fH[6] += g;
        fH[7] += h;
    }

    std::uint32_t fH[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::uint64_t fBits = 0;
    std::uint8_t fBuffer[64] = {};
    std::size_t fCount = 0;
};

bool sha256File(const fs::path& path, std::string* hex, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot open " + path.string();
        return false;
    }
    Sha256 digest;
    std::vector<char> chunk(64u * 1024u);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            digest.update(reinterpret_cast<const std::uint8_t*>(chunk.data()),
                          static_cast<std::size_t>(got));
        }
    }
    *hex = digest.hex();
    return true;
}

void fixtureHashHelperIsCorrect() {
    // Published vectors. If this fails, every corpus decision the suite makes
    // means nothing, so it runs before the corpus is even consulted.
    Sha256 abc;
    abc.update(reinterpret_cast<const std::uint8_t*>("abc"), 3);
    check(abc.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 of \"abc\" matches the published vector");
    Sha256 empty;
    empty.update(reinterpret_cast<const std::uint8_t*>(""), 0);
    check(empty.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 of the empty input matches the published vector");
    // 56 bytes is where the padding has to spill into a second block: the
    // classic place a hand-rolled implementation goes quietly wrong.
    Sha256 spill;
    const std::string text(56, 'a');
    spill.update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    check(spill.hex() == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
          "sha256 across the padding spill matches");
}

/* --- the manifest --------------------------------------------------------- */

struct PinnedFont {
    std::string role;
    std::string family;
    std::string file;
    std::string upstreamPath;  // below the repository root, e.g. ofl/inter/...
    std::string sha256;
    std::uint64_t bytes = 0;
    std::string licenceFile;
    std::string licenceSha256;
    std::uint64_t licenceBytes = 0;
};

struct Corpus {
    std::string revision;
    std::vector<PinnedFont> fonts;
    fs::path directory;  // where the prepared files live
};

bool isHex(const std::string& text, std::size_t length) {
    if (text.size() != length) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string stringField(const boost::json::object& object, const char* key) {
    const auto entry = object.find(key);
    if (entry == object.end() || !entry->value().is_string()) {
        return std::string();
    }
    return std::string(entry->value().as_string());
}

std::uint64_t numberField(const boost::json::object& object, const char* key) {
    const auto entry = object.find(key);
    if (entry == object.end() || !entry->value().is_int64()) {
        return 0;
    }
    return static_cast<std::uint64_t>(entry->value().as_int64());
}

// Reads the manifest and refuses a malformed one. The shape checks are not
// ceremony: this file is the repository's only statement of WHAT the pinned
// corpus is, so a truncated hash or a duplicated role has to stop the suite
// rather than quietly test less than it claims.
bool loadManifest(const fs::path& path, Corpus* corpus, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot read the manifest at " + path.string();
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    boost::json::value parsed;
    try {
        parsed = boost::json::parse(text);
    } catch (const std::exception& e) {
        *error = "manifest is not JSON: " + std::string(e.what());
        return false;
    }
    if (!parsed.is_object()) {
        *error = "manifest is not a JSON object";
        return false;
    }
    const boost::json::object& root = parsed.as_object();
    const auto upstream = root.find("upstream");
    if (upstream == root.end() || !upstream->value().is_object()) {
        *error = "manifest has no 'upstream' object";
        return false;
    }
    corpus->revision = stringField(upstream->value().as_object(), "revision");
    if (!isHex(corpus->revision, 40)) {
        *error = "manifest 'upstream.revision' is not a 40-character commit hash";
        return false;
    }
    const auto fonts = root.find("fonts");
    if (fonts == root.end() || !fonts->value().is_array() || fonts->value().as_array().empty()) {
        *error = "manifest pins no fonts";
        return false;
    }
    int fallbacks = 0;
    for (const boost::json::value& entry : fonts->value().as_array()) {
        if (!entry.is_object()) {
            *error = "a manifest font entry is not an object";
            return false;
        }
        const boost::json::object& row = entry.as_object();
        PinnedFont font;
        font.role = stringField(row, "role");
        font.family = stringField(row, "family");
        font.file = stringField(row, "file");
        font.upstreamPath = stringField(row, "upstreamPath");
        font.sha256 = stringField(row, "sha256");
        font.bytes = numberField(row, "bytes");
        font.licenceFile = stringField(row, "licenceFile");
        font.licenceSha256 = stringField(row, "licenceSha256");
        font.licenceBytes = numberField(row, "licenceBytes");
        if (font.role.empty() || font.family.empty() || font.file.empty()) {
            *error = "a manifest font entry is missing its role, family or file";
            return false;
        }
        if (!isHex(font.sha256, 64) || !isHex(font.licenceSha256, 64)) {
            *error = "font " + font.family + " has a sha256 that is not 64 lowercase hex digits";
            return false;
        }
        if (font.bytes == 0 || font.licenceBytes == 0 || font.licenceFile.empty()) {
            *error = "font " + font.family + " has no pinned size or no pinned licence";
            return false;
        }
        for (const PinnedFont& seen : corpus->fonts) {
            if (seen.family == font.family || seen.file == font.file) {
                *error = "font " + font.family + " is pinned twice";
                return false;
            }
        }
        if (font.role == "fallback") {
            ++fallbacks;
        }
        corpus->fonts.push_back(font);
    }
    if (fallbacks != 1) {
        *error = "the corpus needs exactly one 'fallback' role, found " +
                 std::to_string(fallbacks);
        return false;
    }
    return true;
}

// The same resolution tools/setup-font-fixtures.sh uses, so a developer
// prepares a corpus in one place and the test finds it there.
fs::path corpusDirectory() {
    if (const char* env = std::getenv("OPENSHOW_FONT_FIXTURE_DIR"); env != nullptr && *env) {
        return fs::path(env);
    }
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg) {
        return fs::path(xdg) / "openshow" / "font-fixtures";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home) {
        return fs::path(home) / ".cache" / "openshow" / "font-fixtures";
    }
    return fs::temp_directory_path() / "openshow-font-fixtures";
}

enum class Prepared { Ready, Absent, Corrupt };

// Hashes every pinned file. Absent files are a skip; files that exist and
// disagree with the manifest are a failure. The distinction matters: "you have
// not prepared the corpus" and "the corpus you prepared is not the corpus we
// pinned" are the difference between an honest skip and a measurement of a
// different face.
Prepared prepareCorpus(const Corpus& corpus, std::vector<std::string>* problems) {
    bool absent = false;
    bool corrupt = false;
    for (const PinnedFont& font : corpus.fonts) {
        struct Item {
            std::string name;
            std::string sha;
            std::uint64_t bytes;
        };
        const Item items[] = {Item{font.file, font.sha256, font.bytes},
                              Item{font.licenceFile, font.licenceSha256, font.licenceBytes}};
        for (const Item& item : items) {
            const fs::path path = corpus.directory / item.name;
            std::error_code ec;
            if (!fs::exists(path, ec)) {
                problems->push_back("absent: " + path.string());
                absent = true;
                continue;
            }
            std::string hex;
            std::string error;
            if (!sha256File(path, &hex, &error)) {
                problems->push_back("unreadable: " + path.string() + " (" + error + ")");
                corrupt = true;
                continue;
            }
            if (hex != item.sha) {
                problems->push_back("BAD BYTES: " + path.string() + "\n           found " +
                                    hex + "\n           pinned " + item.sha);
                corrupt = true;
                continue;
            }
            const std::uintmax_t size = fs::file_size(path, ec);
            if (ec || size != item.bytes) {
                problems->push_back("BAD SIZE: " + path.string() + " is " +
                                    std::to_string(static_cast<unsigned long long>(size)) +
                                    " bytes, pinned " + std::to_string(item.bytes));
                corrupt = true;
            }
        }
    }
    if (corrupt) {
        return Prepared::Corrupt;
    }
    return absent ? Prepared::Absent : Prepared::Ready;
}

// The manifest is a copy of a decision made in the review, and a copy that
// drifts is the quiet kind of wrong: the fixtures would still pass, over faces
// nobody chose. So the review document is argv[2], and the agreement between
// the two pins is asserted here rather than kept by whoever last edited them.
void fixtureManifestAgreesWithTheReview(const fs::path& reviewPath, const Corpus& corpus) {
    std::ifstream in(reviewPath, std::ios::binary);
    if (!check(static_cast<bool>(in),
               "the review that pinned this corpus is readable at " + reviewPath.string())) {
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // The revision: a 40-character hex string in backticks near the words
    // "pinned to". Scanned rather than regex-matched because the prose is
    // allowed to wrap.
    std::string reviewRevision;
    {
        const size_t anchorAt = text.find("pinned to");
        for (size_t at = anchorAt == std::string::npos ? 0 : anchorAt; at + 41 < text.size();
             ++at) {
            if (text[at] == '`' && isHex(text.substr(at + 1, 40), 40)) {
                reviewRevision = text.substr(at + 1, 40);
                break;
            }
        }
    }
    check(!reviewRevision.empty(), "the review names a pinned revision");
    check(reviewRevision == corpus.revision,
          "the manifest pins the revision the review pinned (" + reviewRevision +
              " vs " + corpus.revision + ")");

    // The hash table: rows of the shape
    //     | `inter/Inter[opsz,wght].ttf` | purpose | `2916...` |
    // (the table's paths sit below `ofl/`, which the manifest spells out).
    int matched = 0;
    int reviewRows = 0;
    size_t at = 0;
    while ((at = text.find("`", at)) != std::string::npos) {
        const size_t end = text.find("`", at + 1);
        if (end == std::string::npos) {
            break;
        }
        const std::string path = text.substr(at + 1, end - at - 1);
        at = end + 1;
        if (path.size() <= 4 || path.compare(path.size() - 4, 4, ".ttf") != 0) {
            continue;
        }
        // The row's third cell: the next backticked 64-hex on the same line.
        // rest[0] is the backtick that CLOSED the path, so the search starts
        // past it - otherwise "the cells between two ticks" is the answer and
        // no hash is ever hex.
        const size_t lineEnd = text.find('\n', end);
        const std::string rest = text.substr(end, lineEnd - end);
        const size_t tick = rest.find('`', 1);
        if (tick == std::string::npos) {
            continue;
        }
        const size_t hashEnd = rest.find('`', tick + 1);
        if (hashEnd == std::string::npos) {
            continue;
        }
        const std::string hash = rest.substr(tick + 1, hashEnd - tick - 1);
        if (!isHex(hash, 64)) {
            continue;
        }
        ++reviewRows;
        std::string foundSha;
        for (const PinnedFont& font : corpus.fonts) {
            const std::string suffix = font.upstreamPath;
            if (suffix.size() >= path.size() &&
                suffix.compare(suffix.size() - path.size(), path.size(), path) == 0) {
                foundSha = font.sha256;
            }
        }
        if (foundSha.empty()) {
            check(false, "the review pins " + path + " and the manifest does not carry it");
            continue;
        }
        if (foundSha == hash) {
            ++matched;
        }
        // Full hashes in the message: a one-character drift near the end is
        // exactly the mistake a prefix would hide.
        check(foundSha == hash,
              "the manifest's bytes for " + path + " are the bytes the review hashed (review " +
                  hash + " vs manifest " + foundSha + ")");
    }
    check(reviewRows >= 3, "the review's font table was found (" + std::to_string(reviewRows) +
                               " rows)");
    check(matched == reviewRows, "every family the review pinned is pinned identically here (" +
                                     std::to_string(matched) + " of " +
                                     std::to_string(reviewRows) + ")");
}

/* --- frames --------------------------------------------------------------- */

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<SkColor> pixels;

    SkColor at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

bool loadPng(const fs::path& path, Frame* frame, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot read " + path.string();
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    sk_sp<SkData> data = SkData::MakeWithCopy(bytes.data(), bytes.size());
    auto codec = SkCodec::MakeFromData(std::move(data));
    if (!codec) {
        *error = "cannot decode " + path.string();
        return false;
    }
    const SkImageInfo info =
        codec->getInfo().makeColorType(kN32_SkColorType).makeAlphaType(kPremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info)) {
        *error = "cannot allocate " + path.string();
        return false;
    }
    if (codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes()) != SkCodec::kSuccess) {
        *error = "cannot decode the pixels of " + path.string();
        return false;
    }
    frame->width = info.width();
    frame->height = info.height();
    frame->pixels.resize(static_cast<size_t>(frame->width) * frame->height);
    for (int y = 0; y < frame->height; ++y) {
        for (int x = 0; x < frame->width; ++x) {
            frame->pixels[static_cast<size_t>(y) * frame->width + x] = bitmap.getColor(x, y);
        }
    }
    return true;
}

fs::path scratchDirectory;
int renderCounter = 0;

// Renders through the production CPU snapshot path and returns the decoded
// pixels. The PNGs stay on disk, named in the failure text, so a disagreeable
// number can be looked at rather than argued with.
bool renderFrame(const scene::Scene& subject, const std::string& name, Frame* frame,
                 std::string* error, const worker::SnapshotOptions& options = {}) {
    const fs::path path = scratchDirectory /
                          ("pinned-" + std::to_string(++renderCounter) + "-" + name + ".png");
    if (!worker::renderSceneSnapshot(subject, path.string(), kWidth, kHeight, options, error)) {
        return false;
    }
    return loadPng(path, frame, error);
}

// How far a pixel is from the background, summed over the channels: the
// "how much ink is here" number the fade fixtures measure.
int channelSumAbove(SkColor color, SkColor background) {
    return std::abs(static_cast<int>(SkColorGetR(color)) - static_cast<int>(SkColorGetR(background))) +
           std::abs(static_cast<int>(SkColorGetG(color)) - static_cast<int>(SkColorGetG(background))) +
           std::abs(static_cast<int>(SkColorGetB(color)) - static_cast<int>(SkColorGetB(background)));
}

bool differs(SkColor a, SkColor b, int tolerance = 16) {
    const int dr = std::abs(static_cast<int>(SkColorGetR(a)) - static_cast<int>(SkColorGetR(b)));
    const int dg = std::abs(static_cast<int>(SkColorGetG(a)) - static_cast<int>(SkColorGetG(b)));
    const int db = std::abs(static_cast<int>(SkColorGetB(a)) - static_cast<int>(SkColorGetB(b)));
    return dr > tolerance || dg > tolerance || db > tolerance;
}

struct Ink {
    int left = 0, top = 0, right = 0, bottom = 0;  // inclusive
    int count = 0;
};

Ink measureInk(const Frame& frame) {
    const SkColor background = frame.at(2, 2);
    Ink ink;
    ink.left = frame.width;
    ink.top = frame.height;
    ink.right = -1;
    ink.bottom = -1;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (differs(frame.at(x, y), background)) {
                ++ink.count;
                ink.left = std::min(ink.left, x);
                ink.top = std::min(ink.top, y);
                ink.right = std::max(ink.right, x);
                ink.bottom = std::max(ink.bottom, y);
            }
        }
    }
    return ink;
}

// Pixels within `tolerance` of one authored colour: how the three passes of a
// styled item are counted apart inside a single frame.
struct Class {
    int count = 0;
    int peak = 0;  // brightest channel-sum, for the fade ratio
};

Class measureClass(const Frame& frame, std::uint32_t rgb, int tolerance = 24) {
    Class out;
    const int r = static_cast<int>((rgb >> 16) & 0xFF);
    const int g = static_cast<int>((rgb >> 8) & 0xFF);
    const int b = static_cast<int>(rgb & 0xFF);
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const SkColor color = frame.at(x, y);
            if (std::abs(static_cast<int>(SkColorGetR(color)) - r) <= tolerance &&
                std::abs(static_cast<int>(SkColorGetG(color)) - g) <= tolerance &&
                std::abs(static_cast<int>(SkColorGetB(color)) - b) <= tolerance) {
                ++out.count;
                out.peak = std::max(out.peak, static_cast<int>(SkColorGetR(color)) +
                                                  static_cast<int>(SkColorGetG(color)) +
                                                  static_cast<int>(SkColorGetB(color)));
            }
        }
    }
    return out;
}

// The frame's brightest channel-sum above the background: the reference a
// fade-relative measurement needs, because the whole point of a midpoint is
// that every authored colour arrives at half strength.
int peakInkSum(const Frame& frame) {
    const SkColor background = frame.at(2, 2);
    int peak = 0;
    for (const SkColor color : frame.pixels) {
        peak = std::max(peak, channelSumAbove(color, background));
    }
    return peak;
}

// Ink at a fraction of the frame's own peak: a settled frame and its faded
// twin then have the same INK, even though one is half as bright, so bounding
// boxes are comparable. An absolute threshold would instead report the words
// as shrunk, because their antialiasing edges have simply dimmed.
Ink measureInkRelative(const Frame& frame, int divisor) {
    const SkColor background = frame.at(2, 2);
    const int threshold = std::max(8, peakInkSum(frame) / divisor);
    Ink ink;
    ink.left = frame.width;
    ink.top = frame.height;
    ink.right = -1;
    ink.bottom = -1;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            if (channelSumAbove(frame.at(x, y), background) >= threshold) {
                ++ink.count;
                ink.left = std::min(ink.left, x);
                ink.top = std::min(ink.top, y);
                ink.right = std::max(ink.right, x);
                ink.bottom = std::max(ink.bottom, y);
            }
        }
    }
    return ink;
}

// Which pass a pixel belongs to, by HUE rather than by distance to the
// authored colour: a shadow copy arrives blended with the backdrop and a
// blur, so "near #00FFFF" describes none of its pixels while "cyan-dominant"
// describes all of them.
enum class Hue { Fill, Stroke, Shadow };

bool hueOf(SkColor color, Hue* hue) {
    const int r = static_cast<int>(SkColorGetR(color));
    const int g = static_cast<int>(SkColorGetG(color));
    const int b = static_cast<int>(SkColorGetB(color));
    if (std::min(g, b) - r > 40) {
        *hue = Hue::Shadow;  // cyan-dominant
        return true;
    }
    if (r - std::max(g, b) > 40) {
        *hue = Hue::Stroke;  // red-dominant
        return true;
    }
    if (std::min({r, g, b}) > 120 && std::max({r, g, b}) - std::min({r, g, b}) <= 40) {
        *hue = Hue::Fill;  // neutral and bright
        return true;
    }
    return false;
}

bool sameFrame(const Frame& a, const Frame& b) {
    return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}

int differingPixels(const Frame& a, const Frame& b) {
    int count = 0;
    for (size_t i = 0; i < std::min(a.pixels.size(), b.pixels.size()); ++i) {
        if (a.pixels[i] != b.pixels[i]) {
            ++count;
        }
    }
    return count;
}

// Pixel-weighted centre Y of each consecutive band of inked rows: one entry
// per drawn line. Comparing the list between two renders says whether a word
// moved or a line appeared.
std::vector<double> bandCenterYs(const Frame& frame) {
    const SkColor background = frame.at(2, 2);
    std::vector<double> centers;
    double sum = 0.0;
    long long count = 0;
    bool inside = false;
    for (int y = 0; y < frame.height; ++y) {
        long long rowInk = 0;
        for (int x = 0; x < frame.width; ++x) {
            if (differs(frame.at(x, y), background)) {
                ++rowInk;
            }
        }
        if (rowInk > 0) {
            inside = true;
            sum += static_cast<double>(y) * static_cast<double>(rowInk);
            count += rowInk;
        } else if (inside) {
            centers.push_back(sum / static_cast<double>(count));
            sum = 0.0;
            count = 0;
            inside = false;
        }
    }
    if (count > 0) {
        centers.push_back(sum / static_cast<double>(count));
    }
    return centers;
}

/* --- scenes --------------------------------------------------------------- */

// One item, one family, the fields a fixture cares about. Everything else
// stays at the contract's concrete defaults, which is what TS.1 pins. The
// effect records carry their colours as the domain spells them (hex strings);
// the item's own fill is the packed colour the scene carries.
scene::Scene styledScene(const std::string& family, const std::vector<std::string>& lines,
                         double fontSize, int weight = 400, const std::string& fitMode = "shrink",
                         bool strokeEnabled = false, double strokeWidth = 2.0,
                         bool shadowEnabled = false, double offsetX = 0.0, double offsetY = 0.0,
                         double blurSigma = 0.0, double boxWidth = 0.0, double boxHeight = 0.0) {
    scene::Scene subject;
    subject.showId = "corpus-show";
    subject.slideId = "corpus-slide";
    subject.background.color = kBackdrop;
    scene::SceneItem item;
    item.lines = lines;
    item.fontFamily = family;
    item.fontSize = fontSize;
    item.color = kFill;
    item.fontWeight = weight;
    item.fitMode = fitMode;
    if (boxWidth > 0.0) {
        item.width = boxWidth;
    }
    if (boxHeight > 0.0) {
        item.height = boxHeight;
    }
    item.stroke.enabled = strokeEnabled;
    item.stroke.width = strokeWidth;
    item.stroke.color = kStrokeColour;
    item.shadow.enabled = shadowEnabled;
    item.shadow.color = kShadowColour;
    item.shadow.opacity = 1.0;  // the fixtures measure geometry, not the ramp
    item.shadow.offsetX = offsetX;
    item.shadow.offsetY = offsetY;
    item.shadow.blurSigma = blurSigma;
    subject.items.push_back(std::move(item));
    return subject;
}

// FontFamily is a plain struct (the wire encoder walks its fields), so the
// suite compares scans by hand: a cached scan that quietly re-walked
// fontconfig and came back in a different order would look like a font change.
bool sameScans(const std::vector<worker::SceneTextEngine::FontFamily>& a,
               const std::vector<worker::SceneTextEngine::FontFamily>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        // FontFace does declare operator==; the families themselves do not.
        if (a[i].family != b[i].family || a[i].faces != b[i].faces) {
            return false;
        }
    }
    return true;
}

const PinnedFont* findFont(const Corpus& corpus, const std::string& role) {
    for (const PinnedFont& font : corpus.fonts) {
        if (font.role == role) {
            return &font;
        }
    }
    return nullptr;
}

std::string utf8For(std::uint32_t point) {
    char bytes[5] = {};
    if (point < 0x80u) {
        bytes[0] = static_cast<char>(point);
    } else if (point < 0x800u) {
        bytes[0] = static_cast<char>(0xC0u | (point >> 6));
        bytes[1] = static_cast<char>(0x80u | (point & 0x3Fu));
    } else if (point < 0x10000u) {
        bytes[0] = static_cast<char>(0xE0u | (point >> 12));
        bytes[1] = static_cast<char>(0x80u | ((point >> 6) & 0x3Fu));
        bytes[2] = static_cast<char>(0x80u | (point & 0x3Fu));
    } else {
        bytes[0] = static_cast<char>(0xF0u | (point >> 18));
        bytes[1] = static_cast<char>(0x80u | ((point >> 12) & 0x3Fu));
        bytes[2] = static_cast<char>(0x80u | ((point >> 6) & 0x3Fu));
        bytes[3] = static_cast<char>(0x80u | (point & 0x3Fu));
    }
    return std::string(bytes);
}

/* --- fixtures ------------------------------------------------------------- */

// Family resolution first, pixels second. The engine's OWN scan (the one TS.4
// put on the wire) has to name the pinned families and nothing else: if the
// private fontconfig file were ignored, the machine's fonts would walk in and
// every measurement below would be about them.
void fixtureCorpusIsolation(worker::SceneTextEngine& engine, const Corpus& corpus) {
    std::vector<worker::SceneTextEngine::FontFamily> families;
    std::string error;
    if (!check(engine.listFonts(&families, &error),
               "the engine scans its own font manager: " + error)) {
        return;
    }
    std::vector<std::string> names;
    std::string joined;
    for (const auto& family : families) {
        names.push_back(family.family);
        joined += family.family + " ";
    }
    if (!check(names.size() == corpus.fonts.size(),
               "the isolated font manager sees exactly the " +
                   std::to_string(corpus.fonts.size()) + " pinned families (sees: " + joined +
                   ")")) {
        return;
    }
    bool matches = true;
    for (const PinnedFont& font : corpus.fonts) {
        if (std::find(names.begin(), names.end(), font.family) == names.end()) {
            matches = false;
        }
    }
    check(matches, "every pinned family resolves by the name the manifest pins: " + joined);
    bool faces = true;
    for (const auto& family : families) {
        if (family.faces.empty()) {
            faces = false;
        }
    }
    check(faces, "each pinned family reports at least one face (weight and slant)");

    sk_sp<SkFontMgr> manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
    if (!check(manager != nullptr, "a second look at the same fontconfig configuration")) {
        return;
    }
    for (const PinnedFont& font : corpus.fonts) {
        check(manager->matchFamilyStyle(font.family.c_str(), SkFontStyle::Normal()) != nullptr,
              "family \"" + font.family + "\" resolves through fontconfig");
    }
    check(manager->matchFamilyStyle("OpenShow No Such Family 47", SkFontStyle::Normal()) ==
              nullptr,
          "the deliberate missing family resolves to nothing - it is not hiding in the corpus");
}

// Four families are four faces. If two produced the same ink for the same
// words, one of the pinned files would not be the face its manifest role
// claims.
void fixtureFamilyChangesThePixels(const Corpus& corpus) {
    const std::vector<std::string> lines = {"Hallelujah 1234", "glory in the highest"};
    struct Shot {
        std::string family;
        Frame frame;
        Ink ink;
    };
    std::vector<Shot> shots;
    for (const PinnedFont& font : corpus.fonts) {
        Shot shot;
        shot.family = font.family;
        std::string error;
        if (!check(renderFrame(styledScene(font.family, lines, 64.0), "family-" + font.family,
                               &shot.frame, &error),
                   font.family + " renders: " + error)) {
            continue;
        }
        shot.ink = measureInk(shot.frame);
        check(shot.ink.count > 500, font.family + " draws real ink");
        shots.push_back(std::move(shot));
    }
    // Four faces for the same words differ in the AMOUNT of ink they lay down.
    // (Bounding-box width is not the discriminator: two faces can coincide to
    // the pixel on one line's width, and Inter and Lora do here — which is
    // exactly why the pair assertion is about coverage, and the role assertion
    // below is about the condensed face's width.)
    for (size_t a = 0; a < shots.size(); ++a) {
        for (size_t b = a + 1; b < shots.size(); ++b) {
            check(shots[a].ink.count != shots[b].ink.count,
                  shots[a].family + " and " + shots[b].family +
                      " cover a different number of pixels with the same words (" +
                      std::to_string(shots[a].ink.count) + " vs " +
                      std::to_string(shots[b].ink.count) + ")");
            check(differingPixels(shots[a].frame, shots[b].frame) > 2000,
                  shots[a].family + " and " + shots[b].family +
                      " draw visibly different pixels");
        }
    }
    const PinnedFont* sans = findFont(corpus, "sans");
    const PinnedFont* condensed = findFont(corpus, "condensed");
    if (sans != nullptr && condensed != nullptr) {
        const auto widthOf = [&shots](const std::string& family) {
            for (const Shot& shot : shots) {
                if (shot.family == family) {
                    return shot.ink.right - shot.ink.left;
                }
            }
            return 0;
        };
        const int sansWidth = widthOf(sans->family);
        const int condensedWidth = widthOf(condensed->family);
        check(condensedWidth * 10 < sansWidth * 9,
              "the condensed face is more than 10% narrower than the sans (" +
                  std::to_string(condensedWidth) + " vs " + std::to_string(sansWidth) +
                  " device px) - the roles in the manifest mean what they say");
    }
}

// The weight ramp resolves a FACE, and it must not re-wrap: TS.2's promise is
// that weight enters shaping, so the same words keep their positions and only
// the strokes change. A synthetic embolden or a fallback swap would show up
// here as ink that did not grow, or as rows that moved.
void fixtureWeightRampResolvesAFace(const Corpus& corpus) {
    const std::vector<std::string> lines = {"Blessed be the lord", "forever and ever amen"};
    for (const PinnedFont& font : corpus.fonts) {
        Frame light;
        Frame heavy;
        std::string error;
        if (!check(renderFrame(styledScene(font.family, lines, 56.0, 100),
                               "weight100-" + font.family, &light, &error),
                   font.family + " at weight 100 renders: " + error) ||
            !check(renderFrame(styledScene(font.family, lines, 56.0, 900),
                               "weight900-" + font.family, &heavy, &error),
                   font.family + " at weight 900 renders: " + error)) {
            continue;
        }
        const Ink lightInk = measureInk(light);
        const Ink heavyInk = measureInk(heavy);
        check(heavyInk.count > lightInk.count + lightInk.count / 5,
              font.family + ": weight 900 lays down more than 20% more ink than weight 100 (" +
                  std::to_string(lightInk.count) + " -> " + std::to_string(heavyInk.count) + ")");
        check(heavyInk.right - heavyInk.left >= lightInk.right - lightInk.left,
              font.family + ": the heavier weight is not narrower");
        const std::vector<double> lightRows = bandCenterYs(light);
        const std::vector<double> heavyRows = bandCenterYs(heavy);
        bool sameRows = lightRows.size() == heavyRows.size() && lightRows.size() == lines.size();
        for (size_t i = 0; sameRows && i < lightRows.size(); ++i) {
            sameRows = std::abs(lightRows[i] - heavyRows[i]) <= 1.0;
        }
        check(sameRows,
              font.family + ": the weight ramp changes the face WITHOUT moving or re-wrapping a "
                            "line (" +
                  std::to_string(lightRows.size()) + " vs " + std::to_string(heavyRows.size()) +
                  " bands)");
    }
}

// Body and caption sizes on the same face: shaping scales with the authored
// size, and the audience's smallest legible size still draws. The ratio is
// measured through layout() rather than pixels because these are METRICS
// claims - how tall a line of this face is at this size - and the pixel
// fixtures above already own "the words land where the layout says".
void fixtureBodyAndCaptionMetrics(worker::SceneTextEngine& engine, const Corpus& corpus) {
    const std::vector<std::string> lines = {"Lord is my shepherd", "I shall not want"};
    constexpr double kBody = 64.0;
    constexpr double kCaption = 28.0;
    for (const PinnedFont& font : corpus.fonts) {
        // "fixed" both times: shrink would answer a different question (and
        // the shrink path has its own fixture below).
        scene::Scene body = styledScene(font.family, lines, kBody, 400, "fixed");
        scene::Scene caption = styledScene(font.family, lines, kCaption, 400, "fixed");
        const auto bodyLayout = engine.layoutScene(body);
        const auto captionLayout = engine.layoutScene(caption);
        if (!check(bodyLayout.items.size() == 1 && bodyLayout.items[0].hasText &&
                       captionLayout.items.size() == 1 && captionLayout.items[0].hasText,
                   font.family + ": both text sizes lay out")) {
            continue;
        }
        const double bodyHeight = bodyLayout.items[0].height;
        const double captionHeight = captionLayout.items[0].height;
        const double expected = kBody / kCaption;
        const double actual = bodyHeight / captionHeight;
        check(std::abs(actual - expected) < expected * 0.05,
              font.family + ": a body line is " + std::to_string(expected) +
                  "x a caption line (+-5%) - the size means what it says (found " +
                  std::to_string(actual) + ")");
        check(bodyLayout.items[0].lineCount == captionLayout.items[0].lineCount,
              font.family + ": neither size wrapped the other's single lines");
        // And the caption is still a real answer: it has ink and a floor it
        // sits above (the contract's minimum is 12, so 28 is a legal size).
        check(captionHeight > 0.0 && !captionLayout.items[0].ink.empty,
              font.family + ": the caption size has ink");
        std::printf("  record: %-10s body %.1f px, caption %.1f px, ratio %.3f\n",
                    font.family.c_str(), bodyHeight, captionHeight, actual);
    }
}

// Extreme LEGAL values on a pinned face, checked against TS.3's ink bounds:
// the widest stroke the contract carries, a shadow pushed up and LEFT
// (negative offsets are legal, and they are the case a bounds that assumed
// positive ones would clip), and the two together.
void fixtureExtremeEffectsStayInsideInk(worker::SceneTextEngine& engine, const Corpus& corpus) {
    const PinnedFont* sans = findFont(corpus, "sans");
    if (sans == nullptr) {
        return;
    }
    std::string error;
    scene::Scene base = styledScene(sans->family, {"Crown him"}, 96.0);
    base.items[0].x = 560.0;  // narrower than the canvas, so edges mean something
    base.items[0].y = 340.0;
    base.items[0].width = 800.0;
    base.items[0].height = 400.0;

    scene::Scene outlined = base;
    outlined.items[0].stroke.enabled = true;
    outlined.items[0].stroke.width = 32.0;  // the contract's maximum

    scene::Scene northWest = base;
    northWest.items[0].shadow.enabled = true;
    northWest.items[0].shadow.offsetX = -40.0;
    northWest.items[0].shadow.offsetY = -30.0;
    northWest.items[0].shadow.blurSigma = 0.0;

    scene::Scene everything = outlined;
    everything.items[0].shadow = northWest.items[0].shadow;
    everything.items[0].shadow.blurSigma = 64.0;  // the contract's maximum blur

    struct Saved {
        worker::SceneTextEngine::InkRect ink;
        worker::SceneTextEngine::InkRect fillInk;
        float boxX = 0.0f;
        float top = 0.0f;
    };
    const scene::Scene* cases[] = {&base, &outlined, &northWest, &everything};
    Saved saved[4];
    for (int i = 0; i < 4; ++i) {
        const auto layout = engine.layoutScene(*cases[i]);
        if (!check(layout.items.size() == 1 && layout.items[0].hasText,
                   "the effect fixture's layout is a real one")) {
            return;
        }
        saved[i].ink = layout.items[0].ink;
        saved[i].fillInk = layout.items[0].fillInk;
        saved[i].boxX = layout.items[0].boxX;
        saved[i].top = worker::SceneTextEngine::paragraphTop(layout.items[0]);
    }

    Frame plainFrame;
    if (!check(renderFrame(base, "fx-plain", &plainFrame, &error),
               "the plain fill renders: " + error)) {
        return;
    }
    const Ink plainInk = measureInk(plainFrame);

    Frame outlinedFrame;
    if (check(renderFrame(outlined, "fx-stroke-max", &outlinedFrame, &error),
              "the widest legal stroke renders: " + error)) {
        const Ink grown = measureInk(outlinedFrame);
        const int grow = static_cast<int>((32.0 / 2.0) * kScale);  // 8 device px
        check(grown.left <= plainInk.left - grow + kSlack &&
                  grown.left >= plainInk.left - grow - kSlack,
              "a width-32 stroke grows the left edge by half its width (" +
                  std::to_string(plainInk.left) + " -> " + std::to_string(grown.left) +
                  ", expected about -" + std::to_string(grow) + ")");
        check(grown.right >= plainInk.right + grow - kSlack &&
                  grown.right <= plainInk.right + grow + kSlack,
              "a width-32 stroke grows the right edge by the same");
        check(grown.top <= plainInk.top - grow + kSlack &&
                  grown.bottom >= plainInk.bottom + grow - kSlack,
              "a width-32 stroke grows top and bottom too");
        check(measureClass(outlinedFrame, kStrokeRgb).count > 1000,
              "the outline is drawn in its own colour");
        check(measureClass(outlinedFrame, kFill).count > 0,
              "and the fill survives UNDER the outline - outline behind fill, not instead of it");
    }

    Frame ghostFrame;
    if (check(renderFrame(northWest, "fx-shadow-northwest", &ghostFrame, &error),
              "the negatively offset shadow renders: " + error)) {
        const Ink total = measureInk(ghostFrame);
        const int shiftX = static_cast<int>(std::abs(-40.0) * kScale);  // 20 px
        const int shiftY = static_cast<int>(std::abs(-30.0) * kScale);  // 15 px
        check(total.left <= plainInk.left - shiftX + kSlack,
              "a shadow at offsetX -40 reaches " + std::to_string(shiftX) +
                  " device px LEFT of the fill's own edge (found " + std::to_string(total.left) +
                  " vs " + std::to_string(plainInk.left) + ")");
        check(total.top <= plainInk.top - shiftY + kSlack,
              "and offsetY -30 reaches above it (found " + std::to_string(total.top) + " vs " +
                  std::to_string(plainInk.top) + ")");
        check(total.count > plainInk.count, "the shadow is extra ink, never a replacement");
    }

    // Every drawn pixel of the worst combination inside `ink` mapped through
    // the device scale, and `fillInk` unmoved by the effects: that box is what
    // a fits verdict judges, so an effect that grew it would silently change
    // pagination.
    Frame everythingFrame;
    if (!check(renderFrame(everything, "fx-everything", &everythingFrame, &error),
               "the extreme combination renders: " + error)) {
        return;
    }
    const Saved& both = saved[3];
    const Ink pixels = measureInk(everythingFrame);
    const double devLeft = (both.boxX + both.ink.left) * kScale;
    const double devRight = (both.boxX + both.ink.right) * kScale;
    const double devTop = (both.top + both.ink.top) * kScale;
    const double devBottom = (both.top + both.ink.bottom) * kScale;
    check(pixels.count > 0 && pixels.left >= std::floor(devLeft) - kSlack &&
                  pixels.right <= std::ceil(devRight) + kSlack &&
                  pixels.top >= std::floor(devTop) - kSlack &&
                  pixels.bottom <= std::ceil(devBottom) + kSlack,
          "every pixel of a width-32 stroke under a sigma-64 northwest shadow lands inside the "
          "item's ink rect at the 0.5 scale (px [" +
              std::to_string(pixels.left) + "," + std::to_string(pixels.top) + "," +
              std::to_string(pixels.right) + "," + std::to_string(pixels.bottom) + "] bounds [" +
              std::to_string(static_cast<int>(devLeft)) + "," +
              std::to_string(static_cast<int>(devTop)) + "," +
              std::to_string(static_cast<int>(devRight)) + "," +
              std::to_string(static_cast<int>(devBottom)) + "])");
    check(saved[0].fillInk.left == saved[3].fillInk.left &&
                  saved[0].fillInk.right == saved[3].fillInk.right &&
                  saved[0].fillInk.top == saved[3].fillInk.top &&
                  saved[0].fillInk.bottom == saved[3].fillInk.bottom,
          "enabling both effects leaves fillInk exactly where it was - the WORDS verdict cannot "
          "be moved by an outline");
    check(both.ink.left < saved[0].fillInk.left && both.ink.right > saved[0].fillInk.right &&
                  both.ink.bottom > saved[0].fillInk.bottom,
          "and the pixel-truth rect is the one that grew");
}

// Long lines in a small box: shrink answers by getting smaller, fixed keeps
// the authored size and lets the audience see what does not fit.
void fixtureFixedVersusShrink(const Corpus& corpus) {
    const PinnedFont* serif = findFont(corpus, "serif");
    if (serif == nullptr) {
        return;
    }
    const std::vector<std::string> lines = {
        "Give me the hill with the spring in it", "and the anointing and the vision",
        "and every single promise You have made"};
    std::string error;
    Frame shrunk;
    Frame fixed;
    const bool okShrink = renderFrame(
        styledScene(serif->family, lines, 64.0, 400, "shrink", false, 2.0, false, 0.0, 0.0, 0.0,
                    520.0, 260.0),
        "fit-shrink", &shrunk, &error);
    const bool okFixed = renderFrame(
        styledScene(serif->family, lines, 64.0, 400, "fixed", false, 2.0, false, 0.0, 0.0, 0.0,
                    520.0, 260.0),
        "fit-fixed", &fixed, &error);
    if (!check(okShrink && okFixed, "both fit modes render the same overfull box: " + error)) {
        return;
    }
    const Ink shrinkInk = measureInk(shrunk);
    const Ink fixedInk = measureInk(fixed);
    check(fixedInk.count > shrinkInk.count,
          "fitMode fixed draws MORE ink than shrink in the same overfull box - it kept the size (" +
              std::to_string(shrinkInk.count) + " vs " + std::to_string(fixedInk.count) + ")");
    const int boxDeviceHeight = static_cast<int>(260.0 * kScale);  // the authored box in device px
    const int shrinkHeight = shrinkInk.bottom - shrinkInk.top;
    const int fixedHeight = fixedInk.bottom - fixedInk.top;
    check(shrinkHeight <= boxDeviceHeight + kSlack,
          "shrink brings the stanza inside its box (" + std::to_string(shrinkHeight) +
              " device px in a " + std::to_string(boxDeviceHeight) + " px box)");
    check(fixedHeight > boxDeviceHeight + kSlack,
          "fixed keeps the authored size and lets the audience see what does not fit (" +
              std::to_string(fixedHeight) + " device px in the same box)");
    check(fixedHeight > shrinkHeight,
          "and the two modes disagree about the SIZE, not merely about which pixels are lit");
}

// The deliberate missing family: where does it land? Inside the pinned corpus,
// or the isolation is a story we tell ourselves.
void fixtureMissingFamilyFallsBackInsideTheCorpus(const Corpus& corpus) {
    sk_sp<SkFontMgr> manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
    if (!check(manager != nullptr, "the font manager for the fallback question")) {
        return;
    }
    const char* langs[] = {"en"};
    sk_sp<SkTypeface> ghost =
        manager->matchFamilyStyleCharacter(nullptr, SkFontStyle::Normal(), langs, 1, 'H');
    if (!check(ghost != nullptr, "fontconfig still answers a character with no family named")) {
        return;
    }
    SkString resolved;
    ghost->getFamilyName(&resolved);
    const std::string fallback(resolved.c_str());
    bool inside = false;
    for (const PinnedFont& font : corpus.fonts) {
        if (font.family == fallback) {
            inside = true;
        }
    }
    check(inside, "the fallback face for a missing family is one of the pinned corpus (\"" +
                      fallback + "\") - the isolation held");
    if (!inside) {
        return;
    }

    const std::vector<std::string> lines = {"Hallelujah 1234"};
    std::string error;
    Frame missing;
    Frame viaFallback;
    const bool okMissing = renderFrame(
        styledScene("OpenShow No Such Family 47", lines, 64.0), "ghost-family", &missing, &error);
    const bool okFallback = renderFrame(styledScene(fallback, lines, 64.0),
                                        "ghost-via-" + fallback, &viaFallback, &error);
    if (!check(okMissing && okFallback,
               "the missing family and the fallback family both render: " + error)) {
        return;
    }
    check(measureInk(missing).count > 500,
          "a missing family still draws words - the audience never sees a blank slide");
    check(sameFrame(missing, viaFallback),
          "and it draws exactly what \"" + fallback +
              "\" draws: the fallback is a resolved face, not an approximation");
    for (const PinnedFont& font : corpus.fonts) {
        if (font.family == fallback) {
            continue;
        }
        Frame other;
        if (renderFrame(styledScene(font.family, lines, 64.0), "ghost-vs-" + font.family, &other,
                        &error)) {
            check(differingPixels(missing, other) > 2000,
                  "the missing family does NOT look like " + font.family);
        }
    }
}

// Which code points do the pinned faces actually carry? Claimed only where the
// face has the glyph, with the matrix printed as the record and the corpus's
// boundary stated rather than papered over.
void fixtureUnicodeCoverageIsWhatTheFacesSay(const Corpus& corpus) {
    sk_sp<SkFontMgr> manager = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
    if (!check(manager != nullptr, "the font manager for the coverage matrix")) {
        return;
    }
    struct Script {
        const char* name;
        std::vector<std::uint32_t> points;
    };
    const std::vector<Script> scripts = {
        {"latin", {0x0041u, 0x00e9u, 0x0105u}},
        {"greek", {0x03b1u, 0x03a9u}},
        {"cyrillic", {0x0414u, 0x0436u}},
        {"hebrew", {0x05d0u}},
        {"cjk", {0x4e2du}},
        {"emoji", {0x1F600u}},
    };
    const PinnedFont* fallback = findFont(corpus, "fallback");
    int familiesWithGreek = 0;
    int familiesWithCyrillic = 0;
    for (const PinnedFont& font : corpus.fonts) {
        sk_sp<SkTypeface> typeface =
            manager->matchFamilyStyle(font.family.c_str(), SkFontStyle::Normal());
        if (!check(typeface != nullptr, font.family + " resolves for the coverage matrix")) {
            continue;
        }
        std::string line = "  coverage " + font.family + ":";
        for (const Script& script : scripts) {
            bool has = true;
            for (std::uint32_t point : script.points) {
                if (typeface->unicharToGlyph(point) == 0) {
                    has = false;
                }
            }
            line += std::string(" ") + script.name + "=" + (has ? "yes" : "no");
            if (has) {
                if (std::string(script.name) == "greek") {
                    ++familiesWithGreek;
                }
                if (std::string(script.name) == "cyrillic") {
                    ++familiesWithCyrillic;
                }
            }
        }
        std::printf("%s\n", line.c_str());
        check(typeface->unicharToGlyph('A') != 0, font.family + " draws basic Latin");
    }
    check(familiesWithGreek >= 1, "the corpus carries at least one face for Greek");
    check(familiesWithCyrillic >= 1, "the corpus carries at least one face for Cyrillic");
    check(fallback != nullptr, "the corpus names a fallback face");
    if (fallback == nullptr) {
        return;
    }

    // Pixels follow the matrix. A script NO pinned face carries is drawn as
    // the renderer's placeholder: a visible box rather than a blank slide or a
    // crash. That is the corpus's boundary, stated here so nobody reads these
    // fixtures as a product promise about CJK or emoji.
    for (const Script& script : scripts) {
        std::string text;
        for (std::uint32_t point : script.points) {
            text += utf8For(point);
        }
        Frame frame;
        std::string error;
        if (!check(renderFrame(styledScene(fallback->family, {text}, 72.0),
                               "unicode-" + std::string(script.name), &frame, &error),
                   std::string(script.name) + " renders through the fallback face: " + error)) {
            continue;
        }
        check(measureInk(frame).count > 0,
              std::string(script.name) +
                  " draws something - a real glyph or the placeholder box - and never crashes");
    }
}

// The midpoint fade over a slide carrying every effect at once. TS.2 measured
// this on whatever faces the machine had; here it is the pinned sans, because
// the audience sees the fade on the face the operator chose.
void fixtureMidpointFadeAttenuatesEveryEffect(const Corpus& corpus) {
    const PinnedFont* sans = findFont(corpus, "sans");
    if (sans == nullptr) {
        return;
    }
    const scene::Scene styled = styledScene(sans->family, {"Glory", "and honour"}, 88.0, 700,
                                            "shrink", true, 10.0, true, 12.0, 10.0, 6.0);
    scene::Scene blank;
    blank.showId = "corpus-show";
    blank.slideId = "corpus-blank";
    blank.background.color = kBackdrop;  // the same backdrop: only the words fade
    std::string error;
    Frame settled;
    Frame midpoint;
    worker::SnapshotOptions options;
    options.fadeFrom = &blank;
    options.mediaBlend = 1.0f;
    options.textBlend = 0.5f;
    if (!check(renderFrame(styled, "fade-settled", &settled, &error),
               "the settled styled frame renders: " + error) ||
        !check(renderFrame(styled, "fade-mid", &midpoint, &error, options),
               "the same frame at the text midpoint renders: " + error)) {
        return;
    }
    // Placement first, and it needs the RELATIVE ruler: at the midpoint every
    // pixel of words is half as bright, so a frame measured against an
    // absolute background threshold would report the ink box as two pixels
    // smaller - which is the antialiasing dimming, not a moved word. Compared
    // against each frame's own peak, the two boxes are the same box.
    const Ink settledInk = measureInkRelative(settled, 16);
    const Ink midInk = measureInkRelative(midpoint, 16);
    check(std::abs(settledInk.left - midInk.left) <= 1 &&
                  std::abs(settledInk.right - midInk.right) <= 1 &&
                  std::abs(settledInk.top - midInk.top) <= 1 &&
                  std::abs(settledInk.bottom - midInk.bottom) <= 1,
          "the fade does not move or resize a word by a pixel (settled [" +
              std::to_string(settledInk.left) + "," + std::to_string(settledInk.top) + "," +
              std::to_string(settledInk.right) + "," + std::to_string(settledInk.bottom) +
              "] vs midpoint [" + std::to_string(midInk.left) + "," + std::to_string(midInk.top) +
              "," + std::to_string(midInk.right) + "," + std::to_string(midInk.bottom) + "])");

    // Then the three passes SEPARATELY, which is the point of this fixture:
    // TS.2's shadow rides the paragraph's own style and the stroke rides a
    // second painter pass, so a fade that attenuated the fill but not the
    // outline - or dropped the shadow copy - is a real bug these two frames
    // can tell apart. Each pass's pixels are its hue in the SETTLED frame;
    // the same positions in the faded frame should carry about half the
    // distance from the background. The median is the statistic because an
    // overlapping edge pixel attenuates differently from a core pixel, and
    // TS.2's parity fixture already recorded why.
    struct Track {
        const char* name;
        Hue hue;
    };
    for (const Track& track : {Track{"fill", Hue::Fill}, Track{"stroke", Hue::Stroke},
                               Track{"shadow", Hue::Shadow}}) {
        std::vector<double> ratios;
        int drawnInMid = 0;
        const SkColor background = settled.at(2, 2);
        for (int y = 0; y < settled.height; ++y) {
            for (int x = 0; x < settled.width; ++x) {
                Hue hue;
                if (!hueOf(settled.at(x, y), &hue) || hue != track.hue) {
                    continue;
                }
                if (hueOf(midpoint.at(x, y), &hue) && hue == track.hue) {
                    ++drawnInMid;
                }
                const int full = channelSumAbove(settled.at(x, y), background);
                const int half = channelSumAbove(midpoint.at(x, y), background);
                if (full >= 60) {
                    ratios.push_back(static_cast<double>(half) / static_cast<double>(full));
                }
            }
        }
        const std::string label = std::string(track.name);
        if (!check(ratios.size() > 200 && drawnInMid > 200,
                   label + " is drawn in its own hue at both the settled frame and the midpoint (" +
                       std::to_string(ratios.size()) + " then " + std::to_string(drawnInMid) +
                       " pixels)")) {
            continue;
        }
        std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
        const double median = ratios[ratios.size() / 2];
        check(median > 0.40 && median < 0.66,
              label + " rides the midpoint fade to about half its distance from the background "
                      "(median ratio " +
                  std::to_string(median) + ")");
        std::printf("  record: fade %-7s median ratio %.3f over %zu px\n", track.name, median,
                    ratios.size());
    }
}

/* --- costs and cache bounds (the resource half of the leaf) --------------- */

double milliseconds(const std::chrono::steady_clock::time_point& from,
                    const std::chrono::steady_clock::time_point& to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

void fixtureCostIsBounded(worker::SceneTextEngine& engine, const Corpus& corpus) {
    // Nothing has touched this engine yet: the scan below is the one scan, and
    // each family's first layout below is its first shape.
    std::string error;
    const auto scanStart = std::chrono::steady_clock::now();
    std::vector<worker::SceneTextEngine::FontFamily> first;
    const bool scanned = engine.listFonts(&first, &error);
    const double firstScan = milliseconds(scanStart, std::chrono::steady_clock::now());
    if (!check(scanned, "the font scan for the cost record: " + error)) {
        return;
    }
    const auto cachedStart = std::chrono::steady_clock::now();
    std::vector<worker::SceneTextEngine::FontFamily> second;
    engine.listFonts(&second, &error);
    const double cachedScan = milliseconds(cachedStart, std::chrono::steady_clock::now());
    check(sameScans(first, second), "the scan gives the same answer twice (one scan per lifetime)");
    check(cachedScan <= firstScan + 1.0,
          "the second ask copies the cache instead of walking fontconfig again (" +
              std::to_string(firstScan) + " ms -> " + std::to_string(cachedScan) + " ms)");
    std::printf("  record: %zu pinned families in %s\n", corpus.fonts.size(),
                corpus.directory.string().c_str());
    std::printf("  record: font scan %.1f ms first ask, %.2f ms cached\n", firstScan, cachedScan);

    // A family's first shape pays the face lookup and the file load; the same
    // ask again must not pay it back. The ceilings are deliberately generous:
    // the claim is "no unbounded cost", and the printed numbers are the record
    // docs/fixtures/fonts.md carries.
    for (const PinnedFont& font : corpus.fonts) {
        const scene::Scene subject = styledScene(
            font.family, {"Amazing grace how sweet the sound", "that saved a wretch like me"}, 54.0);
        const auto coldStart = std::chrono::steady_clock::now();
        const worker::SceneTextEngine::SceneLayout cold = engine.layoutScene(subject);
        const double coldMs = milliseconds(coldStart, std::chrono::steady_clock::now());
        if (!check(cold.items.size() == 1 && cold.items[0].hasText,
                   font.family + ": the costed layout is a real one")) {
            continue;
        }
        double steadyMs = 0.0;
        constexpr int kRepeats = 20;
        for (int i = 0; i < kRepeats; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const worker::SceneTextEngine::SceneLayout again = engine.layoutScene(subject);
            steadyMs += milliseconds(start, std::chrono::steady_clock::now());
            (void)again;
        }
        steadyMs /= kRepeats;
        check(coldMs < 3000.0, font.family +
                                  ": the first shape of a two-line stanza is bounded (" +
                                      std::to_string(coldMs) + " ms)");
        check(steadyMs <= coldMs + 1.0, font.family +
                                            ": shaping the same words again is not slower than the "
                                            "first time (" +
                                                std::to_string(steadyMs) + " ms vs " +
                                                std::to_string(coldMs) + " ms)");
        std::printf("  record: %-10s first shape %7.1f ms, steady %7.2f ms\n", font.family.c_str(),
                    coldMs, steadyMs);
    }
}

// Four slots, LRU, retained handles (task 6.9b/6.21a): the audience's fade
// pair plus the selected edit and its predecessor cost no rebuild, a fifth
// scene pushes one out, and nothing grows however hard the browser churns.
// "No new shaping" is observed exactly, not inferred from a stopwatch: a hit
// hands back the SAME layout object.
void fixtureLayoutCacheStaysBounded(worker::SceneTextEngine& engine, const Corpus& corpus) {
    const PinnedFont* sans = findFont(corpus, "sans");
    if (sans == nullptr) {
        return;
    }
    std::unique_ptr<worker::SceneTextEngine::LayoutCache> cache =
        worker::SceneTextEngine::newLayoutCache(engine);
    if (!check(cache != nullptr, "a layout cache for the bounds assertions")) {
        return;
    }
    std::vector<std::shared_ptr<const scene::Scene>> prepared;
    for (int i = 0; i < 12; ++i) {
        prepared.push_back(std::make_shared<scene::Scene>(styledScene(
            sans->family, {"Verse " + std::to_string(i), "the words people sing"}, 48.0,
            400 + (i % 3) * 100)));
    }
    const auto churnStart = std::chrono::steady_clock::now();
    for (const auto& subject : prepared) {
        cache->put(subject, subject.get(), cache->build(*subject));
    }
    const double churnMs = milliseconds(churnStart, std::chrono::steady_clock::now());

    int resident = 0;
    for (size_t i = prepared.size() - 4; i < prepared.size(); ++i) {
        if (cache->find(prepared[i].get()) != nullptr) {
            ++resident;
        }
    }
    check(resident == 4,
          "a steady show's four scenes stay resident after churning twelve (four slots is the "
          "contract)");
    check(cache->find(prepared[0].get()) == nullptr,
          "the fifth scene back is gone - occupancy never exceeded four");

    const auto last = prepared.back();
    const std::shared_ptr<const worker::SceneTextEngine::SceneLayout> held = cache->find(last.get());
    bool steady = held != nullptr;
    for (int i = 0; i < 50 && steady; ++i) {
        steady = cache->find(last.get()) == held;
    }
    check(steady, "fifty draws of one resident scene are fifty hits on one retained layout - no "
                  "shaping between them");
    std::printf("  record: 12 scene builds in %.1f ms, 4 resident, fifth evicted\n", churnMs);
}

}  // namespace

int main(int argc, char** argv) {
    fixtureHashHelperIsCorrect();

    if (argc < 3) {
        std::fprintf(stderr,
                     "FAIL: font_fixture_tests takes the pinned-manifest path as argv[1] and the "
                     "review document that pinned it as argv[2]; tests/CMakeLists.txt passes "
                     "both.\n");
        return 1;
    }
    Corpus corpus;
    std::string error;
    if (!loadManifest(fs::path(argv[1]), &corpus, &error)) {
        std::fprintf(stderr, "FAIL: the pinned corpus manifest is unusable (%s)\n", error.c_str());
        return 1;
    }
    corpus.directory = corpusDirectory();

    // Before any skip decision: a manifest that has drifted from the review's
    // pin is wrong whether or not this machine has the bytes, and reporting it
    // as a skip would bury the only machine that could still fix it quietly.
    fixtureManifestAgreesWithTheReview(fs::path(argv[2]), corpus);
    if (failures > 0) {
        std::fprintf(stderr,
                     "font_fixture_tests: the manifest no longer agrees with the review that "
                     "pinned it (%d failure(s))\n", failures);
        return 1;
    }

    std::vector<std::string> problems;
    const Prepared state = prepareCorpus(corpus, &problems);
    if (state == Prepared::Corrupt) {
        for (const std::string& problem : problems) {
            std::fprintf(stderr, "FAIL: %s\n", problem.c_str());
        }
        std::fprintf(stderr,
                     "FAIL: the pinned font corpus in %s does not match the manifest. Run "
                     "tools/setup-font-fixtures.sh prepare --force: the bytes on disk are NOT the "
                     "fonts these fixtures are about.\n",
                     corpus.directory.string().c_str());
        return 1;
    }
    if (state == Prepared::Absent) {
        std::printf("SKIP: the pinned font corpus is not prepared in %s\n",
                    corpus.directory.string().c_str());
        for (const std::string& problem : problems) {
            std::printf("SKIP:   %s\n", problem.c_str());
        }
        std::printf("SKIP: run `tools/setup-font-fixtures.sh prepare --download` (needs network "
                    "once)\n");
        std::printf("SKIP: or  `tools/setup-font-fixtures.sh prepare --from DIR` to place them "
                    "offline,\n");
        std::printf("SKIP: then `ctest --preset desktop -R font_fixture_tests`. Calling this a "
                    "pass would\n");
        std::printf("SKIP: mean measuring faces nobody pinned.\n");
        return 77;
    }

    // Isolated fontconfig: the corpus directory and a private cache, and by
    // design no include of /etc/fonts. Setting it before the engine exists is
    // what makes "the only faces this process can resolve are these" true
    // rather than aspirational.
    std::error_code ec;
    scratchDirectory = fs::temp_directory_path(ec) /
                       ("openshow-font-fixtures-" + std::to_string(static_cast<long>(::getpid())));
    if (ec || !fs::create_directories(scratchDirectory / "fontconfig-cache", ec)) {
        std::fprintf(stderr, "FAIL: cannot create the scratch directory %s\n",
                     scratchDirectory.string().c_str());
        return 1;
    }
    const fs::path fontConfig = scratchDirectory / "fonts.conf";
    bool wroteConfig = false;
    {
        std::ofstream conf(fontConfig);
        conf << "<?xml version=\"1.0\"?>\n"
             << "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
             << "<fontconfig>\n"
             << "  <dir>" << corpus.directory.string() << "</dir>\n"
             << "  <cachedir>" << (scratchDirectory / "fontconfig-cache").string()
             << "</cachedir>\n"
             << "</fontconfig>\n";
        conf.flush();
        wroteConfig = static_cast<bool>(conf);
    }
    if (!wroteConfig) {
        std::fprintf(stderr, "FAIL: cannot write %s\n", fontConfig.string().c_str());
        return 1;
    }
    ::setenv("FONTCONFIG_FILE", fontConfig.string().c_str(), 1);
    ::setenv("XDG_CACHE_HOME", (scratchDirectory / "xdg").c_str(), 1);

    worker::SceneTextEngine engine;
    const auto createStart = std::chrono::steady_clock::now();
    if (!check(engine.create(&error),
               "the production text engine starts over the corpus: " + error)) {
        return 1;
    }
    const double createMs =
        milliseconds(createStart, std::chrono::steady_clock::now());
    check(createMs < 5000.0, "starting the engine over a pinned corpus is bounded");

    std::printf("pinned corpus: google/fonts %s, %zu families, engine up in %.1f ms\n",
                corpus.revision.c_str(), corpus.fonts.size(), createMs);

    // The cost record runs FIRST, so its timings are genuinely cold: by the
    // time the pixel fixtures have run, the scan is cached and every face is
    // already shaped, and the numbers would be a fiction.
    fixtureCostIsBounded(engine, corpus);
    fixtureCorpusIsolation(engine, corpus);
    fixtureBodyAndCaptionMetrics(engine, corpus);
    fixtureFamilyChangesThePixels(corpus);
    fixtureWeightRampResolvesAFace(corpus);
    fixtureExtremeEffectsStayInsideInk(engine, corpus);
    fixtureFixedVersusShrink(corpus);
    fixtureMissingFamilyFallsBackInsideTheCorpus(corpus);
    fixtureUnicodeCoverageIsWhatTheFacesSay(corpus);
    fixtureMidpointFadeAttenuatesEveryEffect(corpus);
    fixtureLayoutCacheStaysBounded(engine, corpus);

    if (failures == 0) {
        fs::remove_all(scratchDirectory, ec);  // the PNGs were the evidence, not the verdict
    } else {
        std::fprintf(stderr, "the rendered PNGs are kept for review in %s\n",
                     scratchDirectory.string().c_str());
    }
    if (failures > 0) {
        std::fprintf(stderr, "font_fixture_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("font_fixture_tests: the pinned corpus drew every fixture\n");
    return 0;
}
