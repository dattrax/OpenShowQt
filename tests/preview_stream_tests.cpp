/*
 * Preview slot ownership (task 6.5, review R5): a release proves
 * ownership or it frees nothing.
 *
 * The review's runtime probe was concrete — slot 0 published sequence 2,
 * and a release of 102 immediately made that still-owned slot claimable,
 * handing the producer a frame the consumer still held. These fixtures
 * drive the real PreviewStream headless: only the exact sequence a slot
 * most recently published releases it; odd (mid-write), stale, future,
 * duplicate and out-of-range releases change nothing; a slot is handed
 * out at most once per publish; and the geometry a stream will mmap is
 * validated before the mapping, not after the arithmetic.
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include <unistd.h>

#include "openshow/protocol/preview_wire.h"
#include "preview/PreviewStream.h"

namespace {

namespace proto = openshow::protocol;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::uint64_t seqAt(openshow::worker::PreviewStream& stream, int slot) {
    const auto* header = stream.slotHeader(slot);
    return std::atomic_ref<const std::uint64_t>(header->seq).load();
}

std::string streamName(const char* tag) {
    return "/openshow-pvtest-" + std::to_string(static_cast<long>(::getpid())) +
           "-" + tag;
}

}  // namespace

int main() {
    // --- the R5 probe: a future sequence does not release an owned slot ---
    {
        openshow::worker::PreviewStream stream;
        std::string error;
        if (!check(stream.create(streamName("probe"), 64, 48, &error),
                   "probe: stream created")) {
            std::fprintf(stderr, "  %s\n", error.c_str());
        }
        const int slotA = stream.claim();
        check(slotA == 0, "probe: first claim takes slot 0");
        const std::uint64_t seqA = stream.publish(slotA, 7, proto::kPreviewTargetAudience, 0,
                                          1234);
        check(seqA == 2, "probe: first publish carries sequence 2");
        check(seqAt(stream, slotA) == seqA, "probe: seqlock reads the even sequence");

        // The exact review runtime probe: release(0, 102).
        stream.release(static_cast<std::uint32_t>(slotA), 102);
        const int next = stream.claim();
        check(next != slotA, "probe: release(102) did NOT free the owned slot");
        check(next == 1, "probe: claim moved on to the next free slot");
        stream.publish(next, 7, proto::kPreviewTargetAudience, 0, 1235);
        const int third = stream.claim();
        check(third == 2, "probe: third slot claims normally");
        stream.publish(third, 7, proto::kPreviewTargetAudience, 0, 1236);
        check(stream.claim() == -1, "probe: all three slots still owned downstream");

        // Odd and future releases stay refused while the slots are held.
        stream.release(0, 3);   // odd: mid-write is not an ownership claim
        stream.release(1, 100); // future: nothing published that high
        stream.release(2, 1);   // stale: below the published sequence
        check(stream.claim() == -1, "probe: odd/future/stale releases free nothing");

        // The exact sequence frees exactly one slot.
        stream.release(0, seqA);
        const int reused = stream.claim();
        check(reused == 0, "probe: the exact release returned slot 0 to the producer");
        check(seqAt(stream, 0) % 2 == 1, "probe: a claimed slot reads odd (mid-write)");
        const std::uint64_t seqA2 = stream.publish(0, 8, proto::kPreviewTargetSelection, 5, 1240);
        check(seqA2 == 4, "probe: the reused slot continues its sequence");
        {  // Task 6.16a (review R16): one stream, two audiences. The header
            // says whose picture these are and which ask they answer, and it
            // does so inside the same seqlock window as the pixels — so a
            // consumer that reads an even sequence has a consistent
            // (target, request, revision) triple and can drop a frame on
            // metadata alone.
            const auto* header = stream.slotHeader(0);
            check(header->target == proto::kPreviewTargetSelection,
                  "probe: the header names the selection target");
            check(header->request == 5, "probe: the header names the request it answers");
            check(header->revision == 8, "probe: the header carries the scene revision");
            // And the mirror is distinguishable from it: an audience frame
            // answers no request, and a consumer checking either field
            // cannot confuse the two.
            stream.release(0, seqA2);
            const int mirrored = stream.claim();
            check(mirrored == 0, "probe: the released slot is claimable for the mirror");
            stream.publish(mirrored, 9, proto::kPreviewTargetAudience, 0, 1300);
            check(stream.slotHeader(mirrored)->target == proto::kPreviewTargetAudience &&
                      stream.slotHeader(mirrored)->request == 0,
                  "probe: an audience frame claims no request generation");
        }

        // The duplicate / late release of the now-SUPERSEDED sequence
        // frees nothing: one release per publish, ever.
        stream.release(0, seqA);
        stream.release(0, seqA);
        check(stream.claim() == -1, "probe: stale releases of a superseded frame free nothing");

        // Correct releases of the two remaining slots free them in turn.
        stream.release(1, 2);
        check(stream.claim() == 1, "probe: exact release frees slot 1");
        // Out-of-range slots are refused, not clamped.
        stream.release(3, 2);
        stream.release(99, 2);
        stream.abort(1);
        stream.release(1, 1);  // odd — the abort left it back at the old sequence
        check(stream.claim() == 1, "probe: abort closed the seqlock, slot still free");
    }

    // --- abort returns the slot with nothing published ---
    {
        openshow::worker::PreviewStream stream;
        std::string error;
        check(stream.create(streamName("abort"), 64, 48, &error), "abort: stream created");
        const int slot = stream.claim();
        check(slot == 0, "abort: slot claimed");
        check(seqAt(stream, slot) == 1, "abort: claim opened an odd seqlock");
        stream.abort(slot);
        check(seqAt(stream, slot) == 0, "abort: seqlock closed on the old value");
        check(stream.claim() == 0, "abort: the aborted slot is claimable again");
    }

    // --- geometry validated before any mapping ---
    {
        openshow::worker::PreviewStream stream;
        std::string error;
        check(!stream.create(streamName("zero"), 0, 48, &error),
              "geometry: zero width is refused before mmap");
        check(!error.empty(), "geometry: refusal carries an error message");
        check(!stream.create(streamName("tall"), 64, 0, &error),
              "geometry: zero height is refused");
        check(!stream.create(streamName("wide"), 100000, 100000, &error),
              "geometry: a stride that cannot fit uint32 offsets is refused");
        check(stream.name().empty(), "geometry: refused creates name nothing");
    }

    // --- a recreated stream restarts its sequences under its new name ---
    {
        openshow::worker::PreviewStream stream;
        std::string error;
        const std::string first = streamName("rec1");
        const std::string second = streamName("rec2");
        check(stream.create(first, 64, 48, &error), "replace: first stream created");
        const int slot = stream.claim();
        stream.publish(slot, 1, proto::kPreviewTargetAudience, 0, 10);
        stream.publish(stream.claim(), 1, proto::kPreviewTargetAudience, 0, 11);
        stream.publish(stream.claim(), 1, proto::kPreviewTargetSelection, 3, 12);
        check(stream.claim() == -1, "replace: everything owned before the swap");
        check(stream.create(second, 96, 64, &error), "replace: create() replaces the stream");
        check(stream.name() == second, "replace: the stream now answers to the new name");
        check(stream.claim() == 0, "replace: a fresh stream starts with free slots");
        check(stream.publish(0, 2, proto::kPreviewTargetAudience, 0, 20) == 2,
              "replace: sequences restart at 2");
        // A release carrying the OLD stream's bookkeeping (same slot/seq
        // numbers!) releases the NEW slot only if it happens to name the
        // exact published sequence — which is the session's job to gate
        // by stream name; here the numbers simply belong to the new life.
        check(stream.claim() == 1, "replace: the new stream owns only what it published");
    }

    // --- a frame carries its own size (task 6.16b, review R16) ------------
    //
    // A thumbnail shares the segment without sharing the stream's geometry, so
    // the header's width/height/rowBytes stopped being stream-wide constants
    // and became per-frame state. Two ways to get that wrong, one check each:
    // stamping only when a caller bothers to pass an extent (a mirror then
    // reuses a slot whose header still claims the last thumbnail's size), and
    // trusting the notification's numbers where the header's disagree.
    {
        openshow::worker::PreviewStream stream;
        std::string error;
        if (!check(stream.create(streamName("extent"), 64, 48, &error),
                   "extent: stream created")) {
            std::fprintf(stderr, "  %s\n", error.c_str());
        }
        check(stream.bytesPerSlot() == 64 + 64 * 48 * 4,
              "extent: one slot's stride is a header plus 64x48 RGBA");
        const int slot = stream.claim();
        const std::uint64_t first =
            stream.publish(slot, 1, proto::kPreviewTargetAudience, 0, 10);
        const auto* header = stream.slotHeader(slot);
        check(header->width == 64 && header->height == 48 && header->rowBytes == 256,
              "extent: a frame published with no extent carries the stream's own geometry");

        stream.release(slot, first);
        const int again = stream.claim();
        check(again == slot, "extent: the released slot is claimable for a smaller frame");
        const openshow::worker::PreviewStream::FrameExtent small{32, 24, 128};
        const std::uint64_t second = stream.publish(again, 7, proto::kPreviewTargetThumbnail, 3,
                                                    20, &small);
        check(header->width == 32 && header->height == 24 && header->rowBytes == 128,
              "extent: the thumbnail stamps its OWN size, and a consumer bounded by it copies "
              "exactly the bytes the producer wrote");
        check(header->target == proto::kPreviewTargetThumbnail && header->request == 3 &&
                  header->revision == 7,
              "extent: a thumbnail frame names its target, batch generation and slide id");
        check(sizeof(proto::PreviewSlotHeader) + 128ull * 24 <= stream.bytesPerSlot(),
              "extent: the stamped frame does fit the slot it was published into");

        stream.release(again, second);
        const int third = stream.claim();
        stream.publish(third, 9, proto::kPreviewTargetAudience, 0, 30);
        check(header->width == 64 && header->height == 48 && header->rowBytes == 256,
              "extent: the mirror reusing that slot restamps its own size — geometry is per "
              "frame, not the first thing written to the slot");

        // The helper both halves judge a batch by, on this stream's real
        // numbers: the stream's own extent is the ceiling, the box is the
        // floor and the cap, and neither is a suggestion.
        check(proto::thumbnailExtentFits(32, 32, stream.bytesPerSlot()),
              "extent: a 32x32 thumbnail fits a 64x48 slot");
        check(!proto::thumbnailExtentFits(65, 48, stream.bytesPerSlot()),
              "extent: one pixel wider than the slot's frame is refused, not clipped");
        check(!proto::thumbnailExtentFits(16, 16, stream.bytesPerSlot()),
              "extent: under the 32-pixel box is refused (a row that size is not worth a draw)");
        check(!proto::thumbnailExtentFits(513, 32, stream.bytesPerSlot()),
              "extent: over the 512-pixel box is refused whatever the slot could hold");
    }

    if (gFailures > 0) {
        return 1;
    }
    std::printf("preview_stream_tests: a release proves ownership or frees nothing\n");
    return 0;
}
