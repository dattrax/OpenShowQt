/*
 * Task 6.10 (review R12): the decoded scene-asset cache — its budget, its
 * pins, its limits, and the rule that a lookup never touches the disk.
 *
 * The review's finding was memory that only grows: every path ever seen
 * retained forever, failures cached permanently, and no bound a hundred
 * distinct 4K backgrounds could not run through. The counters below are the
 * evidence, and they are stated in ABSOLUTE units on purpose (task 6.8b's
 * lesson): a fixture that measures the budget against the constant under
 * test passes whatever that budget is raised to, which is precisely how a
 * cache "test" survives a cache with no limit at all.
 *
 * A churn image here is 768x768 N32 = 2.25 MiB decoded: 100 of them are
 * 225 MiB asked of the 192 MiB ceiling, so the plateau is real, and the
 * whole
 * file still runs in seconds — the flood shares ONE encoded JPEG across
 * its paths (the cache is keyed by path, and re-encoding flat colour a
 * hundred times would only spend the fixture's time). The last scenario
 * checks a full 3840x2160 still at exactly the review's number.
 *
 * Stills are JPEG because that is what this Skia build decodes; the
 * fixture generates them with ffmpeg and exits 77 (skip, never silent
 * pass) when the tool is absent.
 */

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "render/SceneText.h"

#include "include/core/SkImage.h"

