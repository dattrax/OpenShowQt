#include "live/fake_backend.h"

#include "live/shm_frame_reader.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace openshow::live {

namespace {

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 180;
constexpr std::uint32_t kStride = kWidth * 4;
constexpr auto kPeriod = std::chrono::milliseconds(33);
std::atomic<unsigned> gCounter{0};

void paint(std::vector<std::uint8_t>& pixels, std::uint64_t frame) {
    const std::uint32_t bar = kWidth / 8;
    const std::uint32_t moving = static_cast<std::uint32_t>((frame * 3) % kWidth);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * kStride;
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const std::uint32_t band = x / (bar == 0 ? 1 : bar);
            std::uint8_t b = 0;
            std::uint8_t g = 0;
            std::uint8_t r = 0;
            switch (band % 8) {
                case 0: b = 235; g = 235; r = 235; break;
                case 1: b = 235; g = 235; r = 40; break;
                case 2: b = 40; g = 235; r = 235; break;
                case 3: b = 40; g = 235; r = 40; break;
                case 4: b = 235; g = 40; r = 235; break;
                case 5: b = 40; g = 40; r = 235; break;
                case 6: b = 235; g = 40; r = 40; break;
                default: b = 40; g = 40; r = 40; break;
            }
            if (x >= moving && x < moving + 8) {
                b = 255;
                g = 255;
                r = 255;
            }
            std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = 255;
        }
    }
}

}  // namespace

FakeBackend::~FakeBackend() { stop(); }

bool FakeBackend::start(std::string* /*error*/) { return true; }

bool FakeBackend::probe(std::string* ndiVersion, std::string* error) {
    if (!ndiVersion) {
        if (error) {
            *error = "probe: no output";
        }
        return false;
    }
    *ndiVersion = "fake";
    return true;
}

bool FakeBackend::list(std::uint32_t /*waitMs*/, std::vector<SourceInfo>* out,
                       std::string* error) {
    if (!out) {
        if (error) {
            *error = "list: no output";
        }
        return false;
    }
    out->clear();
    out->push_back({"Fake NDI camera 1", "fake://camera1"});
    out->push_back({"Fake NDI camera 2", "fake://camera2"});
    return true;
}

bool FakeBackend::createSegment(std::uint32_t width, std::uint32_t height,
                                std::uint32_t stride, std::string* error) {
    const std::uint64_t total = frameRingBytes(stride, height);
    if (total > kFrameRingMaxBytes) {
        if (error) {
            *error = "fake live source: frame is too large";
        }
        return false;
    }

    const std::string base =
        "/openshow-live-fake-" + std::to_string(static_cast<long>(getpid())) + "-";
    int fd = -1;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const std::string name = base + std::to_string(gCounter.fetch_add(1));
        fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
        if (fd >= 0) {
            fName = name;
            break;
        }
        if (errno != EEXIST) {
            if (error) {
                *error = std::string("fake live source: shm_open failed: ") + std::strerror(errno);
            }
            return false;
        }
    }
    if (fd < 0) {
        if (error) {
            *error = "fake live source: could not allocate a segment name";
        }
        return false;
    }

    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
        if (error) {
            *error = std::string("fake live source: ftruncate failed: ") + std::strerror(errno);
        }
        ::close(fd);
        fName.clear();
        return false;
    }
    void* map = ::mmap(nullptr, static_cast<std::size_t>(total), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
        if (error) {
            *error = std::string("fake live source: mmap failed: ") + std::strerror(errno);
        }
        ::shm_unlink(fName.c_str());
        fName.clear();
        return false;
    }

    auto ring = std::make_unique<FrameRing>(map, static_cast<std::size_t>(total));
    if (!ring->initForWrite(width, height, stride,
                            static_cast<std::uint32_t>(FrameFormat::BGRA8), error)) {
        ::munmap(map, static_cast<std::size_t>(total));
        ::shm_unlink(fName.c_str());
        fName.clear();
        return false;
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
    paint(pixels, 0);
    FrameView first;
    first.data = pixels.data();
    first.width = width;
    first.height = height;
    first.stride = stride;
    first.format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    first.flags = kFrameFlagAlpha;
    if (!ring->write(first, error)) {
        ::munmap(map, static_cast<std::size_t>(total));
        ::shm_unlink(fName.c_str());
        fName.clear();
        return false;
    }

    fMap = map;
    fBytes = static_cast<std::size_t>(total);
    fRing = std::move(ring);
    return true;
}

void FakeBackend::destroySegment() {
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

bool FakeBackend::open(const std::string& source, OpenInfo* out, std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!out) {
        if (error) {
            *error = "open: no output";
        }
        return false;
    }
    if (fStreaming.load()) {
        if (error) {
            *error = "open: a live source is already open";
        }
        return false;
    }
    fSource = source;
    if (!createSegment(kWidth, kHeight, kStride, error)) {
        fSource.clear();
        return false;
    }

    fStop.store(false);
    fStreaming.store(true);
    fThread = std::thread([this] { run(); });

    out->shmName = fName;
    out->width = kWidth;
    out->height = kHeight;
    out->stride = kStride;
    out->format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
    out->bytes = fBytes;
    out->fps = 30.0;
    return true;
}

bool FakeBackend::close(std::string* /*error*/) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (fStreaming.load()) {
        fStop.store(true);
        if (fThread.joinable()) {
            fThread.join();
        }
        fStreaming.store(false);
    }
    destroySegment();
    fSource.clear();
    return true;
}

bool FakeBackend::streaming() const { return fStreaming.load(); }

FrameStatus FakeBackend::readFrame(std::uint64_t afterSequence, Frame* frame,
                                   std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fRing) {
        if (error) {
            error->clear();
        }
        return FrameStatus::NoFrame;
    }
    return readNewestFrame(*fRing, afterSequence, frame, error);
}

bool FakeBackend::takeEvent(Event* /*out*/) { return false; }

void FakeBackend::stop() { close(nullptr); }

void FakeBackend::run() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kStride) * kHeight);
    std::uint64_t frame = 1;
    while (!fStop.load()) {
        paint(pixels, frame);
        FrameView view;
        view.data = pixels.data();
        view.width = kWidth;
        view.height = kHeight;
        view.stride = kStride;
        view.format = static_cast<std::uint32_t>(FrameFormat::BGRA8);
        view.flags = kFrameFlagAlpha;
        if (!fRing || !fRing->write(view, nullptr)) {
            break;
        }
        ++frame;
        std::this_thread::sleep_for(kPeriod);
    }
}

}  // namespace openshow::live
