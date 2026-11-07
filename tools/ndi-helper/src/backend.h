// The helper's backend seam. The fake backend always builds and is what CI
// exercises; the NDI backend builds only when the proprietary SDK is present.
// The protocol layer in main.cpp knows nothing about NDI.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace openshow::ndi {

struct SourceInfo {
    std::string name;
    std::string url;
};

struct ProbeInfo {
    std::string helperVersion;
    int protocolVersion = 1;
    std::string ndiVersion;
    bool canList = false;
    bool canReceive = false;
};

struct OpenInfo {
    std::string shmName;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = 0;
    std::uint64_t bytes = 0;
    double fps = 0.0;
};

struct Event {
    std::string kind;  // "source-lost" or "receiver-error"
    std::string source;
    std::string message;
};

class Backend {
public:
    virtual ~Backend() = default;

    virtual bool probe(ProbeInfo* out, std::string* error) = 0;
    virtual bool list(std::uint32_t waitMs, std::vector<SourceInfo>* out,
                      std::string* error) = 0;
    virtual bool open(const std::string& source, OpenInfo* out, std::string* error) = 0;
    virtual bool close(std::string* error) = 0;
    virtual bool streaming() const = 0;
    virtual void setEventSink(std::function<void(const Event&)> sink) = 0;
};

// Helper version reported by --probe and the protocol. Keep it in step with
// the helper's README and the worker's capability check.
inline constexpr const char* kHelperVersion = "0.1.0";
inline constexpr int kProtocolVersion = 1;

}  // namespace openshow::ndi
