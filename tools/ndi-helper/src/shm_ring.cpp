#include "shm_ring.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace openshow::ndi {

namespace {
std::atomic<unsigned> gCounter{0};
}

ShmRing::~ShmRing() { close(); }

bool ShmRing::create(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                     std::uint32_t format, std::string* error) {
    close();
    const std::uint64_t total = openshow::live::frameRingBytes(stride, height);
    if (total > openshow::live::kFrameRingMaxBytes) {
        if (error) {
            *error = "frame ring: frame is too large";
        }
        return false;
    }

    const std::string base = "/openshow-ndi-" + std::to_string(static_cast<long>(getpid())) + "-";
    int fd = -1;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        std::string name = base + std::to_string(gCounter.fetch_add(1));
        fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
        if (fd >= 0) {
            fName = name;
            break;
        }
        if (errno != EEXIST) {
            if (error) {
                *error = std::string("frame ring: shm_open failed: ") + std::strerror(errno);
            }
            return false;
        }
    }
    if (fd < 0) {
        if (error) {
            *error = "frame ring: could not allocate a unique shared-memory name";
        }
        return false;
    }

    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
        if (error) {
            *error = std::string("frame ring: ftruncate failed: ") + std::strerror(errno);
        }
        ::close(fd);
        close();
        return false;
    }

    void* map = ::mmap(nullptr, static_cast<std::size_t>(total), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
        if (error) {
            *error = std::string("frame ring: mmap failed: ") + std::strerror(errno);
        }
        close();
        return false;
    }

    fMap = map;
    fBytes = static_cast<std::size_t>(total);
    fRing = std::make_unique<openshow::live::FrameRing>(map, fBytes);
    if (!fRing->initForWrite(width, height, stride, format, error)) {
        close();
        return false;
    }
    return true;
}

bool ShmRing::write(const openshow::live::FrameView& frame, std::string* error) {
    if (!fRing) {
        if (error) {
            *error = "frame ring: no open ring";
        }
        return false;
    }
    return fRing->write(frame, error);
}

void ShmRing::close() {
    fRing.reset();
    if (fMap) {
        ::munmap(fMap, fBytes);
        fMap = nullptr;
    }
    fBytes = 0;
    if (!fName.empty()) {
        ::shm_unlink(fName.c_str());
        fName.clear();
    }
}

bool ShmRing::valid() const { return fRing && fRing->validate(nullptr); }

const openshow::live::FrameRingHeader* ShmRing::header() const {
    return fRing ? fRing->header() : nullptr;
}

}  // namespace openshow::ndi
