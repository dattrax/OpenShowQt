#include "openshow/domain/thumbnails.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include "openshow/domain/version.h"

namespace openshow::domain {

namespace {

constexpr std::uint64_t kProbeBytes = 4096;

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool isLowerHex(const std::string& text, std::size_t length) {
    if (text.size() != length) {
        return false;
    }
    for (char c : text) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return false;
        }
    }
    return true;
}

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

// --- SHA-256 ---------------------------------------------------------------
//
// One small, dependency-free implementation: the record checksum and the
// fingerprint both need a stable digest, and the worker's Qt-free presets have
// no other hash to share.

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

std::uint32_t rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

void sha256Transform(std::uint32_t state[8], const unsigned char block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

std::string hexOf(const std::uint32_t state[8]) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(digits[(state[i] >> shift) & 0xf]);
        }
    }
    return out;
}

// --- scene normalization ---------------------------------------------------

void clearIdentityMembers(boost::json::value& value) {
    if (value.is_object()) {
        boost::json::object& object = value.as_object();
        for (auto& member : object) {
            const std::string_view key = member.key();
            if ((key == "assetPath" || key == "showId" || key == "slideId") &&
                member.value().is_string()) {
                // A path is where the bytes happen to live; the ids are the
                // service's own names. Neither decides a pixel, and a Library
                // variant is copied onto freshly minted ids, so all three are
                // emptied. Asset BYTES still enter through ThumbnailAssetStamp.
                member.value() = boost::json::string("");
                continue;
            }
            clearIdentityMembers(member.value());
        }
        return;
    }
    if (value.is_array()) {
        for (boost::json::value& element : value.as_array()) {
            clearIdentityMembers(element);
        }
    }
}

std::uint64_t littleEndian(std::uint64_t value, std::string* out) {
    for (int i = 0; i < 8; ++i) {
        out->push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }
    return value;
}

