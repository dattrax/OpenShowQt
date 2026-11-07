/*
 * The worker's seam for live inputs (ND.3). HelperBackend speaks the separate
 * helper's JSON protocol and maps its frame ring; FakeBackend produces frames
 * in-process for tests. Both are ordinary worker code with no NDI headers or
 * symbols — the licence boundary in docs/ndi-camera-input.md depends on that.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "live/live_source.h"

namespace openshow::live {

class LiveSourceBackend {
public:
    virtual ~LiveSourceBackend() = default;

    // Starts the transport (a helper process, for HelperBackend). Idempotent.
    virtual bool start(std::string* error) = 0;

    // Reports the live input version. Starts the transport when needed; this is
    // the cheap capability probe the frontend asks for.
    virtual bool probe(std::string* ndiVersion, std::string* error) = 0;

    // Discovers sources, waiting up to `waitMs` for a new one to appear.
    virtual bool list(std::uint32_t waitMs, std::vector<SourceInfo>* out,
                      std::string* error) = 0;

    // Opens `source` and maps its frame ring. Only one source is open at a
    // time; opening again while streaming is refused.
    virtual bool open(const std::string& source, OpenInfo* out, std::string* error) = 0;

    // Stops the receiver and unmaps the ring. Safe when nothing is open.
    virtual bool close(std::string* error) = 0;

    virtual bool streaming() const = 0;

    // Copies the newest frame when it is newer than `afterSequence`. The
    // caller reuses `frame` across calls.
    virtual FrameStatus readFrame(std::uint64_t afterSequence, Frame* frame,
                                  std::string* error) = 0;

    // Pops one queued asynchronous event, if any.
    virtual bool takeEvent(Event* out) = 0;

    // Stops the transport and releases its process. Idempotent; the destructor
    // calls it.
    virtual void stop() {}

    virtual const char* backendName() const = 0;
};

}  // namespace openshow::live
