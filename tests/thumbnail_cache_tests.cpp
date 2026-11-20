/*
 * TC.2/TC.4: the Qt-free cache record contract.
 *
 * The fingerprint is what decides whether a stored card still describes the
 * scene it is listed under, so these checks pin its inputs one at a time:
 * moving a package changes no identity, a different byte of a referenced asset
 * changes it, a text change changes it, and repeated occurrences share it.
 * The hash, Base64 and PNG-header helpers are checked against fixed vectors so
 * an accidental "compatible" rewrite of any of them fails here rather than in
 * a user's cache.
 */

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "openshow/domain/thumbnails.h"

namespace {

namespace fs = std::filesystem;
using openshow::domain::ThumbnailAssetStamp;
using openshow::domain::ThumbnailCache;
using openshow::domain::ThumbnailCacheEntry;
using openshow::domain::sha256Hex;

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

std::string sceneWith(const std::string& words, const std::string& assetPath,
                      const std::string& background = "flat") {
    std::string json = R"({"format":1,"background":{"transparent":false,"image":)";
    json += background == "flat" ? "false" : "true";
    json += R"(,"video":)";
    json += background == "video" ? "true" : "false";
    json += R"(,"camera":false,"fit":"cover","assetId":)";
    json += background == "image" || background == "video" ? "\"asset1\"" : "\"\"";
    json += R"(,"assetPath":")";
    json += assetPath;
    json += R"("},"items":[{"kind":"text","lines":[")";
    json += words;
    json += R"("],"color":4294967295,"fontFamily":"Inter","fontSize":64,"x":100,"y":90,"width":1720,"height":900,"align":"center"}]})";
    return json;
}

