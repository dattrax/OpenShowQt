/*
 * In-process fake live source for worker tests and headless journeys (ND.3).
 * It publishes the same POSIX frame ring the helper does, but produces a moving
 * colour-bar pattern on a thread with no NDI SDK, no helper and no camera.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "live/live_source_backend.h"
#include "openshow/live/frame_ring.h"

namespace openshow::live {

class FakeBackend final : public LiveSourceBackend {
public:
    FakeBackend() = default;
    ~FakeBackend() override;

    bool start(std::string* error) override;
    bool probe(std::string* ndiVersion, std::string* error) override;
    bool list(std::uint32_t waitMs, std::vector<SourceInfo>* out, std::string* error) override;
    bool open(const std::string& source, OpenInfo* out, std::string* error) override;
    bool close(std::string* error) override;
    bool streaming() const override;
    FrameStatus readFrame(std::uint64_t afterSequence, Frame* frame, std::string* error) override;
    bool takeEvent(Event* out) override;
    void stop() override;
    const char* backendName() const override { return "fake"; }

private:
    bool createSegment(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                       std::string* error);
    void destroySegment();
    void run();

    mutable std::mutex fMutex;
    std::string fSource;
    std::string fName;
    void* fMap = nullptr;
    std::size_t fBytes = 0;
    std::unique_ptr<FrameRing> fRing;
    std::thread fThread;
    std::atomic<bool> fStop{false};
    std::atomic<bool> fStreaming{false};
};

}  // namespace openshow::live
