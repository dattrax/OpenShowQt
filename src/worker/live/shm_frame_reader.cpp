#include "live/shm_frame_reader.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace openshow::live {

namespace {

std::uint64_t publishedSequence(const FrameRingHeader* header) {
    return reinterpret_cast<const std::atomic<std::uint64_t>*>(&header->sequence)
        ->load(std::memory_order_acquire);
}

bool fail(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

}  // namespace

FrameStatus readNewestFrame(FrameRing& ring, std::uint64_t afterSequence, Frame* frame,
                            std::string* error) {
    if (error) {
        error->clear();
    }
    if (!frame) {
        fail(error, "live frame: no destination");
        return FrameStatus::Error;
    }
    if (!ring.validate(error)) {
        return FrameStatus::Error;
    }
    const FrameRingHeader* header = ring.header();
    const std::uint64_t sequence = publishedSequence(header);
    if (sequence <= afterSequence) {
        return FrameStatus::NoFrame;
    }
    const std::uint64_t bytes = header->frameBytes;
    if (bytes == 0) {
        return FrameStatus::NoFrame;
    }
    if (!frame->ensure(static_cast<std::size_t>(bytes))) {
        fail(error, "live frame: could not allocate the copy buffer");
        return FrameStatus::Error;
    }

    FrameView view;
    if (!ring.read(&view, frame->pixels.data(), frame->pixels.size(), error)) {
        // A false read with no message is a writer race: the slot was still
        // being filled or was overwritten during the copy. Not an error.
        if (error && !error->empty()) {
            return FrameStatus::Error;
        }
        return FrameStatus::NoFrame;
    }

    frame->width = view.width;
    frame->height = view.height;
    frame->stride = view.stride;
    frame->format = view.format;
    frame->flags = view.flags;
    frame->timestampNs = view.timestampNs;
    frame->sequence = sequence;
    return FrameStatus::Frame;
}

ShmFrameReader::~ShmFrameReader() { unmap(); }

bool ShmFrameReader::map(const std::string& name, std::uint64_t bytes, std::string* error) {
    unmap();
    if (name.empty()) {
        return fail(error, "live frame: no shared-memory name");
    }
    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        return fail(error, "live frame: cannot open '" + name + "': " + std::strerror(errno));
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        const std::string message = std::strerror(errno);
        ::close(fd);
        return fail(error, "live frame: cannot stat '" + name + "': " + message);
    }
    const std::uint64_t actual = static_cast<std::uint64_t>(status.st_size);
    const std::uint64_t wanted = bytes == 0 ? actual : bytes;
    if (actual < wanted || wanted < kFrameRingDataOffset) {
        ::close(fd);
        return fail(error, "live frame: segment '" + name + "' is smaller than the frame");
    }
    void* map = ::mmap(nullptr, static_cast<std::size_t>(actual), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
        return fail(error, "live frame: cannot map '" + name + "': " + std::strerror(errno));
    }

    auto ring = std::make_unique<FrameRing>(map, static_cast<std::size_t>(actual));
    if (!ring->validate(error)) {
        ::munmap(map, static_cast<std::size_t>(actual));
        return false;
    }
    fName = name;
    fMap = map;
    fBytes = static_cast<std::size_t>(actual);
    fRing = std::move(ring);
    return true;
}

void ShmFrameReader::unmap() {
    fRing.reset();
    if (fMap) {
        ::munmap(fMap, fBytes);
        fMap = nullptr;
    }
    fBytes = 0;
    fName.clear();
}

const FrameRingHeader* ShmFrameReader::header() const {
    return fRing ? fRing->header() : nullptr;
}

FrameStatus ShmFrameReader::read(std::uint64_t afterSequence, Frame* frame, std::string* error) {
    if (!fRing) {
        if (error) {
            error->clear();
        }
        return FrameStatus::NoFrame;
    }
    return readNewestFrame(*fRing, afterSequence, frame, error);
}

}  // namespace openshow::live