namespace {

using openshow::worker::SceneImageCache;

// The cache's own numbers, written out. testTheBoundsAreTheOnesTheCodeClaims
// ties these to the code; every other scenario judges against THESE.
constexpr std::size_t kBudget = 192u * 1024u * 1024u;
constexpr std::uint64_t kMaxFile = 64u * 1024u * 1024u;
constexpr std::int32_t kMaxDimension = 8192;

// One churn image's decoded bytes, and how many of them overflow the
// budget: 768x768 N32 is 2.25 MiB, so a hundred of them are 225 MiB asked
// of a 192 MiB ceiling. (512 was the first try and it is 1 MiB — a flood
// of 100 would sit UNDER the budget and every eviction check would pass
// vacuously; the arithmetic of the fixture is part of what it claims.)
constexpr int kChurnSide = 768;
constexpr std::size_t kChurnBytes =
    static_cast<std::size_t>(kChurnSide) * kChurnSide * 4u;  // 2.25 MiB, N32
constexpr int kFlood = 100;                                   // 225 MiB

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::string fixtureDir() {
    const std::string dir =
        "/tmp/opencode/scene-image-cache-" + std::to_string(static_cast<int>(getpid()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool generate(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

std::string readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string writeFixture(const std::string& dir, const std::string& name,
                         const std::string& bytes) {
    const std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

// The centre pixel of a decoded image as N32, for the "is this still the
// picture" checks: a surface of exactly the right size that was never drawn
// into is the characteristic failure of a resample, and dimensions alone let
// it through. Zero means the read itself failed.
std::uint32_t centrePixel(const sk_sp<SkImage>& image) {
    if (image == nullptr) {
        return 0;
    }
    const SkImageInfo info = SkImageInfo::MakeN32Premul(image->width(), image->height());
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(image->width()) *
                                      static_cast<std::size_t>(image->height()));
    if (!image->readPixels(info, pixels.data(), info.minRowBytes(), 0, 0)) {
        return 0;
    }
    return pixels[static_cast<std::size_t>(image->height()) / 2 *
                      static_cast<std::size_t>(image->width()) +
                  static_cast<std::size_t>(image->width()) / 2];
}

// Flat-colour PNG at an arbitrary size. Needed because PNG is the codec that
// cannot scale at all: it is the fixture that proves a size cap is enforced
// rather than requested.
bool makePng(const std::string& path, int width, int height, const char* colour) {
    const std::string command =
        "ffmpeg -v error -y -f lavfi -i color=c=" + std::string(colour) + ":s=" +
        std::to_string(width) + "x" + std::to_string(height) + ":r=1 -frames:v 1 '" + path +
        "' >/dev/null 2>&1";
    return generate(command) && !readFileBytes(path).empty();
}

// Flat-colour JPEG at an arbitrary size (a solid field encodes tiny even at
// absurd dimensions, which is exactly what the dimension gate is about).
bool makeJpeg(const std::string& path, int width, int height, const char* colour, int quality) {
    const std::string command =
        "ffmpeg -v error -y -f lavfi -i color=c=" + std::string(colour) + ":s=" +
        std::to_string(width) + "x" + std::to_string(height) + ":r=1 -frames:v 1 -q:v " +
        std::to_string(quality) + " '" + path + "' >/dev/null 2>&1";
    return generate(command) && !readFileBytes(path).empty();
}

// --- the bounds themselves ----------------------------------------------

void testTheBoundsAreTheOnesTheCodeClaims() {
    check(SceneImageCache::kDecodedImageBudget == kBudget,
          "the decoded-image budget is 192 MiB, written out here and not next to it");
    check(SceneImageCache::kMaxEncodedFileBytes == kMaxFile,
          "the encoded asset file limit is 64 MiB");
    check(SceneImageCache::kMaxImageDimension == kMaxDimension,
          "the declared-dimension limit is 8192 a side");
}

// --- budget: accounting, plateau, and what eviction may touch -----------

void testBudgetAndPlateau(const std::string& churnJpeg) {
    const std::string dir = fixtureDir();
    SceneImageCache cache;
    check(cache.stats().decodedBytes == 0 && cache.stats().entries == 0,
          "a fresh cache owes no memory");

    // Check the accounting on the FIRST load, before any eviction muddies it.
    const std::string first = writeFixture(dir, "a.jpg", churnJpeg);
    check(cache.loadFor(first) != nullptr, "a real JPEG loads");
    check(cache.stats().decodedBytes == kChurnBytes,
          "the entry accounts for its decoded bytes: 768x768 N32 is 2.25 MiB");

    // Churn distinct PATHS past the budget: kFlood x 2 MiB = 200 MiB asked
    // of a 192 MiB ceiling. This is the review's scenario at fixture scale —
    // a show cycling more backgrounds than memory can hold.
    for (int i = 0; i < kFlood; ++i) {
        const std::string name = "churn-" + std::to_string(i) + ".jpg";
        const std::string path = writeFixture(dir, name, churnJpeg);
        cache.loadFor(path);
    }
    const auto stats = cache.stats();
    check(stats.decodedBytes <= kBudget,
          "100 distinct 2.25 MiB images (225 MiB of decoded pixels) plateau at the 192 MiB "
          "budget — the review's unbounded growth cannot happen");
    check(stats.evictions > 0,
          "and the cache says plainly that it evicted, rather than growing quietly");
    check(cache.has(dir + "/churn-" + std::to_string(kFlood - 1) + ".jpg"),
          "the newest churn is still cached: recency is the policy");
    std::filesystem::remove_all(dir);
}

// --- pins: the operator's content outlives the churn ---------------------

void testPinsSurviveChurnAndReleaseRejoins(const std::string& churnJpeg) {
    const std::string dir = fixtureDir();
    SceneImageCache cache;
    const std::string held = writeFixture(dir, "held.jpg", churnJpeg);
    // The session pins through syncPinSet alone — the whole pin set re-
    // derived each time — and so does this fixture: mixing hand pins with
    // set syncs would count references from two owners and test neither.
    cache.syncPinSet({held});  // a pin may precede the load, as prepare precedes prefetch
    check(cache.loadFor(held) != nullptr, "the pinned image loads");
    check(cache.stats().pins == 1, "the prepared scene's pin is visible in the counters");

    const std::string other = writeFixture(dir, "other.jpg", churnJpeg);
    for (int i = 0; i < kFlood; ++i) {
        const std::string name = "flood-" + std::to_string(i) + ".jpg";
        cache.loadFor(writeFixture(dir, name, churnJpeg));
    }
    // Load the not-yet-pinned one LAST so what follows cannot evict it for
    // age: the survivor below must survive because it is PINNED, and the
    // release below must make it fall because it is not.
    check(cache.loadFor(other) != nullptr, "the unpinned image loads, most recent of all");
    cache.syncPinSet({held, other});  // hold both through the second flood
    for (int i = 0; i < kFlood; ++i) {
        const std::string name = "flood2-" + std::to_string(i) + ".jpg";
        cache.loadFor(writeFixture(dir, name, churnJpeg));
    }
    check(cache.has(held) && cache.has(other),
          "both pinned images survive 200 distinct churns — a prepared scene "
          "presents its picture, not a colour fill, however hard the show cycles");

    // syncPinSet: re-deriving the set WITHOUT `held` releases it — and the
    // next load over the line takes it, because a path the session just
    // let go is now the oldest eligible entry. (The release alone would
    // not: at 85 churns of 2.25 MiB the cache sits just UNDER the budget,
    // and eviction is a response to exceeding it, not to idleness.)
    cache.syncPinSet({other});
    check(cache.stats().pins == 1, "the re-derived set holds exactly its one pin");
    cache.loadFor(writeFixture(dir, "last.jpg", churnJpeg));
    check(!cache.has(held),
          "a path the session no longer names is released and evicted: churned shows "
          "plateau because the store's castoffs become eligible, not because pins leak");
    check(cache.has(other), "the newly named path keeps its picture");
    std::filesystem::remove_all(dir);
}

// --- held content BIGGER than the budget: a sync is not an eviction -----
//
// The production case is sixteen prepared 4K stills (16 x 31.6 MiB is
// 505 MiB against a 192 MiB ceiling): the pin set itself sits above the
// budget, and that excess is exactly what the budget is told to tolerate.
// Re-deriving the SAME set must therefore change nothing that is resident.
// An evictor that runs while every pin is down (the release half of a sync
// settling the budget before the survivors are re-pinned) forgets the
// OLDEST entries first — which are the prepared scenes — and turns them
// into zero-byte placeholders: the operator's scene presents a colour fill
// mid-show, on a worker that answered every command successfully.
void testPinnedSetLargerThanBudgetSurvivesSync(const std::string& churnJpeg) {
    const std::string dir = fixtureDir();
    SceneImageCache cache;
    std::vector<std::string> held;
    for (int i = 0; i < 90; ++i) {
        held.push_back(writeFixture(dir, "keep-" + std::to_string(i) + ".jpg", churnJpeg));
    }
    cache.syncPinSet(held);  // pin-before-load, exactly as prepare does
    for (const std::string& path : held) {
        cache.loadFor(path);
    }
    check(cache.stats().decodedBytes == 90 * kChurnBytes &&
              cache.stats().decodedBytes > kBudget,
          "the held set is deliberately bigger than the budget (90 x 2.25 MiB = 202.5 MiB "
          "of pins against 192 MiB) — the sixteen-prepared-4K-stills case at fixture scale");
    for (int i = 0; i < 20; ++i) {
        cache.loadFor(writeFixture(dir, "extra-" + std::to_string(i) + ".jpg", churnJpeg));
    }
    cache.syncPinSet(held);  // the same set again: nothing was let go
    std::size_t survivors = 0;
    for (const std::string& path : held) {
        if (cache.has(path)) {
            ++survivors;
        }
    }
    check(survivors == held.size(),
          "re-deriving the same over-budget set evicts NOTHING: the pins are the reported "
          "excess, not the next sync's victims");
    check(cache.stats().decodedBytes == 90 * kChurnBytes,
          "and the accounted bytes are exactly the pinned content, neither the churned "
          "surplus above it nor a prepared picture short");
    check(cache.stats().pins == 90, "one reference per held path, not two after a repeat sync");
    check(!cache.has(dir + "/extra-0.jpg"),
          "what DID pay for the budget is the unpinned surplus, as it should be");
    std::filesystem::remove_all(dir);
}

// --- failures: cached for the draw path, retried by the operator ---------

void testFailuresArePlaceholdersNotVerdicts(const std::string& churnJpeg) {
    const std::string dir = fixtureDir();
    SceneImageCache cache;
    const std::string path = dir + "/later.jpg";
    ::unlink(path.c_str());

    std::string error;
    check(cache.refreshFor(path, &error) == nullptr && !error.empty(),
          "a missing file fails with a reason");
    check(cache.imageFor(path) == nullptr && cache.stats().decodedBytes == 0,
          "the failure is a zero-byte placeholder: it weighs nothing, and imageFor "
          "never retries the disk on its behalf");
    check(cache.stats().entries == 1, "the remembered failure is still an entry");

    // The draw path must not be the one that discovers the repair.
    writeFixture(dir, "later.jpg", churnJpeg);
    check(cache.imageFor(path) == nullptr,
          "after the file appears, imageFor STILL answers null — the draw path never "
          "touches the disk, which is the no-disk-in-draw rule");
    check(cache.refreshFor(path, &error) != nullptr && error.empty(),
          "a prepare (refreshFor) retries the cached failure: the file the operator put "
          "back comes back");
    check(cache.has(path) && cache.stats().decodedBytes == kChurnBytes,
          "and the draw path picks the picture up from then on");

    // The inverse direction of the same rule. A CACHED picture must not
    // care what its file does: delete it, and the lookup still answers the
    // image — proof that nothing in the draw path opens a descriptor even
    // when it has an answer to give.
    writeFixture(dir, "later.jpg", churnJpeg);
    check(cache.refreshFor(path, &error) != nullptr, "the picture is loaded again");
    ::unlink(path.c_str());
    check(cache.imageFor(path) != nullptr,
          "a cached still survives the deletion of its file: the lookup never re-reads, "
          "so a show's pictures do not depend on the disk mid-frame");

    // Explicit invalidation forgets the success too — the asset-repair hook.
    cache.invalidate(path);
    check(!cache.has(path) && cache.stats().entries == 0 && cache.stats().decodedBytes == 0,
          "invalidate() drops the bytes, the entry, and (here) the path entirely");

    // A failure placeholder is no permanent anchor either: unpinned, the
    // flood evicts it like any other entry.
    cache.loadFor(dir + "/missing-2.jpg");
    const std::size_t evictionsBefore = cache.stats().evictions;
    for (int i = 0; i < kFlood; ++i) {
        const std::string name = "f3-" + std::to_string(i) + ".jpg";
        cache.loadFor(writeFixture(dir, name, churnJpeg));
    }
    check(cache.stats().decodedBytes <= kBudget,
          "the flood plateaus at the budget with a failure placeholder in the map");
    check(cache.stats().evictions > evictionsBefore,
          "and eviction passed the placeholder by, rather than the map growing on it");
    std::filesystem::remove_all(dir);
}

// --- limits: refused on their numbers, before any pixels -----------------

void testLimitsRefuseBeforeAllocating() {
    const std::string dir = fixtureDir();
    SceneImageCache cache;

    // A 9000x9000 header: a few hundred KB of JPEG whose pixels would be
    // 324 MB. Refusal must come from the declared numbers, before pixels.
    const std::string huge = dir + "/huge.jpg";
    if (!makeJpeg(huge, 9000, 9000, "blue", 30)) {
        check(false, "the 9000x9000 fixture could not be staged");
        return;
    }
    std::string error;
    check(cache.refreshFor(huge, &error) == nullptr,
          "a 9000x9000 image is refused outright");
    check(error.find("dimension") != std::string::npos,
          "and the error names the dimension limit as the reason");
    struct stat st {};
    ::stat(huge.c_str(), &st);
    check(st.st_size < 2 * 1024 * 1024,
          "the refused file is small against its 324 MB of declared pixels: the gate "
          "judged the header, not an allocation");

    // Inside the dimension gate but past the pixel ceiling: 7000x6000 is
    // 42 MP of N32 (168 MB) at extents the dimension gate cannot see. The
    // decoded-pixel ceiling is the only thing standing between it and the
    // allocator — without this case, deleting the megapixel gate would
    // survive every other check here (the 9000x9000 file falls to the
    // dimension gate first).
    const std::string wide = dir + "/wide.jpg";
    if (makeJpeg(wide, 7000, 6000, "green", 30)) {
        check(cache.refreshFor(wide, &error) == nullptr &&
                  error.find("megapixel") != std::string::npos,
              "a 7000x6000 image (42 MP, inside the dimension limit) is refused by the "
              "decoded-pixel ceiling, and the error names it");
    } else {
        check(false, "the 7000x6000 fixture could not be staged");
    }

    // An over-limit FILE: sparse, so staging 65 MiB costs a few real
    // blocks, and the refusal must come from the stat before the read.
    const std::string fat = dir + "/fat.jpg";
    {
        std::ofstream out(fat, std::ios::binary);
        out << "not really an image at all";
        out.close();
    }
    if (::truncate(fat.c_str(), static_cast<off_t>(kMaxFile + 1024)) == 0) {
        check(cache.refreshFor(fat, &error) == nullptr &&
                  error.find("file limit") != std::string::npos,
              "a file past 64 MiB is refused by its stat before it is read");
    } else {
        check(false, "the fixture could not stage an over-limit file");
    }

    // Under the limits, ordinary sizes decode: the gates are ceilings, not
    // a size policy — a 4K still at 3840x2160 is 8.3 MP, inside both.
    const std::string ok = dir + "/ok.jpg";
    if (!makeJpeg(ok, 3840, 2160, "red", 10)) {
        check(false, "the 4K fixture could not be staged");
    } else {
        check(cache.refreshFor(ok, &error) != nullptr,
              "a 3840x2160 still is comfortably inside the limits");
        check(cache.stats().decodedBytes == 3840u * 2160u * 4u,
              "one 4K still accounts for 33.2 MiB — the review's own number");
    }
    std::filesystem::remove_all(dir);
}

// --- a SECOND pool, and the scaled decode that feeds it (task 6.16b) -----
//
// The slide list draws with its own renderer, and the claim that makes that
// safe rather than merely redundant is that its picture memory is a different,
// bounded number from the show's. Two things had to be built for that claim: a
// cache that holds the line at a budget it was GIVEN, and a decode that hands
// it pixels small enough for the budget to mean anything. Neither shows in the
// scenarios above, which all run at the default 192 MiB.
//
// The arithmetic is deliberately small, and stated rather than measured: a
// 12 MiB ceiling (the one the session builds a list's cache with), a 512 KiB
// one for the case where a band wants more than it has, and a 1600x900 source
// asked for at 160x90. Waiting for a real list to scroll through 192 MiB of
// backdrops would take a hundred decodes to reach eviction once, and a fixture
// that cannot say WHICH budget a counter belongs to proves nothing about
// either.
void testASecondBudgetAndTheScaledDecode() {
    const std::string dir = fixtureDir();
    constexpr std::size_t kListBudget = 12u * 1024u * 1024u;
    constexpr std::size_t kTightBudget = 512u * 1024u;
    // The whole batch one ask may name (preview_wire.h's kMaxThumbnailBatch,
    // written out here the way the budgets above are).
    constexpr int kBatch = 32;

    SceneImageCache show;
    SceneImageCache list(kListBudget);
    check(show.stats().budget == kBudget,
          "the default cache reports the 192 MiB ceiling it was built with");
    check(list.stats().budget == kListBudget,
          "a cache built with a budget of its own reports THAT, not the class default — the "
          "two pools are two, and status says so on the wire");

    // A 1600x900 backdrop: 5.76 MB decoded, the size a real photographic
    // backdrop is. Asked for at 160x90 it must come back at row size or the
    // ceilings above are a rumour.
    const std::string big = dir + "/backdrop.jpg";
    if (!makeJpeg(big, 1600, 900, "0xE03030", 10)) {
        check(false, "the 1600x900 fixture could not be staged");
        return;
    }
    std::string error;
    const sk_sp<SkImage> native = SceneImageCache::decodeAtMost(big, 0, 0, &error);
    check(native != nullptr && native->width() == 1600 && native->height() == 900,
          "a cap of 0 means native: the show's own decode is unchanged by this task");
    const sk_sp<SkImage> scaled = SceneImageCache::decodeAtMost(big, 160, 90, &error);
    check(scaled != nullptr, "the same file decodes inside a 160x90 box");
    if (scaled != nullptr) {
        check(scaled->width() <= 160 && scaled->height() <= 90,
              "and the result is inside the box, whatever the codec's own steps — a PNG cannot "
              "scale at all, so the box is finished off after the decode, not suggested to it");
        const std::size_t scaledBytes = static_cast<std::size_t>(scaled->width()) *
                                        static_cast<std::size_t>(scaled->height()) * 4u;
        const std::size_t nativeBytes =
            static_cast<std::size_t>(native->width()) * static_cast<std::size_t>(native->height()) * 4u;
        check(scaledBytes <= 60u * 1024u && scaledBytes * 32u < nativeBytes,
              "one scaled backdrop costs a few tens of kilobytes where the native decode costs "
              "megabytes: that ratio is what makes thirty rows affordable");
        // The picture has to still BE the picture. A resample into the wrong
        // surface, or one that forgets to draw, is exactly the right size and
        // wrong.
        const std::uint32_t centre = centrePixel(scaled);
        const int red = static_cast<int>((centre >> 16) & 0xFF);
        const int green = static_cast<int>((centre >> 8) & 0xFF);
        const int blue = static_cast<int>(centre & 0xFF);
        check(red > 0xA0 && green < 0x70 && blue < 0x70,
              "the scaled pixels are the backdrop's own colour, not an empty surface");
    }

    // THE CODEC THAT CANNOT SCALE, which is the case the box has to survive.
    // `getScaledDimensions` answers a PNG with its native size, so a cap that
    // only ASKED the codec would hand this cache a 5.76 MB picture for a 57 KB
    // row and spend a whole band's ceiling on its first entry — with every
    // dimension check above still passing, because they were only ever asked
    // about a JPEG.
    const std::string png = dir + "/backdrop.png";
    if (!makePng(png, 1600, 900, "0x30E030")) {
        check(false, "the 1600x900 PNG fixture could not be staged");
    } else {
        const sk_sp<SkImage> pngNative = SceneImageCache::decodeAtMost(png, 0, 0, &error);
        check(pngNative != nullptr && pngNative->width() == 1600 && pngNative->height() == 900,
              "a PNG decodes at its native size when no box is asked for");
        const sk_sp<SkImage> pngScaled = SceneImageCache::decodeAtMost(png, 160, 90, &error);
        check(pngScaled != nullptr && pngScaled->width() <= 160 && pngScaled->height() <= 90,
              "and inside the box when one is asked for, though PNG has no scaled mode: the "
              "resample after the decode is the half that makes the promise true");
        const std::uint32_t pngCentre = centrePixel(pngScaled);
        const int pngRed = static_cast<int>((pngCentre >> 16) & 0xFF);
        const int pngGreen = static_cast<int>((pngCentre >> 8) & 0xFF);
        const int pngBlue = static_cast<int>(pngCentre & 0xFF);
        check(pngGreen > 0xA0 && pngRed < 0x70 && pngBlue < 0x70,
              "and the resampled PNG still holds its own picture");
        SceneImageCache pngList(kListBudget);
        pngList.install(png, pngScaled);
        check(pngList.stats().decodedBytes <= 60u * 1024u,
              "so a row's PNG backdrop costs the list's cache tens of kilobytes, which is the "
              "difference between a bounded list and one that grows with its document");
    }

    // Thirty-two paths, one encoded file: the cache is keyed by path, and this
    // is the shape of a band arriving.
    const std::string backdrop = readFileBytes(big);
    std::vector<std::string> rows;
    for (int i = 0; i < kBatch; ++i) {
        rows.push_back(writeFixture(dir, "row-" + std::to_string(i) + ".jpg", backdrop));
    }

    // The production case: a whole permitted band at row size fits its ceiling
    // and nothing is evicted — which is the difference between a list that
    // plateaus and one that re-decodes a backdrop every time it scrolls past.
    SceneImageCache band(kListBudget);
    for (const std::string& path : rows) {
        band.install(path, SceneImageCache::decodeAtMost(path, 160, 90, &error));
    }
    check(band.stats().entries == static_cast<std::size_t>(kBatch) &&
              band.stats().evictions == 0,
          "the whole permitted batch at row size is kept, and none of it is thrown away");
    check(band.stats().decodedBytes <= kListBudget / 4,
          "32 rows cost at most a quarter of the list's ceiling: its picture memory is "
          "arithmetic, and the 4K backdrop it pictures is not in it");
    check(band.imageFor(rows.back()) != nullptr && band.has(rows.back()),
          "install() is the loader's whole way in, and a lookup finds what it put there with a "
          "path that cannot touch the disk");

    // The same band against a ceiling it cannot fit, so eviction is WITNESSED
    // rather than assumed. A check that nothing was evicted passes happily for
    // a cache that cannot evict at all, which is the whole lesson of R12.
    SceneImageCache tight(kTightBudget);
    for (const std::string& path : rows) {
        tight.install(path, SceneImageCache::decodeAtMost(path, 160, 90, &error));
    }
    check(tight.stats().decodedBytes <= kTightBudget,
          "a band larger than its ceiling holds the line AT that ceiling: a scrolled list "
          "plateaus instead of remembering every row it has ever painted");
    check(tight.stats().entries < static_cast<std::size_t>(kBatch),
          "not every row survived, so the bound is real rather than a coincidence of size");
    check(tight.stats().evictions >= static_cast<std::size_t>(kBatch) - tight.stats().entries,
          "and the evictions are counted rather than hidden");
    check(tight.has(rows.back()) && !tight.has(rows.front()),
          "the newest row is the one kept and the oldest the one dropped: the same recency the "
          "show's cache uses");

    // The one case eviction must not win. A picture bigger than the whole
    // ceiling, freshly installed, IS the frame about to be drawn; forgetting
    // it to chase a number would paint a row blank. The excess is reported.
    SceneImageCache oneRow(kTightBudget);
    oneRow.install(rows[0], SceneImageCache::decodeAtMost(rows[0], 0, 0, &error));
    check(oneRow.stats().entries == 1 && oneRow.has(rows[0]) &&
              oneRow.stats().decodedBytes > kTightBudget,
          "an entry larger than the entire ceiling survives when nothing else can go");
    oneRow.install(rows[1], SceneImageCache::decodeAtMost(rows[1], 0, 0, &error));
    check(oneRow.has(rows[1]) && !oneRow.has(rows[0]),
          "and the next install takes it: what is protected is the NEWEST picture, not the "
          "first one in");

    // A null install is a remembered failure, not an empty picture: the list's
    // loader must not re-open a broken file once per frame per row.
    const std::string broken = dir + "/broken.jpg";
    std::string brokenError;
    check(SceneImageCache::decodeAtMost(broken, 160, 90, &brokenError) == nullptr &&
              !brokenError.empty(),
          "a file that is not there fails with a reason");
    const std::size_t beforeFailureEntries = band.stats().entries;
    const std::size_t beforeFailureBytes = band.stats().decodedBytes;
    band.install(broken, nullptr);
    check(!band.has(broken) && band.imageFor(broken) == nullptr,
          "a remembered failure looks like no picture to the draw path");
    check(band.stats().entries == beforeFailureEntries + 1 &&
              band.stats().decodedBytes == beforeFailureBytes,
          "and it costs an entry and no pixels");
    band.install(broken, nullptr);
    check(band.stats().entries == beforeFailureEntries + 1,
          "recording the same failure again does not grow the cache");
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    const std::string dir = fixtureDir();
    const std::string churnPath = dir + "/churn-src.jpg";
    if (!makeJpeg(churnPath, kChurnSide, kChurnSide, "green", 10)) {
        std::fprintf(stderr, "no ffmpeg still: skipping (77)\n");
        std::filesystem::remove_all(dir);
        return 77;
    }
    const std::string churnJpeg = readFileBytes(churnPath);
    std::filesystem::remove_all(dir);

    testTheBoundsAreTheOnesTheCodeClaims();
    testBudgetAndPlateau(churnJpeg);
    testPinsSurviveChurnAndReleaseRejoins(churnJpeg);
    testPinnedSetLargerThanBudgetSurvivesSync(churnJpeg);
    testFailuresArePlaceholdersNotVerdicts(churnJpeg);
    testLimitsRefuseBeforeAllocating();
    testASecondBudgetAndTheScaledDecode();

    if (gFailures != 0) {
        std::fprintf(stderr, "scene_image_cache_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("ok: the decoded-asset cache budgets, pins, limits and retries as R12 asks\n");
    return 0;
}
