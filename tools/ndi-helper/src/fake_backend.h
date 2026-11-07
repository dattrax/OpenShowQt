// A backend with no NDI dependency. It always builds, so the protocol and the
// frame ring can be tested without the proprietary SDK or a camera. It produces
// a colour-bar test pattern at 30 fps, in either the BGRA the old tests used or
// the packed UYVY the real NDI receiver delivers. Options can freeze the
// pattern for snapshot tests or make the source disappear so the asynchronous
// event path is covered without unplugging anything.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backend.h"
#include "shm_ring.h"

namespace openshow::ndi {

struct FakeOptions {
    // Emit UYVY (4:2:2, the real HX/full-NDI YUV shape) instead of BGRA.
    bool uyvy = false;
    // Keep the first frame's pattern for deterministic screenshots.
    bool staticFrames = false;
    // Emit source-lost after this many milliseconds; 0 never loses the source.
    int loseSourceAfterMs = 0;
};

class FakeBackend final : public Backend {
public:
    explicit FakeBackend(FakeOptions options = {});
    ~FakeBackend() override;

    bool probe(ProbeInfo* out, std::string* error) override;
    bool list(std::uint32_t waitMs, std::vector<SourceInfo>* out, std::string* error) override;
    bool open(const std::string& source, OpenInfo* out, std::string* error) override;
    bool close(std::string* error) override;
    bool streaming() const override;
    void setEventSink(std::function<void(const Event&)> sink) override;

private:
    void run();
    void emit(Event event);
    static void paint(std::vector<std::uint8_t>& pixels, std::uint32_t width,
                      std::uint32_t height, std::uint64_t frame, bool uyvy);

    FakeOptions fOptions;
    std::mutex fMutex;
    std::mutex fSinkMutex;
    std::string fSource;
    ShmRing fRing;
    std::thread fThread;
    std::atomic<bool> fStop{false};
    std::atomic<bool> fStreaming{false};
    std::function<void(const Event&)> fSink;
};

}  // namespace openshow::ndi
