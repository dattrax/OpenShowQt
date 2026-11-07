// The helper's line protocol: one JSON object per line on stdin, one JSON
// object per line on stdout. stdout carries protocol objects only; diagnostics
// go to stderr. The server is backend-agnostic so the fake backend and the real
// NDI backend drive the same code, and it can be unit-tested with streams.
#pragma once

#include <atomic>
#include <cstddef>
#include <iosfwd>
#include <mutex>
#include <string>

#include "backend.h"
#include "json.h"

namespace openshow::ndi {

class ProtocolServer {
public:
    ProtocolServer(Backend& backend, std::istream& in, std::ostream& out);
    ~ProtocolServer();

    ProtocolServer(const ProtocolServer&) = delete;
    ProtocolServer& operator=(const ProtocolServer&) = delete;

    // Reads commands until EOF or `quit`. Returns 0 on a clean finish.
    int run();

    // A command longer than this is discarded and refused without exiting.
    static constexpr std::size_t kMaxLineBytes = 64 * 1024;

private:
    enum class LineResult { Ok, TooLong, End };

    LineResult readLine(std::string* line);
    void handleLine(const std::string& line);
    void respond(const json::Value& object);
    void respondOk();
    void respondError(const std::string& message);
    void eventSink(const Event& event);

    Backend& fBackend;
    std::istream& fIn;
    std::ostream& fOut;
    std::mutex fOutMutex;
    std::atomic<bool> fQuit{false};
    std::string fSource;
    OpenInfo fInfo;
};

}  // namespace openshow::ndi
