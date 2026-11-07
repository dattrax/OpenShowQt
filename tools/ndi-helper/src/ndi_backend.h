// The real NDI backend. Compiled only when the NDI SDK headers are present.
// It dynamically loads the NDI runtime (libndi.so / ProcessingNDI.dll), so the
// helper does not link the proprietary library and can ship without it.
#pragma once

#include <memory>

#include "backend.h"

namespace openshow::ndi {

class NdiBackend final : public Backend {
public:
    NdiBackend();
    ~NdiBackend() override;

    bool probe(ProbeInfo* out, std::string* error) override;
    bool list(std::uint32_t waitMs, std::vector<SourceInfo>* out, std::string* error) override;
    bool open(const std::string& source, OpenInfo* out, std::string* error) override;
    bool close(std::string* error) override;
    bool streaming() const override;
    void setEventSink(std::function<void(const Event&)> sink) override;

private:
    struct Impl;
    std::unique_ptr<Impl> fImpl;
};

}  // namespace openshow::ndi
