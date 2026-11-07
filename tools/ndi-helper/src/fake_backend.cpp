#include "fake_backend.h"

#include <chrono>
#include <cstring>

namespace openshow::ndi {

namespace {
constexpr std::uint32_t kFakeWidth = 320;
constexpr std::uint32_t kFakeHeight = 180;
constexpr std::uint32_t kFakeStride = kFakeWidth * 4;
constexpr double kFakeFps = 30.0;
constexpr auto kFakePeriod = std::chrono::milliseconds(33);

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

FakeBackend::~FakeBackend() { close(nullptr); }

bool FakeBackend::probe(ProbeInfo* out, std::string* error) {
    if (!out) {
        if (error) {
            *error = "probe: no output";
        }
        return false;
    }
    out->helperVersion = kHelperVersion;
    out->protocolVersion = kProtocolVersion;
    out->ndiVersion = "fake";
    out->canList = true;
    out->canReceive = true;
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
    out->push_back({"Fake camera 1", "fake://camera1"});
    out->push_back({"Fake camera 2", "fake://camera2"});
    return true;
}

bool FakeBackend::open(const std::string& source, OpenInfo* out, std::string* error) {
    if (!out) {
        if (error) {
            *error = "open: no output";
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(fMutex);
    if (fStreaming.load()) {
        if (error) {
            *error = "open: a source is already open";
        }
        return false;
    }
    fSource = source;
    if (!fRing.create(kFakeWidth, kFakeHeight, kFakeStride,
                      static_cast<std::uint32_t>(openshow::live::FrameFormat::BGRA8), error)) {
        return false;
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kFakeStride) * kFakeHeight);
    paint(pixels, kFakeWidth, kFakeHeight, 0, true);
    openshow::live::FrameView first;
    first.data = pixels.data();
    first.width = kFakeWidth;
    first.height = kFakeHeight;
    first.stride = kFakeStride;
    first.format = static_cast<std::uint32_t>(openshow::live::FrameFormat::BGRA8);
    first.flags = openshow::live::kFrameFlagAlpha;
    first.timestampNs = nowNs();
    if (!fRing.write(first, error)) {
        fRing.close();
        return false;
    }

    fStop.store(false);
    fStreaming.store(true);
    fThread = std::thread([this] { run(); });

    out->shmName = fRing.name();
    out->width = kFakeWidth;
    out->height = kFakeHeight;
    out->stride = kFakeStride;
    out->format = static_cast<std::uint32_t>(openshow::live::FrameFormat::BGRA8);
    out->bytes = fRing.bytes();
    out->fps = kFakeFps;
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
    fRing.close();
    fSource.clear();
    return true;
}

bool FakeBackend::streaming() const { return fStreaming.load(); }

void FakeBackend::setEventSink(std::function<void(const Event&)> sink) {
    std::lock_guard<std::mutex> lock(fMutex);
    fSink = std::move(sink);
}

void FakeBackend::run() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kFakeStride) * kFakeHeight);
    std::uint64_t frame = 1;
    while (!fStop.load()) {
        paint(pixels, kFakeWidth, kFakeHeight, frame, true);
        openshow::live::FrameView view;
        view.data = pixels.data();
        view.width = kFakeWidth;
        view.height = kFakeHeight;
        view.stride = kFakeStride;
        view.format = static_cast<std::uint32_t>(openshow::live::FrameFormat::BGRA8);
        view.flags = openshow::live::kFrameFlagAlpha;
        view.timestampNs = nowNs();
        if (!fRing.write(view, nullptr)) {
            break;
        }
        ++frame;
        std::this_thread::sleep_for(kFakePeriod);
    }
}

void FakeBackend::paint(std::vector<std::uint8_t>& pixels, std::uint32_t width,
                        std::uint32_t height, std::uint64_t frame, bool alpha) {
    const std::uint32_t bar = width / 8;
    const std::uint32_t moving = static_cast<std::uint32_t>((frame * 3) % width);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t b = 0;
            std::uint8_t g = 0;
            std::uint8_t r = 0;
            const std::uint32_t band = x / (bar == 0 ? 1 : bar);
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
            p[3] = alpha ? 255 : 0;
        }
    }
}

}  // namespace openshow::ndi