bool readRange(std::ifstream& file, std::uint64_t offset, std::uint64_t length,
               std::string* out) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return false;
    }
    out->resize(static_cast<std::size_t>(length));
    if (length != 0) {
        file.read(out->data(), static_cast<std::streamsize>(length));
        if (static_cast<std::uint64_t>(file.gcount()) != length) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string sha256Hex(std::string_view bytes) {
    std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8;
    const std::size_t fullBlocks = bytes.size() / 64;
    for (std::size_t block = 0; block < fullBlocks; ++block) {
        sha256Transform(state, reinterpret_cast<const unsigned char*>(bytes.data() + block * 64));
    }
    unsigned char tail[128] = {0};
    const std::size_t remainder = bytes.size() % 64;
    if (remainder != 0) {
        std::memcpy(tail, bytes.data() + fullBlocks * 64, remainder);
    }
    tail[remainder] = 0x80;
    const std::size_t tailBlocks = remainder + 1 + 8 <= 64 ? 1 : 2;
    for (int i = 0; i < 8; ++i) {
        tail[tailBlocks * 64 - 1 - i] = static_cast<unsigned char>((bitLength >> (i * 8)) & 0xff);
    }
    for (std::size_t block = 0; block < tailBlocks; ++block) {
        sha256Transform(state, tail + block * 64);
    }
    return hexOf(state);
}

std::size_t base64EncodedSize(std::size_t rawBytes) {
    return ((rawBytes + 2) / 3) * 4;
}

std::string base64Encode(std::string_view bytes) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(base64EncodedSize(bytes.size()));
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        const std::uint32_t chunk = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                                    (static_cast<std::uint8_t>(bytes[i + 1]) << 8) |
                                    static_cast<std::uint8_t>(bytes[i + 2]);
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.push_back(alphabet[(chunk >> 6) & 0x3f]);
        out.push_back(alphabet[chunk & 0x3f]);
        i += 3;
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t chunk = static_cast<std::uint8_t>(bytes[i]) << 16;
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.append("==");
    } else if (rest == 2) {
        const std::uint32_t chunk = (static_cast<std::uint8_t>(bytes[i]) << 16) |
                                    (static_cast<std::uint8_t>(bytes[i + 1]) << 8);
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.push_back(alphabet[(chunk >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

namespace {

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

bool base64IsCanonical(std::string_view text, std::size_t* decodedBytes) {
    if (text.empty()) {
        // The empty text is the empty picture: the encoder emits nothing for
        // nothing, and a record's own bounds refuse a zero-length PNG.
        if (decodedBytes != nullptr) {
            *decodedBytes = 0;
        }
        return true;
    }
    if (text.size() % 4 != 0) {
        return false;
    }
    std::size_t padding = 0;
    if (text.back() == '=') {
        padding = 1;
        if (text.size() >= 2 && text[text.size() - 2] == '=') {
            padding = 2;
        }
    }
    for (std::size_t i = 0; i < text.size() - padding; ++i) {
        if (base64Value(text[i]) < 0) {
            return false;
        }
    }
    for (std::size_t i = text.size() - padding; i < text.size(); ++i) {
        if (text[i] != '=') {
            return false;
        }
    }
    if (padding != 0) {
        // The final data character's unused bits must be zero in a canonical
        // spelling; otherwise two texts decode to one picture.
        const int last = base64Value(text[text.size() - padding - 1]);
        if (last < 0) {
            return false;
        }
        const int unused = padding == 2 ? 4 : 2;
        if ((last & ((1 << unused) - 1)) != 0) {
            return false;
        }
    }
    if (decodedBytes != nullptr) {
        *decodedBytes = (text.size() / 4) * 3 - padding;
    }
    return true;
}

bool base64Decode(std::string_view text, std::string* out) {
    std::size_t decoded = 0;
    if (!base64IsCanonical(text, &decoded)) {
        return false;
    }
    out->clear();
    out->reserve(decoded);
    std::size_t i = 0;
    while (i + 4 <= text.size()) {
        const bool last = i + 4 == text.size();
        const int a = base64Value(text[i]);
        const int b = base64Value(text[i + 1]);
        const int c = text[i + 2] == '=' ? 0 : base64Value(text[i + 2]);
        const int d = text[i + 3] == '=' ? 0 : base64Value(text[i + 3]);
        const std::uint32_t chunk = (static_cast<std::uint32_t>(a) << 18) |
                                    (static_cast<std::uint32_t>(b) << 12) |
                                    (static_cast<std::uint32_t>(c) << 6) |
                                    static_cast<std::uint32_t>(d);
        out->push_back(static_cast<char>((chunk >> 16) & 0xff));
        if (!last || text[i + 2] != '=') {
            out->push_back(static_cast<char>((chunk >> 8) & 0xff));
        }
        if (!last || text[i + 3] != '=') {
            out->push_back(static_cast<char>(chunk & 0xff));
        }
        i += 4;
    }
    return out->size() == decoded;
}

std::string pngChecksum(std::string_view png) {
    return sha256Hex(png);
}

bool pngDimensions(std::string_view png, std::uint32_t* width, std::uint32_t* height) {
    static constexpr unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (png.size() < 24 || std::memcmp(png.data(), signature, sizeof(signature)) != 0) {
        return false;
    }
    const std::uint32_t headerLength =
        (static_cast<std::uint8_t>(png[8]) << 24) | (static_cast<std::uint8_t>(png[9]) << 16) |
        (static_cast<std::uint8_t>(png[10]) << 8) | static_cast<std::uint8_t>(png[11]);
    if (headerLength != 13 || std::memcmp(png.data() + 12, "IHDR", 4) != 0) {
        return false;
    }
    if (width != nullptr) {
        *width = (static_cast<std::uint8_t>(png[16]) << 24) |
                 (static_cast<std::uint8_t>(png[17]) << 16) |
                 (static_cast<std::uint8_t>(png[18]) << 8) | static_cast<std::uint8_t>(png[19]);
    }
    if (height != nullptr) {
        *height = (static_cast<std::uint8_t>(png[20]) << 24) |
                  (static_cast<std::uint8_t>(png[21]) << 16) |
                  (static_cast<std::uint8_t>(png[22]) << 8) | static_cast<std::uint8_t>(png[23]);
    }
    return true;
}

std::string rendererIdentity() {
    std::string identity = "openshow-scene-card/1;app=";
    identity.append(applicationVersion());
    return identity;
}

bool thumbnailAssetStamp(const std::filesystem::path& path, const std::string& assetId,
                         ThumbnailAssetStamp* out, std::string* error) {
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    if (code) {
        fail(error, "asset '" + assetId + "' cannot be stamped: " + code.message());
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail(error, "asset '" + assetId + "' cannot be opened for stamping");
        return false;
    }
    const std::uint64_t length = static_cast<std::uint64_t>(size);
    const std::uint64_t headLength = std::min<std::uint64_t>(length, kProbeBytes);
    const std::uint64_t tailLength = std::min<std::uint64_t>(length, kProbeBytes);
    std::string head;
    std::string tail;
    if (!readRange(file, 0, headLength, &head) ||
        !readRange(file, length - tailLength, tailLength, &tail)) {
        fail(error, "asset '" + assetId + "' cannot be read for stamping");
        return false;
    }
    std::string probeInput;
    probeInput.reserve(8 + head.size() + tail.size());
    littleEndian(length, &probeInput);
    probeInput.append(head);
    probeInput.append(tail);
    out->assetId = assetId;
    out->size = length;
    out->probe = sha256Hex(probeInput);
    return true;
}

bool normalizeSceneForFingerprint(std::string_view sceneJson, std::string* out,
                                  std::string* error) {
    boost::system::error_code parseError;
    boost::json::value parsed = boost::json::parse(sceneJson, parseError);
    if (parseError || !parsed.is_object()) {
        fail(error, "thumbnail fingerprint: the scene is not a JSON object");
        return false;
    }
    clearIdentityMembers(parsed);
    *out = boost::json::serialize(parsed);
    return true;
}

bool scenePersistable(std::string_view sceneJson) {
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(sceneJson, parseError);
    if (parseError || !parsed.is_object()) {
        return false;
    }
    const boost::json::object& root = parsed.as_object();
    const auto background = root.if_contains("background");
    if (background == nullptr || !background->is_object()) {
        return false;
    }
    const boost::json::object& layer = background->as_object();
    auto flag = [&layer](const char* key) {
        const auto member = layer.if_contains(key);
        return member != nullptr && member->is_bool() && member->as_bool();
    };
    return !flag("video") && !flag("camera");
}

std::string renderFingerprint(std::string_view sceneJson,
                              const std::vector<ThumbnailAssetStamp>& assets,
                              std::string_view renderer, std::string_view policy,
                              std::uint32_t width, std::uint32_t height) {
    std::string normalized;
    if (!normalizeSceneForFingerprint(sceneJson, &normalized, nullptr)) {
        return {};
    }
    std::string canonical;
    canonical.append("openshow-thumbnail-fingerprint/1\n");
    canonical.append("renderer\n");
    canonical.append(renderer.empty() ? rendererIdentity() : std::string(renderer));
    canonical.push_back('\n');
    canonical.append("policy\n");
    canonical.append(policy.empty() ? kThumbnailPolicy : std::string(policy));
    canonical.push_back('\n');
    canonical.append("extent\n");
    canonical.append(std::to_string(width));
    canonical.push_back('x');
    canonical.append(std::to_string(height));
    canonical.push_back('\n');
    canonical.append("scene\n");
    canonical.append(normalized);
    canonical.push_back('\n');
    canonical.append("assets\n");
    std::vector<const ThumbnailAssetStamp*> ordered;
    ordered.reserve(assets.size());
    for (const ThumbnailAssetStamp& asset : assets) {
        ordered.push_back(&asset);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ThumbnailAssetStamp* a, const ThumbnailAssetStamp* b) {
                  return a->assetId < b->assetId;
              });
    std::string previous;
    for (const ThumbnailAssetStamp* asset : ordered) {
        if (asset->assetId == previous) {
            continue;  // one asset named twice is one identity
        }
        previous = asset->assetId;
        canonical.append(asset->assetId);
        canonical.push_back('\x1f');
        canonical.append(std::to_string(asset->size));
        canonical.push_back('\x1f');
        canonical.append(asset->probe);
        canonical.push_back('\n');
    }
    return sha256Hex(canonical);
}

namespace {

// The pixel contract, shared by a service record and a Library variant. Only
// ownership differs: a variant names no show or slide, because the ids a
// prepared service will mint do not exist yet.
bool validateThumbnailPicture(const ThumbnailCacheEntry& entry, bool owned, std::string* error) {
    if (owned) {
        if (!entry.showId.empty() && !safeOpaqueId(entry.showId)) {
            fail(error, "thumbnail record: the owning show id is not a name");
            return false;
        }
        if (!safeOpaqueId(entry.slideId)) {
            fail(error, "thumbnail record: the owning slide id is not a name");
            return false;
        }
    } else if (!entry.showId.empty() || !entry.slideId.empty()) {
        fail(error, "thumbnail variant: a Library picture names no service owner");
        return false;
    }
    if (!isLowerHex(entry.fingerprint, 64)) {
        fail(error, "thumbnail record: the fingerprint is not 64 lowercase hex characters");
        return false;
    }
    if (entry.renderer.empty() || entry.renderer.size() > 64) {
        fail(error, "thumbnail record: the renderer identity is empty or longer than 64");
        return false;
    }
    for (char c : entry.renderer) {
        if (static_cast<unsigned char>(c) < 0x20) {
            fail(error, "thumbnail record: the renderer identity carries a control character");
            return false;
        }
    }
    if (entry.width < kMinThumbnailWidth || entry.width > kThumbnailWidth ||
        entry.height < kMinThumbnailHeight || entry.height > kThumbnailHeight) {
        fail(error, "thumbnail record: the picture dimensions are outside the accepted box");
        return false;
    }
    const double aspect = static_cast<double>(entry.width) / static_cast<double>(entry.height);
    const double wanted = static_cast<double>(kThumbnailWidth) / static_cast<double>(kThumbnailHeight);
    if (aspect < wanted * 0.98 || aspect > wanted * 1.02) {
        fail(error, "thumbnail record: the picture is not 16:9");
        return false;
    }
    if (entry.pngBytes == 0 || entry.pngBytes > kMaxThumbnailPngBytes) {
        fail(error, "thumbnail record: the declared PNG length is outside 1..32768");
        return false;
    }
    if (!isLowerHex(entry.checksum, 64)) {
        fail(error, "thumbnail record: the checksum is not 64 lowercase hex characters");
        return false;
    }
    std::size_t decoded = 0;
    if (!base64IsCanonical(entry.pngBase64, &decoded)) {
        fail(error, "thumbnail record: \"pngBase64\" is not canonical Base64");
        return false;
    }
    if (decoded != entry.pngBytes) {
        fail(error, "thumbnail record: \"pngBytes\" does not match the encoded picture");
        return false;
    }
    return true;
}

}  // namespace

bool validateThumbnailEntry(const ThumbnailCacheEntry& entry, std::string* error) {
    return validateThumbnailPicture(entry, true, error);
}

bool validThumbnailVariant(const ThumbnailCacheEntry& entry, std::string* error) {
    return validateThumbnailPicture(entry, false, error);
}

std::size_t sanitizeThumbnailVariants(std::vector<ThumbnailCacheEntry>* variants) {
    if (variants == nullptr) {
        return 0;
    }
    std::size_t removed = 0;
    std::vector<ThumbnailCacheEntry> kept;
    kept.reserve(std::min(variants->size(), kMaxThumbnailVariantsPerItem));
    std::uint64_t encoded = 0;
    std::set<std::string> seen;
    for (const ThumbnailCacheEntry& entry : *variants) {
        if (kept.size() >= kMaxThumbnailVariantsPerItem) {
            ++removed;
            continue;
        }
        if (!validThumbnailVariant(entry, nullptr)) {
            ++removed;
            continue;
        }
        if (!seen.insert(entry.fingerprint).second) {
            ++removed;  // one look's pixels have one home
            continue;
        }
        if (encoded + entry.pngBase64.size() > kMaxThumbnailVariantBytesPerItem) {
            ++removed;
            continue;
        }
        encoded += entry.pngBase64.size();
        kept.push_back(entry);
    }
    *variants = std::move(kept);
    return removed;
}

const ThumbnailCacheEntry* findThumbnailVariant(
    const std::vector<ThumbnailCacheEntry>& variants, std::string_view fingerprint) {
    for (const ThumbnailCacheEntry& entry : variants) {
        if (entry.fingerprint == fingerprint) {
            return &entry;
        }
    }
    return nullptr;
}

std::size_t sanitizeThumbnailCache(ThumbnailCache* cache) {
    if (cache == nullptr) {
        return 0;
    }
    std::size_t removed = 0;
    std::vector<ThumbnailCacheEntry> kept;
    kept.reserve(std::min(cache->entries.size(), kMaxThumbnailCacheEntries));
    std::uint64_t encoded = 0;
    std::set<std::pair<std::string, std::string>> seen;
    for (const ThumbnailCacheEntry& entry : cache->entries) {
        if (kept.size() >= kMaxThumbnailCacheEntries) {
            ++removed;
            continue;
        }
        if (!validateThumbnailEntry(entry, nullptr)) {
            ++removed;
            continue;
        }
        if (!seen.insert({entry.slideId, entry.fingerprint}).second) {
            ++removed;  // one slide's one picture has one home
            continue;
        }
        if (encoded + entry.pngBase64.size() > kMaxThumbnailEncodedBytes) {
            ++removed;
            continue;
        }
        encoded += entry.pngBase64.size();
        kept.push_back(entry);
    }
    cache->entries = std::move(kept);
    return removed;
}

bool validateThumbnailCache(const ThumbnailCache& cache, std::string* error) {
    if (cache.format != kThumbnailCacheFormat) {
        fail(error, "thumbnail cache: unsupported format " + std::to_string(cache.format));
        return false;
    }
    if (cache.entries.size() > kMaxThumbnailCacheEntries) {
        fail(error, "thumbnail cache: " + std::to_string(cache.entries.size()) +
                        " records exceed the " +
                        std::to_string(kMaxThumbnailCacheEntries) + "-record bound");
        return false;
    }
    std::uint64_t encoded = 0;
    std::set<std::pair<std::string, std::string>> seen;
    for (const ThumbnailCacheEntry& entry : cache.entries) {
        if (!validateThumbnailEntry(entry, error)) {
            return false;
        }
        if (!seen.insert({entry.slideId, entry.fingerprint}).second) {
            fail(error, "thumbnail cache: two records claim one slide's one picture");
            return false;
        }
        encoded += entry.pngBase64.size();
        if (encoded > kMaxThumbnailEncodedBytes) {
            fail(error, "thumbnail cache: the encoded pictures exceed the 1.5 MiB budget");
            return false;
        }
    }
    return true;
}

const ThumbnailCacheEntry* findThumbnail(const ThumbnailCache& cache, std::string_view slideId,
                                         std::string_view fingerprint) {
    for (const ThumbnailCacheEntry& entry : cache.entries) {
        if (entry.slideId == slideId && entry.fingerprint == fingerprint) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace openshow::domain
