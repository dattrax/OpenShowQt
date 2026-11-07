/*
 * ND.3: the worker's live-source seam. FakeBackend proves the in-process source
 * and its shared-memory ring; HelperBackend runs the MIT helper's fake backend
 * as a real child process and speaks the real protocol over pipes, so the
 * client, process lifecycle, mapping and newest-wins handoff are all exercised
 * without the NDI SDK or a camera.
 */

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "live/fake_backend.h"
#include "live/helper_backend.h"
#include "live/live_source.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::live::Frame;
using openshow::live::FrameStatus;
using openshow::live::LiveSourceBackend;

bool waitForFrame(LiveSourceBackend& backend, std::uint64_t after, Frame* frame,
                  int timeoutMs, const char* what) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string error;
        const FrameStatus status = backend.readFrame(after, frame, &error);
        if (status == FrameStatus::Frame) {
            return true;
        }
        if (status == FrameStatus::Error) {
            std::fprintf(stderr, "FAIL: %s: %s\n", what, error.c_str());
            gFailed = true;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "FAIL: %s: no frame in %d ms\n", what, timeoutMs);
    gFailed = true;
    return false;
}

bool hasPicture(const Frame& frame) {
    for (std::size_t i = 0; i + 3 < frame.pixels.size(); i += 4) {
        if (frame.pixels[i] != 0 || frame.pixels[i + 1] != 0 || frame.pixels[i + 2] != 0) {
            return true;
        }
    }
    return false;
}

void testFakeBackend() {
    using namespace openshow::live;

    FakeBackend backend;
    std::string error;
    std::string version;
    check(backend.start(&error), "fake: starts");
    check(backend.probe(&version, &error) && version == "fake", "fake: probe names the backend");
    check(std::string(backend.backendName()) == "fake", "fake: backend name");

    std::vector<SourceInfo> sources;
    check(backend.list(0, &sources, &error) && sources.size() == 2,
          "fake: lists two sources");

    OpenInfo info;
    check(backend.open("Fake NDI camera 1", &info, &error), "fake: opens a source");
    check(info.width == 320 && info.height == 180, "fake: reports its frame size");
    check(info.stride == 320 * 4, "fake: reports its stride");
    check(info.bytes == frameRingBytes(info.stride, info.height),
          "fake: reports the ring size");
    check(info.shmName.rfind("/openshow-live-fake-", 0) == 0,
          "fake: publishes a segment name");
    check(backend.streaming(), "fake: streaming after open");

    OpenInfo refused;
    check(!backend.open("Fake NDI camera 2", &refused, &error),
          "fake: a second open is refused");
    check(backend.streaming(), "fake: the refused open keeps the first source");

    Frame frame;
    if (check(waitForFrame(backend, 0, &frame, 1000, "fake: first frame"), "fake: first frame")) {
        check(frame.width == info.width && frame.height == info.height,
              "fake: the frame dimensions match open");
        check(frame.sequence >= 1, "fake: the frame carries a sequence");
        check(hasPicture(frame), "fake: the frame carries a picture");
        const std::uint64_t first = frame.sequence;
        check(waitForFrame(backend, first, &frame, 1500, "fake: newer frame"),
              "fake: a newer frame arrives");
        check(frame.sequence > first, "fake: the sequence advances");
    }

    const std::string name = info.shmName;
    check(backend.close(&error), "fake: closes");
    check(!backend.streaming(), "fake: not streaming after close");
    check(backend.readFrame(0, &frame, &error) == FrameStatus::NoFrame,
          "fake: no frame after close");
    const int gone = ::shm_open(name.c_str(), O_RDONLY, 0);
    check(gone < 0, "fake: close unlinks the segment");
    if (gone >= 0) {
        ::close(gone);
    }
    check(backend.takeEvent(nullptr) == false, "fake: no asynchronous events");
    backend.stop();
}

