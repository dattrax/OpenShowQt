#include "fake_backend.h"

#include <chrono>
#include <cstring>

namespace openshow::ndi {

namespace {
constexpr std::uint32_t kFakeWidth = 320;
constexpr std::uint32_t kFakeHeight = 180;
constexpr double kFakeFps = 30.0;
constexpr auto kFakePeriod = std::chrono::milliseconds(33);

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::uint8_t clampByte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

void barColour(std::uint32_t x, std::uint32_t frame, std::uint8_t* b,
               std::uint8_t* g, std::uint8_t* r) {
    const std::uint32_t bar = kFakeWidth / 8;
    const std::uint32_t moving =
        static_cast<std::uint32_t>((frame * 3) % kFakeWidth);
    const std::uint32_t band = x / (bar == 0 ? 1 : bar);
    switch (band % 8) {
        case 0: *b = 235; *g = 235; *r = 235; break;
        case 1: *b = 235; *g = 235; *r = 40; break;
        case 2: *b = 40; *g = 235; *r = 235; break;
        case 3: *b = 40; *g = 235; *r = 40; break;
        case 4: *b = 235; *g = 40; *r = 235; break;
        case 5: *b = 40; *g = 40; *r = 235; break;
        case 6: *b = 235; *g = 40; *r = 40; break;
        default: *b = 40; *g = 40; *r = 40; break;
    }
    if (x >= moving && x < moving + 8) {
        *b = 255;
        *g = 255;
        *r = 255;
    }
}

}  // namespace

FakeBackend::FakeBackend(FakeOptions options) : fOptions(options) {}
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
    const std::uint32_t format = static_cast<std::uint32_t>(
        fOptions.uyvy ? openshow::live::FrameFormat::UYVY
                      : openshow::live::FrameFormat::BGRA8);
    const std::uint32_t stride =
        kFakeWidth * (fOptions.uyvy ? 2u : 4u);
    if (!fRing.create(kFakeWidth, kFakeHeight, stride, format, error)) {
        return false;
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * kFakeHeight);
    paint(pixels, kFakeWidth, kFakeHeight, 0, fOptions.uyvy);
    openshow::live::FrameView first;
    first.data = pixels.data();
    first.width = kFakeWidth;
    first.height = kFakeHeight;
    first.stride = stride;
    first.format = format;
    first.flags = fOptions.uyvy ? 0 : openshow::live::kFrameFlagAlpha;
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
    out->stride = stride;
    out->format = format;
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
    } else if (fThread.joinable()) {
        fThread.join();
    }
    fRing.close();
    fSource.clear();
    return true;
}

bool FakeBackend::streaming() const { return fStreaming.load(); }

void FakeBackend::setEventSink(std::function<void(const Event&)> sink) {
    std::lock_guard<std::mutex> lock(fSinkMutex);
    fSink = std::move(sink);
}

void FakeBackend::emit(Event event) {
    std::function<void(const Event&)> sink;
    {
        std::lock_guard<std::mutex> lock(fSinkMutex);
        sink = fSink;
    }
    if (sink) {
        sink(event);
    }
}

void FakeBackend::run() {
    const std::uint32_t stride = kFakeWidth * (fOptions.uyvy ? 2u : 4u);
    const std::uint32_t format = static_cast<std::uint32_t>(
        fOptions.uyvy ? openshow::live::FrameFormat::UYVY
                      : openshow::live::FrameFormat::BGRA8);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * kFakeHeight);
    std::uint64_t frame = 1;
    const auto start = std::chrono::steady_clock::now();
    while (!fStop.load()) {
        if (fOptions.loseSourceAfterMs > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (elapsed >= fOptions.loseSourceAfterMs) {
                emit({"source-lost", fSource, "the fake source stopped"});
                fStreaming.store(false);
                return;
            }
        }
        paint(pixels, kFakeWidth, kFakeHeight, fOptions.staticFrames ? 0 : frame,
              fOptions.uyvy);
        openshow::live::FrameView view;
        view.data = pixels.data();
        view.width = kFakeWidth;
        view.height = kFakeHeight;
        view.stride = stride;
        view.format = format;
        view.flags = fOptions.uyvy ? 0 : openshow::live::kFrameFlagAlpha;
        view.timestampNs = nowNs();
        if (!fRing.write(view, nullptr)) {
            break;
        }
        if (!fOptions.staticFrames) {
            ++frame;
        }
        std::this_thread::sleep_for(kFakePeriod);
    }
}

void FakeBackend::paint(std::vector<std::uint8_t>& pixels, std::uint32_t width,
                        std::uint32_t height, std::uint64_t frame, bool uyvy) {
    const std::uint32_t stride = width * (uyvy ? 2u : 4u);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * stride;
        if (!uyvy) {
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint8_t b = 0;
                std::uint8_t g = 0;
                std::uint8_t r = 0;
                barColour(x, static_cast<std::uint32_t>(frame), &b, &g, &r);
                std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
                p[0] = b;
                p[1] = g;
                p[2] = r;
                p[3] = 255;
            }
            continue;
        }
        for (std::uint32_t x = 0; x < width; x += 2) {
            std::uint8_t b0 = 0;
            std::uint8_t g0 = 0;
            std::uint8_t r0 = 0;
            std::uint8_t b1 = 0;
            std::uint8_t g1 = 0;
            std::uint8_t r1 = 0;
            barColour(x, static_cast<std::uint32_t>(frame), &b0, &g0, &r0);
            barColour(x + 1, static_cast<std::uint32_t>(frame), &b1, &g1, &r1);
            // BT.601 full-range-ish integers; the fake only needs a plausible
            // picture, and the real conversion path is exercised downstream.
            const std::uint8_t y0 = clampByte((77 * r0 + 150 * g0 + 29 * b0) >> 8);
            const std::uint8_t y1 = clampByte((77 * r1 + 150 * g1 + 29 * b1) >> 8);
            const std::uint8_t u = clampByte(
                128 + ((-43 * (r0 + r1) - 85 * (g0 + g1) + 128 * (b0 + b1)) >> 9));
            const std::uint8_t v = clampByte(
                128 + ((128 * (r0 + r1) - 107 * (g0 + g1) - 21 * (b0 + b1)) >> 9));
            std::uint8_t* p = row + static_cast<std::size_t>(x) * 2;
            p[0] = u;
            p[1] = y0;
            p[2] = v;
            p[3] = y1;
        }
    }
}

}  // namespace openshow::ndi
