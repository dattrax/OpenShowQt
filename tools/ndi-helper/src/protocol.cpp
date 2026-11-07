#include "protocol.h"

#include <algorithm>
#include <istream>
#include <ostream>

#include "openshow/live/frame_ring.h"

namespace openshow::ndi {

namespace {

json::Value makeError(const std::string& message) {
    json::Value object = json::Value::makeObject();
    object.object.push_back({"ok", json::Value::makeBool(false)});
    object.object.push_back({"error", json::Value::makeString(message)});
    return object;
}

json::Value makeOk() {
    json::Value object = json::Value::makeObject();
    object.object.push_back({"ok", json::Value::makeBool(true)});
    return object;
}

}  // namespace

ProtocolServer::ProtocolServer(Backend& backend, std::istream& in, std::ostream& out)
    : fBackend(backend), fIn(in), fOut(out) {
    fBackend.setEventSink(
        [this](const Event& event) { eventSink(event); });
}

ProtocolServer::~ProtocolServer() { fBackend.setEventSink(nullptr); }

int ProtocolServer::run() {
    std::string line;
    while (!fQuit.load()) {
        const LineResult result = readLine(&line);
        if (result == LineResult::End) {
            return 0;
        }
        if (result == LineResult::TooLong) {
            respondError("command line is longer than 65536 bytes");
            continue;
        }
        handleLine(line);
    }
    return 0;
}

// Reads one line, bounded. A line that exceeds kMaxLineBytes is drained to the
// next newline and reported as TooLong; the server stays within its memory
// budget whatever the peer sends. A final line without a newline is accepted.
ProtocolServer::LineResult ProtocolServer::readLine(std::string* line) {
    line->clear();
    bool any = false;
    bool tooLong = false;
    while (true) {
        const int next = fIn.get();
        if (next == std::char_traits<char>::eof()) {
            if (tooLong) {
                return LineResult::TooLong;
            }
            return any ? LineResult::Ok : LineResult::End;
        }
        any = true;
        const char c = static_cast<char>(next);
        if (c == '\n') {
            if (tooLong) {
                return LineResult::TooLong;
            }
            return LineResult::Ok;
        }
        if (tooLong) {
            continue;
        }
        if (line->size() >= kMaxLineBytes) {
            tooLong = true;
            line->clear();
            line->shrink_to_fit();
            continue;
        }
        if (c != '\r') {
            line->push_back(c);
        }
    }
}

void ProtocolServer::handleLine(const std::string& line) {
    std::string trimmed = line;
    const auto first = trimmed.find_first_not_of(" \t");
    if (first == std::string::npos) {
        respondError("empty command");
        return;
    }
    if (first != 0) {
        trimmed.erase(0, first);
    }

    json::Value command;
    std::string parseError;
    if (!json::parse(trimmed, &command, &parseError)) {
        respondError("invalid JSON: " + parseError);
        return;
    }
    if (command.type != json::Value::Type::Object) {
        respondError("command must be a JSON object");
        return;
    }

    // Version negotiation: a peer that names a protocol version gets its own
    // version echoed only when the helper speaks it.
    if (const json::Value* version = json::find(command, "protocol")) {
        if (version->type != json::Value::Type::Number) {
            respondError("protocol must be a number");
            return;
        }
        double wanted = 0.0;
        if (!json::numberOr(command, "protocol", &wanted) ||
            static_cast<int>(wanted) != wanted || static_cast<int>(wanted) != kProtocolVersion) {
            respondError("unsupported protocol version (this helper speaks " +
                         std::to_string(kProtocolVersion) + ")");
            return;
        }
    }

    const std::string cmd = json::stringOr(command, "cmd");
    if (cmd.empty()) {
        respondError("missing 'cmd'");
        return;
    }

    if (cmd == "probe") {
        ProbeInfo info;
        std::string error;
        if (!fBackend.probe(&info, &error)) {
            respondError(error.empty() ? "probe failed" : error);
            return;
        }
        json::Value object = makeOk();
        object.object.push_back({"helper", json::Value::makeString(info.helperVersion)});
        object.object.push_back({"protocol", json::Value::makeInteger(info.protocolVersion)});
        object.object.push_back({"ndi", json::Value::makeString(info.ndiVersion)});
        object.object.push_back({"canList", json::Value::makeBool(info.canList)});
        object.object.push_back({"canReceive", json::Value::makeBool(info.canReceive)});
        respond(object);
        return;
    }

    if (cmd == "list") {
        std::uint32_t waitMs = 0;
        double requested = 0.0;
        if (json::numberOr(command, "waitMs", &requested)) {
            if (requested < 0) {
                respondError("waitMs must not be negative");
                return;
            }
            waitMs = static_cast<std::uint32_t>(std::min(requested, 10000.0));
        }
        std::vector<SourceInfo> sources;
        std::string error;
        if (!fBackend.list(waitMs, &sources, &error)) {
            respondError(error.empty() ? "list failed" : error);
            return;
        }
        json::Value array = json::Value::makeArray();
        for (const SourceInfo& source : sources) {
            json::Value item = json::Value::makeObject();
            item.object.push_back({"name", json::Value::makeString(source.name)});
            item.object.push_back({"url", json::Value::makeString(source.url)});
            array.array.push_back(std::move(item));
        }
        json::Value object = makeOk();
        object.object.push_back({"sources", std::move(array)});
        respond(object);
        return;
    }

    if (cmd == "open") {
        const std::string source = json::stringOr(command, "source");
        if (source.empty()) {
            respondError("open needs a non-empty 'source'");
            return;
        }
        OpenInfo info;
        std::string error;
        if (!fBackend.open(source, &info, &error)) {
            respondError(error.empty() ? "open failed" : error);
            return;
        }
        fSource = source;
        fInfo = info;
        json::Value object = makeOk();
        object.object.push_back({"source", json::Value::makeString(source)});
        object.object.push_back({"shm", json::Value::makeString(info.shmName)});
        object.object.push_back({"width", json::Value::makeInteger(info.width)});
        object.object.push_back({"height", json::Value::makeInteger(info.height)});
        object.object.push_back({"stride", json::Value::makeInteger(info.stride)});
        object.object.push_back(
            {"format", json::Value::makeString(openshow::live::frameFormatName(info.format))});
        object.object.push_back({"bytes", json::Value::makeInteger(
                                             static_cast<long long>(info.bytes))});
        object.object.push_back({"fps", json::Value::makeNumber(info.fps)});
        respond(object);
        return;
    }

    if (cmd == "close" || cmd == "stop") {
        std::string error;
        if (!fBackend.close(&error)) {
            respondError(error.empty() ? "close failed" : error);
            return;
        }
        fSource.clear();
        fInfo = OpenInfo{};
        respondOk();
        return;
    }

    if (cmd == "status") {
        json::Value object = makeOk();
        const bool streaming = fBackend.streaming();
        object.object.push_back({"open", json::Value::makeBool(streaming)});
        if (streaming) {
            object.object.push_back({"source", json::Value::makeString(fSource)});
            object.object.push_back({"shm", json::Value::makeString(fInfo.shmName)});
            object.object.push_back({"width", json::Value::makeInteger(fInfo.width)});
            object.object.push_back({"height", json::Value::makeInteger(fInfo.height)});
            object.object.push_back({"stride", json::Value::makeInteger(fInfo.stride)});
            object.object.push_back(
                {"format", json::Value::makeString(openshow::live::frameFormatName(fInfo.format))});
            object.object.push_back({"fps", json::Value::makeNumber(fInfo.fps)});
        }
        respond(object);
        return;
    }

    if (cmd == "quit") {
        fBackend.close(nullptr);
        respondOk();
        fQuit.store(true);
        return;
    }

    respondError("unknown command: " + cmd);
}

void ProtocolServer::respond(const json::Value& object) {
    const std::string line = json::write(object);
    std::lock_guard<std::mutex> lock(fOutMutex);
    fOut << line << '\n';
    fOut.flush();
}

void ProtocolServer::respondOk() { respond(makeOk()); }

void ProtocolServer::respondError(const std::string& message) { respond(makeError(message)); }

// Called from the backend's receiver thread. The output lock serialises it with
// command responses, so a line is never interleaved.
void ProtocolServer::eventSink(const Event& event) {
    json::Value object = json::Value::makeObject();
    object.object.push_back({"event", json::Value::makeString(event.kind)});
    object.object.push_back({"source", json::Value::makeString(event.source)});
    if (!event.message.empty()) {
        object.object.push_back({"message", json::Value::makeString(event.message)});
    }
    respond(object);
}

}  // namespace openshow::ndi