void testHelperBackend() {
    using namespace openshow::live;

    HelperOptions options;
    options.executable = OPENSHOW_FAKE_HELPER_BIN;
    options.arguments = {"--fake"};
    options.startTimeoutMs = 5000;
    options.requestTimeoutMs = 5000;
    options.openTimeoutMs = 15000;

    HelperBackend backend(options);
    std::string error;
    std::string version;
    check(backend.start(&error), "helper: starts the fake helper");
    check(backend.processId() > 0, "helper: reports its process id");
    check(backend.probe(&version, &error) && version == "fake",
          "helper: probe names the backend");
    check(std::string(backend.backendName()) == "ndi", "helper: backend name");

    std::vector<SourceInfo> sources;
    check(backend.list(0, &sources, &error) && sources.size() == 2,
          "helper: lists the helper's sources");
    check(!sources.empty() && sources[0].name == "Fake camera 1",
          "helper: names the first source");

    OpenInfo info;
    check(backend.open("Fake camera 1", &info, &error), "helper: opens a source");
    check(info.width == 320 && info.height == 180, "helper: reports the helper's frame size");
    check(info.shmName.rfind("/openshow-ndi-", 0) == 0,
          "helper: publishes the helper's segment name");
    check(backend.streaming(), "helper: streaming after open");

    OpenInfo refused;
    check(!backend.open("Fake camera 2", &refused, &error),
          "helper: a second open is refused");
    check(backend.streaming(), "helper: the refused open keeps the first source");

    Frame frame;
    if (check(waitForFrame(backend, 0, &frame, 2000, "helper: first frame"),
              "helper: first frame")) {
        check(frame.width == info.width && frame.height == info.height,
              "helper: the frame dimensions match open");
        check(hasPicture(frame), "helper: the frame carries a picture");
        const std::uint64_t first = frame.sequence;
        check(waitForFrame(backend, first, &frame, 2000, "helper: newer frame"),
              "helper: a newer frame arrives");
        check(frame.sequence > first, "helper: the sequence advances");
    }

    const std::string name = info.shmName;
    check(backend.close(&error), "helper: closes the source");
    check(!backend.streaming(), "helper: not streaming after close");
    const int gone = ::shm_open(name.c_str(), O_RDONLY, 0);
    check(gone < 0, "helper: close unlinks the segment");
    if (gone >= 0) {
        ::close(gone);
    }
    check(backend.close(&error), "helper: close is idempotent");

    // The process stays alive for the next source after close.
    check(backend.processId() > 0, "helper: the process survives close");
    check(backend.probe(&version, &error) && version == "fake",
          "helper: probe works after close");

    Event event;
    check(!backend.takeEvent(&event), "helper: no asynchronous events in a clean run");

    backend.stop();
    check(backend.processId() == -1, "helper: stop reaps the process");
}

void testHelperFailures() {
    using namespace openshow::live;

    HelperOptions missing;
    missing.executable = "/nonexistent/openshow-ndi-helper";
    missing.startTimeoutMs = 1000;
    HelperBackend notThere(missing);
    std::string error;
    check(!notThere.start(&error), "helper failure: a missing executable fails to start");
    check(error.find("cannot start") != std::string::npos,
          "helper failure: the error names the start failure");

    HelperOptions immediate;
    immediate.executable = "/bin/true";
    immediate.startTimeoutMs = 1000;
    HelperBackend dying(immediate);
    error.clear();
    check(!dying.start(&error), "helper failure: an exiting helper fails the probe");
    check(error.find("probe failed") != std::string::npos,
          "helper failure: the error names the probe");
    check(dying.processId() == -1, "helper failure: the dead process is reaped");
}

}  // namespace

int main() {
    testFakeBackend();
    testHelperBackend();
    testHelperFailures();

    if (gFailed) {
        std::fprintf(stderr, "live_source_tests: FAILED\n");
        return 1;
    }
    std::printf("live_source_tests: all checks passed\n");
    return 0;
}
