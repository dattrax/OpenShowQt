// A backend with no NDI dependency. It always builds, so the protocol and the
// frame ring can be tested without the proprietary SDK or a camera. It produces
// a moving colour-bar test pattern at 30 fps.
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

class FakeBackend final : public Backend {
public:
    FakeBackend() = default;
    ~FakeBackend() override;

    bool probe(ProbeInfo* out, std::string* error) override;
    bool list(std::uint32_t waitMs, std::vector<SourceInfo>* out, std::string* error) override;
    bool open(const std::string& source, OpenInfo* out, std::string* error) override;
    bool close(std::string* error) override;
    bool streaming() const override;
    void setEventSink(std::function<void(const Event&)> sink) override;

private:
    void run();
    static void paint(std::vector<std::uint8_t>& pixels, std::uint32_t width,
                      std::uint32_t height, std::uint64_t frame, bool alpha);

    std::mutex fMutex;
    std::string fSource;
    ShmRing fRing;
    std::thread fThread;
    std::atomic<bool> fStop{false};
    std::atomic<bool> fStreaming{false};
    std::function<void(const Event&)> fSink;
};

}  // namespace openshow::ndi
