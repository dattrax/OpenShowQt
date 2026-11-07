/*
 * The worker's client for the separate NDI helper (ND.3). It launches the
 * helper process, speaks the line-delimited JSON protocol on its stdin/stdout
 * and maps the frame ring it publishes. No NDI headers, no NDI symbols: this is
 * ordinary GPL worker code and the licence boundary is the process itself.
 *
 * A missing or dead helper is a start/request error for the caller; it never
 * takes the worker down with it.
 */

#pragma once

#include <sys/types.h>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "live/live_source_backend.h"
#include "live/shm_frame_reader.h"

namespace openshow::live {

struct HelperOptions {
    // Empty resolves OPENSHOW_NDI_HELPER, then "ndi-helper" on PATH.
    std::string executable;
    std::vector<std::string> arguments;
    int startTimeoutMs = 5000;
    int requestTimeoutMs = 5000;
    // The helper waits up to 10 s for a first frame before answering `open`.
    int openTimeoutMs = 15000;
    int stopTimeoutMs = 2000;
};

class HelperBackend final : public LiveSourceBackend {
public:
    explicit HelperBackend(HelperOptions options = {});
    ~HelperBackend() override;

    HelperBackend(const HelperBackend&) = delete;
    HelperBackend& operator=(const HelperBackend&) = delete;

    bool start(std::string* error) override;
    bool probe(std::string* ndiVersion, std::string* error) override;
    bool list(std::uint32_t waitMs, std::vector<SourceInfo>* out, std::string* error) override;
    bool open(const std::string& source, OpenInfo* out, std::string* error) override;
    bool close(std::string* error) override;
    bool streaming() const override;
    FrameStatus readFrame(std::uint64_t afterSequence, Frame* frame, std::string* error) override;
    bool takeEvent(Event* out) override;
    void stop() override;
    const char* backendName() const override { return "ndi"; }

    // The helper's process id while running, else -1. For lifecycle tests.
    pid_t processId() const;

private:
    bool ensureStarted(std::string* error);
    bool request(const std::string& commandLine, std::string* reply, std::string* error,
                 int timeoutMs);
    bool writeAll(const std::string& data, int timeoutMs, std::string* error);
    bool readLine(std::string* line, int timeoutMs, std::string* error);
    void pollEvents();
    bool routeEventLine(const std::string& line);
    void shutdownProcess();

    HelperOptions fOptions;
    pid_t fPid = -1;
    int fWriteFd = -1;
    int fReadFd = -1;
    std::string fReadBuffer;
    std::deque<Event> fEvents;
    std::string fNdiVersion;
    bool fProbed = false;
    std::unique_ptr<ShmFrameReader> fReader;
    OpenInfo fOpen;
    bool fOpenValid = false;
    mutable std::mutex fMutex;
};

}  // namespace openshow::live
