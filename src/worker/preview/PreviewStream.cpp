/*
 * PreviewStream implementation. The mmap'd segment is written once at
 * create() (headers only); afterwards only slot payloads and the seqlock
 * counters move. Unlinking happens in destroy(): the name disappears, the
 * mapping stays valid for whoever mapped it earlier.
 */

#include "preview/PreviewStream.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace openshow::worker {

namespace proto = openshow::protocol;

namespace {

std::atomic_ref<std::uint64_t> seqOf(proto::PreviewSlotHeader* header) {
    return std::atomic_ref<std::uint64_t>(header->seq);
}

}  // namespace

PreviewStream::~PreviewStream() {
    destroy();
}

bool PreviewStream::create(const std::string& name, uint32_t width, uint32_t height,
                           std::string* error) {
    destroy();

    // Geometry validated before any mmap or slot arithmetic (task 6.5,
    // review R5): the wire stores a per-slot stride as uint32, so a size
    // that does not fit — or does not fit the sane-preview bounds the
    // session enforces — is refused here too rather than wrapping into a
    // segment shorter than its slots.
    const std::size_t stride = proto::previewSlotStrideBytes(width, height);
    if (width == 0 || height == 0 || width > 4096 || height > 4096 ||
        stride > std::numeric_limits<std::uint32_t>::max() - 64) {
        if (error) {
            *error = "preview geometry out of range";
        }
        return false;
    }

    const std::size_t bytes = proto::previewSegmentBytes(width, height);
    fName = name;
    const int fd = ::shm_open(fName.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        if (error) {
            *error = "shm_open " + fName + ": " + std::strerror(errno);
        }
        return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        if (error) {
            *error = "ftruncate " + fName + ": " + std::strerror(errno);
        }
        ::close(fd);
        ::shm_unlink(fName.c_str());
        fName.clear();
        return false;
    }
    void* mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        if (error) {
            *error = "mmap " + fName + ": " + std::strerror(errno);
        }
        ::close(fd);
        ::shm_unlink(fName.c_str());
        fName.clear();
        return false;
    }

    fMapping = mapping;
    fFd = fd;
    fWidth = width;
    fHeight = height;
    fBytesPerSlot = static_cast<std::uint32_t>(proto::previewSlotStrideBytes(width, height));
    // A new segment is a new ownership life: the seq bookkeeping must
    // start empty, or a stream object re-created over an old life would
    // keep honouring the retired segment's outstanding claims (and its
    // release proofs).
    std::memset(fPublished, 0, sizeof(fPublished));
    std::memset(fReleased, 0, sizeof(fReleased));

    std::memset(mapping, 0, bytes);
    auto* segment = static_cast<proto::PreviewSegmentHeader*>(mapping);
    segment->magic = proto::kPreviewMagic;
    segment->layoutVersion = proto::kPreviewLayoutVersion;
    segment->slotCount = proto::kPreviewSlotCount;
    segment->bytesPerSlot = fBytesPerSlot;
    segment->width = width;
    segment->height = height;
    segment->format = proto::kPreviewFourccRGBA8;
    for (std::uint32_t slot = 0; slot < proto::kPreviewSlotCount; ++slot) {
        auto* header = reinterpret_cast<proto::PreviewSlotHeader*>(slotBase(slot));
        header->width = width;
        header->height = height;
        header->rowBytes = width * 4;
        header->format = proto::kPreviewFourccRGBA8;
    }
    std::fprintf(stderr, "openshow-worker: preview stream %s, %ux%u, %u slots\n",
                 fName.c_str(), width, height, proto::kPreviewSlotCount);
    return true;
}

void PreviewStream::destroy() {
    if (fMapping) {
        ::munmap(fMapping, proto::previewSegmentBytes(fWidth, fHeight));
        fMapping = nullptr;
    }
    if (fFd >= 0) {
        ::close(fFd);
        fFd = -1;
    }
    if (!fName.empty()) {
        ::shm_unlink(fName.c_str());
        fName.clear();
    }
    fWidth = 0;
    fHeight = 0;
}

std::uint8_t* PreviewStream::slotBase(std::uint32_t slot) const {
    return static_cast<std::uint8_t*>(fMapping) +
           proto::previewSlotOffset(slot, fBytesPerSlot);
}

int PreviewStream::claim() {
    if (!fMapping) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(fMutex);
    for (std::uint32_t slot = 0; slot < proto::kPreviewSlotCount; ++slot) {
        if (fReleased[slot] >= fPublished[slot]) {
            // Open the seqlock: odd tells the consumer the payload is
            // mid-write, and no consumer can claim to own this frame
            // because nothing newer was published from it.
            seqOf(slotHeader(static_cast<int>(slot)))
                .store(fPublished[slot] + 1, std::memory_order_seq_cst);
            return static_cast<int>(slot);
        }
    }
    return -1;
}

std::uint8_t* PreviewStream::slotPixels(int slot) {
    return slotBase(static_cast<std::uint32_t>(slot)) +
           sizeof(proto::PreviewSlotHeader);
}

proto::PreviewSlotHeader* PreviewStream::slotHeader(int slot) {
    return reinterpret_cast<proto::PreviewSlotHeader*>(
        slotBase(static_cast<std::uint32_t>(slot)));
}

std::uint64_t PreviewStream::publish(int slot, std::uint32_t revision,
                                     std::uint32_t target, std::uint32_t request,
                                     std::uint64_t publishedNs) {
    std::lock_guard<std::mutex> lock(fMutex);
    auto* header = slotHeader(slot);
    header->revision = revision;
    // Which picture these are, and which request answers it (task 6.16a):
    // written inside the seqlock's odd window like every other field, so a
    // consumer that sees an even sequence has a consistent (target,
    // request, pixels) triple and can drop the frame on metadata alone.
    header->target = target;
    header->request = request;
    header->publishedNs = publishedNs;
    const std::uint64_t even = fPublished[static_cast<std::size_t>(slot)] + 2;
    seqOf(header).store(even, std::memory_order_seq_cst);
    fPublished[static_cast<std::size_t>(slot)] = even;
    return even;
}

void PreviewStream::abort(int slot) {
    std::lock_guard<std::mutex> lock(fMutex);
    // Close the seqlock on the previous published value: nothing changed.
    seqOf(slotHeader(slot))
        .store(fPublished[static_cast<std::size_t>(slot)], std::memory_order_seq_cst);
}

void PreviewStream::release(std::uint32_t slot, std::uint64_t seq) {
    if (slot >= proto::kPreviewSlotCount) {
        return;
    }
    std::lock_guard<std::mutex> lock(fMutex);
    std::uint64_t& released = fReleased[static_cast<std::size_t>(slot)];
    // Proof of ownership (task 6.5, review R5): only the exact sequence
    // this slot most recently published releases it. The watermark this
    // used to keep accepted any newer number — release(0, 102) against a
    // slot at sequence 2 made a still-owned slot claimable and let the
    // producer overwrite a frame mid-copy. Odd (mid-write), stale,
    // future and duplicate sequences change nothing; a slot is freed for
    // reuse exactly once per publish.
    if (seq == fPublished[static_cast<std::size_t>(slot)] && released < seq) {
        released = seq;
    }
}

}  // namespace openshow::worker
