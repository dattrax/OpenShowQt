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

    if (gFailures != 0) {
        std::fprintf(stderr, "scene_image_cache_tests: %d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("ok: the decoded-asset cache budgets, pins, limits and retries as R12 asks\n");
    return 0;
}