void exerciseHashes() {
    using openshow::domain::sha256Hex;
    check(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 of nothing is the empty vector");
    check(sha256Hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 of abc");
    std::string million(1000000, 'a');
    check(sha256Hex(million) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256 crosses its padding block correctly");
    const std::string bytes("\x00\x01\x02\xff", 4);
    check(sha256Hex(bytes) == sha256Hex(bytes), "sha256 is stable");
    check(openshow::domain::pngChecksum(bytes) == sha256Hex(bytes),
          "the PNG checksum is the PNG bytes' digest");
}

void exerciseBase64() {
    using openshow::domain::base64Decode;
    using openshow::domain::base64Encode;
    using openshow::domain::base64EncodedSize;
    using openshow::domain::base64IsCanonical;

    check(base64Encode("Man") == "TWFu", "base64 of Man");
    check(base64Encode("Ma") == "TWE=", "base64 pads two bytes");
    check(base64Encode("M") == "TQ==", "base64 pads one byte");
    check(base64Encode("") == "", "base64 of nothing is nothing");
    for (std::size_t length = 0; length < 300; ++length) {
        std::string raw;
        for (std::size_t i = 0; i < length; ++i) {
            raw.push_back(static_cast<char>(i * 31 + 7));
        }
        const std::string encoded = base64Encode(raw);
        check(encoded.size() == base64EncodedSize(length), "the encoded length is predictable");
        std::size_t decoded = 0;
        check(base64IsCanonical(encoded, &decoded), "the encoder's spelling is canonical");
        check(decoded == length, "the canonical length is the raw length");
        std::string back;
        check(base64Decode(encoded, &back) && back == raw, "base64 round-trips every length");
    }
    std::string out;
    check(!base64Decode("TWFu\n", &out), "a line break is not accepted");
    check(!base64Decode("TWE", &out), "an unpadded final quantum is refused");
    check(!base64Decode("TQ=x", &out), "padding in the middle is refused");
    check(!base64Decode("TR==", &out), "non-zero padding bits are refused");
    check(!base64Decode("_-_-", &out), "the URL alphabet is refused");
    check(base64Decode("TWFu", &out) && out == "Man", "a plain quantum decodes");
}

void exercisePngHeader() {
    using openshow::domain::pngDimensions;
    const unsigned char header[24] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
                                      0x00, 0x00, 0x00, 0x0d, 'I',  'H',  'D',  'R',
                                      0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x20};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    check(pngDimensions(std::string_view(reinterpret_cast<const char*>(header), sizeof(header)),
                        &width, &height),
          "a PNG header is recognised");
    check(width == 512 && height == 288, "the header's dimensions are read");
    std::string jpeg("\xff\xd8\xff\xe0\x00\x10JFIF", 11);
    check(!pngDimensions(jpeg, &width, &height), "JPEG bytes are not a PNG");
    std::string shortPng("\x89PNG\r\n\x1a\n", 8);
    check(!pngDimensions(shortPng, &width, &height), "a truncated header is refused");
}

void exerciseFingerprint() {
    using openshow::domain::normalizeSceneForFingerprint;
    using openshow::domain::rendererIdentity;
    using openshow::domain::renderFingerprint;
    using openshow::domain::scenePersistable;

    const std::string sceneA = sceneWith("Amazing grace", "/home/a/service/assets/a1-hymn.jpg");
    const std::string sceneB = sceneWith("Amazing grace", "/srv/moved/assets/a1-hymn.jpg");
    const std::vector<ThumbnailAssetStamp> stamp{{"asset1", 4096, sha256Hex("asset bytes")}};

    const std::string base = renderFingerprint(sceneA, stamp);
    check(base.size() == 64, "a fingerprint is a 64-character digest");
    check(renderFingerprint(sceneB, stamp) == base,
          "the package's location does not enter the fingerprint");
    check(renderFingerprint(sceneA, {}) != base,
          "a referenced asset's identity is part of the fingerprint");
    check(renderFingerprint(sceneA, {{"asset1", 4097, stamp[0].probe}}) != base,
          "an asset's size is part of the fingerprint");
    check(renderFingerprint(sceneA, {{"asset1", 4096, sha256Hex("replaced")}}) != base,
          "an asset's content is part of the fingerprint");
    check(renderFingerprint(sceneWith("Amazing love", "/home/a/service/assets/a1-hymn.jpg"),
                            stamp) != base,
          "changed words miss");
    check(renderFingerprint(sceneA, stamp, "openshow-scene-card/2;app=9.9") != base,
          "a different renderer misses");
    check(renderFingerprint(sceneA, stamp, {}, "card-surface/2") != base,
          "a different thumbnail policy misses");
    check(renderFingerprint(sceneA, stamp, {}, {}, 256, 144) != base,
          "a different bucket misses");
    check(renderFingerprint(sceneA, stamp) == base, "the same scene hits again");
    check(renderFingerprint(sceneA, stamp, rendererIdentity()) == base,
          "the default renderer identity is the named one");

    // An occurrence listed twice compiles to the same payload and therefore
    // shares one record; order of the stamp list must not matter.
    std::vector<ThumbnailAssetStamp> reversed{stamp[0], {"asset2", 7, sha256Hex("x")}};
    std::vector<ThumbnailAssetStamp> forward{reversed[1], reversed[0]};
    check(renderFingerprint(sceneA, reversed) == renderFingerprint(sceneA, forward),
          "asset order does not change the fingerprint");
    check(renderFingerprint("not a scene", stamp).empty(), "an unreadable scene has no fingerprint");

    std::string normalized;
    check(normalizeSceneForFingerprint(sceneA, &normalized, nullptr),
          "a compiled scene normalizes");
    check(normalized.find("/home/a") == std::string::npos,
          "the normalized scene carries no asset path");
    check(normalized.find("\"assetPath\":\"\"") != std::string::npos,
          "the asset path member stays, empty");

    check(scenePersistable(sceneA), "a static card may be kept");
    check(!scenePersistable(sceneWith("x", "/a/v.mp4", "video")),
          "a video background is never persisted");
    check(!scenePersistable(sceneWith("x", "ndi://cam", "video")),
          "live content is never persisted");
    check(!scenePersistable("nonsense"), "an unreadable scene is not persisted");
}

void exerciseAssetStamp(const fs::path& root) {
    using openshow::domain::thumbnailAssetStamp;
    fs::create_directories(root);
    const fs::path original = root / "a1-hymn.jpg";
    std::string bytes(20000, 'q');
    bytes[19999] = 'z';
    {
        std::ofstream out(original, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ThumbnailAssetStamp first;
    std::string error;
    check(thumbnailAssetStamp(original, "asset1", &first, &error), "an asset stamps");
    check(first.size == bytes.size() && first.probe.size() == 64, "the stamp carries size and probe");

    // A save that adopts copies the same bytes to a new name: identity is
    // content, so the copy must stamp identically even though its mtime is new.
    const fs::path copied = root / "service-assets" / "a1-hymn.jpg";
    fs::create_directories(copied.parent_path());
    fs::copy_file(original, copied, fs::copy_options::overwrite_existing);
    ThumbnailAssetStamp second;
    check(thumbnailAssetStamp(copied, "asset1", &second, &error), "the copy stamps");
    check(second.size == first.size && second.probe == first.probe,
          "a byte-identical copy keeps the asset's identity");

    // Replacing the file in place, even at the same size, is a miss.
    std::string replaced = bytes;
    replaced[19999] = 'y';
    {
        std::ofstream out(original, std::ios::binary | std::ios::trunc);
        out.write(replaced.data(), static_cast<std::streamsize>(replaced.size()));
    }
    ThumbnailAssetStamp third;
    check(thumbnailAssetStamp(original, "asset1", &third, &error), "the replacement stamps");
    check(third.probe != first.probe, "changed bytes change the asset's identity");

    ThumbnailAssetStamp missing;
    check(!thumbnailAssetStamp(root / "absent.jpg", "asset9", &missing, &error),
          "a missing asset cannot be stamped");
    check(!error.empty(), "the refusal names its reason");
}

ThumbnailCacheEntry goodEntry() {
    ThumbnailCacheEntry entry;
    entry.showId = "show3";
    entry.slideId = "slide17";
    entry.fingerprint = sha256Hex("fingerprint");
    entry.renderer = openshow::domain::rendererIdentity();
    entry.width = openshow::domain::kThumbnailWidth;
    entry.height = openshow::domain::kThumbnailHeight;
    std::string png("not really a png, but bounded: ");
    png.append(100, 'p');
    entry.pngBytes = png.size();
    entry.checksum = sha256Hex(png);
    entry.pngBase64 = openshow::domain::base64Encode(png);
    return entry;
}

void exerciseValidation() {
    using openshow::domain::findThumbnail;
    using openshow::domain::sanitizeThumbnailCache;
    using openshow::domain::validateThumbnailCache;
    using openshow::domain::validateThumbnailEntry;

    std::string error;
    ThumbnailCacheEntry entry = goodEntry();
    check(validateThumbnailEntry(entry, &error), "a well-formed record passes");

    ThumbnailCacheEntry bad = entry;
    bad.width = 1024;
    check(!validateThumbnailEntry(bad, &error), "an oversized picture is refused");
    bad = entry;
    bad.height = 144;  // 512x144 is not 16:9
    check(!validateThumbnailEntry(bad, &error), "a non-16:9 picture is refused");
    bad = entry;
    bad.fingerprint = "ABC";
    check(!validateThumbnailEntry(bad, &error), "a short fingerprint is refused");
    bad = entry;
    bad.slideId = "../escape";
    check(!validateThumbnailEntry(bad, &error), "an id that is a path is refused");
    bad = entry;
    bad.pngBytes = openshow::domain::kMaxThumbnailPngBytes + 1;
    check(!validateThumbnailEntry(bad, &error), "a declared PNG over 32 KiB is refused");
    bad = entry;
    bad.pngBase64 = entry.pngBase64 + "\n";
    check(!validateThumbnailEntry(bad, &error), "a Base64 line break is refused");
    bad = entry;
    bad.pngBytes = entry.pngBytes - 1;
    check(!validateThumbnailEntry(bad, &error), "a length that disagrees with the picture is refused");
    bad = entry;
    bad.renderer.clear();
    check(!validateThumbnailEntry(bad, &error), "an empty renderer identity is refused");

    ThumbnailCache cache;
    cache.entries.push_back(entry);
    cache.entries.push_back(entry);  // same slide and fingerprint twice
    check(!validateThumbnailCache(cache, &error), "one slide's one picture has one home");

    ThumbnailCache clean;
    clean.entries.push_back(entry);
    check(validateThumbnailCache(clean, &error), "a valid cache passes");
    check(findThumbnail(clean, "slide17", entry.fingerprint) == &clean.entries.front(),
          "lookup finds the record by slide and fingerprint");
    check(findThumbnail(clean, "slide18", entry.fingerprint) == nullptr,
          "another slide has no picture");
    check(findThumbnail(clean, "slide17", sha256Hex("other")) == nullptr,
          "another fingerprint has no picture");

    ThumbnailCache doomed;
    doomed.entries.push_back(entry);
    ThumbnailCacheEntry broken = entry;
    broken.slideId = "slide18";
    broken.pngBase64 = "not base64!";
    doomed.entries.push_back(broken);
    ThumbnailCacheEntry duplicate = entry;
    doomed.entries.push_back(duplicate);
    check(sanitizeThumbnailCache(&doomed) == 2, "sanitize drops the broken and the duplicate");
    check(doomed.entries.size() == 1 && doomed.entries.front().slideId == "slide17",
          "sanitize keeps the valid record in file order");

    doomed.entries.clear();
    for (std::size_t i = 0; i < openshow::domain::kMaxThumbnailCacheEntries + 5; ++i) {
        ThumbnailCacheEntry extra = entry;
        extra.slideId = "slide" + std::to_string(1000 + i);
        doomed.entries.push_back(extra);
    }
    check(sanitizeThumbnailCache(&doomed) == 5, "sanitize enforces the record bound");
    check(doomed.entries.size() == openshow::domain::kMaxThumbnailCacheEntries,
          "the earliest records survive the count bound");

    doomed.entries.clear();
    ThumbnailCacheEntry big = goodEntry();
    std::string bulk(24 * 1024, 'b');
    big.pngBytes = bulk.size();
    big.checksum = sha256Hex(bulk);
    big.pngBase64 = openshow::domain::base64Encode(bulk);
    const std::size_t perEntry = big.pngBase64.size();
    const std::size_t fit = openshow::domain::kMaxThumbnailEncodedBytes / perEntry;
    for (std::size_t i = 0; i < fit + 3; ++i) {
        ThumbnailCacheEntry extra = big;
        extra.slideId = "slide" + std::to_string(2000 + i);
        doomed.entries.push_back(extra);
    }
    check(sanitizeThumbnailCache(&doomed) == 3, "sanitize enforces the encoded-byte budget");
    check(doomed.entries.size() == fit, "only records that fit the budget are kept");
}

}  // namespace

int main() {
    exerciseHashes();
    exerciseBase64();
    exercisePngHeader();
    exerciseFingerprint();
    const fs::path root = fs::temp_directory_path() /
                          ("openshow-thumbnail-tests-" + std::to_string(getpid()));
    fs::remove_all(root);
    exerciseAssetStamp(root / "assets");
    fs::remove_all(root);
    exerciseValidation();

    if (gFailed) {
        return 1;
    }
    std::printf("ok: thumbnail hashes, Base64, fingerprints, asset stamps and bounds\n");
    return 0;
}
