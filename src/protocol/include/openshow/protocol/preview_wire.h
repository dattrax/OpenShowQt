/*
 * Shared-memory preview transport (task 1.4, PLAN.md "Preview").
 *
 * Pixels never travel over the RPC socket. The worker creates one POSIX
 * shared-memory segment holding three slots; the socket carries only the
 * preview.start result (which names the segment) and per-frame ready and
 * release notifications. The frontend maps the segment by name.
 *
 * Layout inside the mapped segment (little-endian, fixed-size POD headers,
 * same machine on both ends):
 *
 *   offset 0     PreviewSegmentHeader                    (64 bytes)
 *   slot i       PreviewSlotHeader                       (64 bytes)
 *   slot i + 64  pixels: rowBytes * height, RGBA8 top-down
 *
 * Slot ownership is a seqlock. `seq` even means the payload is stable;
 * odd means the producer is mid-write. A consumer copies the payload and
 * re-reads `seq`: unchanged means the copy is valid. The producer, on its
 * side, only claims a slot whose last published frame the consumer has
 * already released (see preview.released) or that was never used; when no
 * slot qualifies the preview frame is dropped. Output is never blocked by
 * a preview consumer, and a slot still owned by the consumer is never
 * overwritten — the two properties PLAN.md demands.
 *
 * Ownership is PROVEN, not assumed (task 6.5, review R5): the release
 * notification must name the stream it belongs to (the segment name —
 * `preview.ready` carries it as "stream", and capability
 * "preview.stream-id.v1" says so) and carry the exact sequence that
 * stream most recently published for that slot. Odd (mid-write), stale,
 * future and duplicate sequences release nothing, and a release naming
 * any other stream is refused — so a delayed release from a retired
 * stream can never free a live stream's slot.
 *
 * Two kinds of picture share one stream (task 6.16a, review R16). The
 * audience mirror is what the panel was shown; a SELECTION frame is the
 * worker's render of a prepared scene the operator has picked and NOT put
 * on air, so the editor can show the real Skia picture — text laid out by
 * the production engine, background media included — without presenting
 * anything. Every frame says which it is (`target`) and, for a selection,
 * which REQUEST it answers (`request`, the generation the frontend asked
 * for): a frame whose generation is not the one the client wants is stale
 * imagery, and the client drops it rather than painting yesterday's edit
 * under today's handles. Layout version 2 is exactly those two fields.
 *
 * The header is deliberately Qt-free and dependency-free so both processes
 * compile the exact same definition.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace openshow::protocol {

inline constexpr std::uint32_t kPreviewMagic = 0x4F565350;  // "OPSV" little-endian
// Version 2 (task 6.16a): the slot header names the TARGET the pixels were
// rendered for and the request GENERATION they answer. Version 1 carried
// neither, so a stream could only ever mirror the audience.
inline constexpr std::uint32_t kPreviewLayoutVersion = 2;
inline constexpr std::uint32_t kPreviewSlotCount = 3;
inline constexpr std::uint32_t kPreviewFourccRGBA8 = 0x52474241;  // "RGBA"

// Who a published frame was drawn for (PreviewSlotHeader::target).
inline constexpr std::uint32_t kPreviewTargetAudience = 0;   // the panel's picture
inline constexpr std::uint32_t kPreviewTargetSelection = 1;  // an off-air edit
// Task 6.16b (review R16): one slide of the list, rendered on demand at the
// size the frontend asked for. It is a THIRD target on the SAME stream rather
// than a second segment: the slot accounting, the seqlock, the identity rule
// and the drop-don't-block producer are the thing thumbnails need, and
// duplicating them would put the interesting failure — a frame of the wrong
// kind under valid handles — on a seam no consumer checks. A frame carries
// its OWN width/height/rowBytes, so a thumbnail needs no new layout version;
// its `revision` names the id the worker assigned that slide in its ack (the
// per-target meaning of a per-target field), and its `request` names the
// batch generation it answers.
//
// An older consumer reads any target it does not know as the audience, which
// is why a worker only ever draws one when the frontend asked: the capability
// ("preview.thumbnail.v1") is the whole of the negotiation.
inline constexpr std::uint32_t kPreviewTargetThumbnail = 2;

// Notification and method names shared by both sides.
inline constexpr const char* kMethodPreviewReady = "preview.ready";
inline constexpr const char* kMethodPreviewReleased = "preview.released";
// Task 6.16a (review R16): ask the worker to render a PREPARED scene that is
// not on air, into this same transport.
inline constexpr const char* kMethodPreviewSelect = "preview.select";
// Task 6.16b (review R16): ask the worker to render a BATCH of scenes it has
// NOT prepared — the payloads travel as text, never pixels — at a thumbnail
// size, into this same transport. The request REPLACES the pending batch, so
// coalescing and cancellation are the same act: the newest list wins and
// nothing queued for an older one is ever drawn.
inline constexpr const char* kMethodPreviewThumbnails = "preview.thumbnails";

// The batch's bounds, in ONE place because both halves enforce them: the
// worker refuses a request it will not honour, and a frontend that asked
// beyond these is buggy rather than merely optimistic. The queue is what
// bounds memory — an entry is a compiled scene plus, once decoded, one
// thumbnail-sized picture — so a list of any length costs at most this.
inline constexpr std::size_t kMaxThumbnailBatch = 32;
inline constexpr std::size_t kMaxThumbnailKeyLength = 128;
inline constexpr std::uint32_t kMinThumbnailDimension = 32;
inline constexpr std::uint32_t kMaxThumbnailDimension = 512;

struct PreviewSegmentHeader {
    std::uint32_t magic = kPreviewMagic;
    std::uint32_t layoutVersion = kPreviewLayoutVersion;
    std::uint32_t slotCount = kPreviewSlotCount;
    std::uint32_t bytesPerSlot = 0;   // header + pixels; slot stride
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = kPreviewFourccRGBA8;
    std::uint32_t reserved = 0;
    std::uint8_t padding[32] = {};
};

// `revision` is per-target by necessity: the header is a fixed 64 bytes and a
// frame is identified by (target, request, revision). For the audience it is
// the presented session revision; for a selection, the prepared revision being
// rendered; for a thumbnail (task 6.16b), the id the worker assigned that
// slide in its preview.thumbnails ack — the only stable name a scene the
// store has never held can have.
struct PreviewSlotHeader {
    // Seqlock counter, see file comment. Publish protocol for one frame:
    // seq += 1 (odd), write header fields then pixels, seq += 1 (even).
    std::uint64_t seq = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowBytes = 0;       // width * 4
    std::uint32_t format = kPreviewFourccRGBA8;
    std::uint32_t revision = 0;       // session revision the frame shows
    // Task 6.16a (review R16): which request these pixels answer. A stream
    // now carries two kinds of picture, and the consumer cannot tell an
    // off-air edit from the audience's own frame by looking at bytes.
    std::uint32_t target = kPreviewTargetAudience;
    std::uint32_t request = 0;        // the ask these pixels answer; 0 for the mirror
    std::uint64_t publishedNs = 0;    // worker steady clock at publish time
    std::uint8_t padding[16] = {};
};

static_assert(sizeof(PreviewSegmentHeader) == 64, "segment header must be one cache line");
static_assert(sizeof(PreviewSlotHeader) == 64, "slot header must be one cache line");
static_assert(alignof(PreviewSegmentHeader) <= 64 && alignof(PreviewSlotHeader) <= 64,
              "headers must fit their fixed stride");

// Geometry helpers: the sizes both sides agree on for a given frame size.
inline constexpr std::size_t previewSlotStrideBytes(std::uint32_t width,
                                                    std::uint32_t height) {
    return 64 + static_cast<std::size_t>(width) * height * 4;
}

inline constexpr std::size_t previewSegmentBytes(std::uint32_t width,
                                                 std::uint32_t height) {
    return 64 + kPreviewSlotCount * previewSlotStrideBytes(width, height);
}

// Byte offset of slot `index` (its header; pixels follow at +64).
inline constexpr std::size_t previewSlotOffset(std::uint32_t index,
                                               std::size_t bytesPerSlot) {
    return 64 + static_cast<std::size_t>(index) * bytesPerSlot;
}

// A thumbnail may be any size in the agreed box that FITS the slot it is
// published into, because a slot's payload area was reserved for the stream's
// own frame size. Both halves of the conversation check this: the worker
// refuses to accept a batch it could not publish, and a consumer checks the
// per-frame extent of a frame it is about to copy before it memcpy's.
inline constexpr bool thumbnailExtentFits(std::uint32_t width, std::uint32_t height,
                                          std::uint32_t bytesPerSlot) {
    if (width < kMinThumbnailDimension || height < kMinThumbnailDimension ||
        width > kMaxThumbnailDimension || height > kMaxThumbnailDimension) {
        return false;
    }
    const std::uint64_t payload =
        static_cast<std::uint64_t>(width) * 4ull * static_cast<std::uint64_t>(height);
    return payload + 64ull <= bytesPerSlot;
}

}  // namespace openshow::protocol
