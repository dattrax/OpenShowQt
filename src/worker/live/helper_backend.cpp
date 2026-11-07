#include "live/helper_backend.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>

#include <boost/json.hpp>

#include "openshow/live/frame_ring.h"

extern char** environ;

namespace openshow::live {

namespace {

constexpr std::size_t kMaxLineBytes = 64 * 1024;

using Clock = std::chrono::steady_clock;

bool fail(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

// Writes to a pipe without letting a dead helper raise SIGPIPE in the worker.
// SIGPIPE is blocked only on this thread and any pending instance is consumed
// before the old mask returns.
ssize_t writeNoSigPipe(int fd, const void* data, std::size_t size) {
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    const ssize_t written = ::write(fd, data, size);
    if (written < 0 && errno == EPIPE) {
        sigset_t pending;
        sigemptyset(&pending);
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
            struct timespec zero = {0, 0};
            sigtimedwait(&blocked, nullptr, &zero);
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return written;
}

std::uint32_t formatFromName(const std::string& name) {
    if (name == "BGRA" || name.empty()) {
        return static_cast<std::uint32_t>(FrameFormat::BGRA8);
    }
    if (name == "RGBA") {
        return static_cast<std::uint32_t>(FrameFormat::RGBA8);
    }
    if (name == "UYVY") {
        return static_cast<std::uint32_t>(FrameFormat::UYVY);
    }
    return static_cast<std::uint32_t>(FrameFormat::BGRA8);
}

std::string resolveExecutable(const HelperOptions& options) {
    if (!options.executable.empty()) {
        return options.executable;
    }
    if (const char* env = std::getenv("OPENSHOW_NDI_HELPER"); env && *env) {
        return env;
    }
    return "ndi-helper";
}

bool jsonNumber(const boost::json::object& object, const char* key, std::uint64_t* out) {
    const boost::json::value* value = object.if_contains(key);
    if (!value) {
        return false;
    }
    if (value->is_int64()) {
        const long long number = value->as_int64();
        if (number < 0) {
            return false;
        }
        if (out) {
            *out = static_cast<std::uint64_t>(number);
        }
        return true;
    }
    if (value->is_uint64()) {
        if (out) {
            *out = value->as_uint64();
        }
        return true;
    }
    if (value->is_double()) {
        const double number = value->get_double();
        if (number < 0.0) {
            return false;
        }
        if (out) {
            *out = static_cast<std::uint64_t>(number);
        }
        return true;
    }
    return false;
}

bool jsonDouble(const boost::json::object& object, const char* key, double* out) {
    const boost::json::value* value = object.if_contains(key);
    if (!value) {
        return false;
    }
    if (value->is_int64()) {
        if (out) {
            *out = static_cast<double>(value->get_int64());
        }
        return true;
    }
    if (value->is_uint64()) {
        if (out) {
            *out = static_cast<double>(value->get_uint64());
        }
        return true;
    }
    if (value->is_double()) {
        if (out) {
            *out = value->get_double();
        }
        return true;
    }
    return false;
}

bool jsonOk(const boost::json::object& object) {
    const boost::json::value* ok = object.if_contains("ok");
    return ok && ok->is_bool() && ok->as_bool();
}

std::string replyError(const boost::json::object& object, const std::string& fallback) {
    const boost::json::value* message = object.if_contains("error");
    if (message && message->is_string()) {
        return std::string(message->as_string());
    }
    return fallback;
}

}  // namespace

HelperBackend::HelperBackend(HelperOptions options) : fOptions(std::move(options)) {}

HelperBackend::~HelperBackend() { stop(); }

pid_t HelperBackend::processId() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fPid;
}

bool HelperBackend::start(std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    return ensureStarted(error);
}

bool HelperBackend::probe(std::string* ndiVersion, std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!ensureStarted(error)) {
        return false;
    }
    if (!ndiVersion) {
        return fail(error, "probe: no output");
    }
    *ndiVersion = fNdiVersion;
    return true;
}

bool HelperBackend::ensureStarted(std::string* error) {
    if (fPid > 0) {
        return true;
    }

    const std::string executable = resolveExecutable(fOptions);
    if (executable.empty()) {
        return fail(error, "NDI helper: no executable configured");
    }

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (::pipe2(inPipe, O_CLOEXEC) != 0) {
        return fail(error, std::string("NDI helper: cannot create its input pipe: ") +
                               std::strerror(errno));
    }
    if (::pipe2(outPipe, O_CLOEXEC) != 0) {
        const std::string message = std::strerror(errno);
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        return fail(error, "NDI helper: cannot create its output pipe: " + message);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, inPipe[0]);
    posix_spawn_file_actions_addclose(&actions, inPipe[1]);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);

    std::vector<std::string> arguments;
    arguments.push_back(executable);
    for (const std::string& argument : fOptions.arguments) {
        arguments.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int spawned = ::posix_spawnp(&pid, executable.c_str(), &actions, nullptr,
                                       argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(inPipe[0]);
    ::close(outPipe[1]);
    if (spawned != 0) {
        const std::string message = std::strerror(spawned);
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        return fail(error, "NDI helper: cannot start '" + executable + "': " + message);
    }

    fPid = pid;
    fWriteFd = inPipe[1];
    fReadFd = outPipe[0];
    fReadBuffer.clear();
    fEvents.clear();
    fProbed = false;
    fNdiVersion.clear();

    std::string reply;
    std::string probeError;
    if (!request("{\"cmd\":\"probe\"}", &reply, &probeError, fOptions.startTimeoutMs)) {
        shutdownProcess();
        return fail(error, "NDI helper: probe failed: " + probeError);
    }
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(reply, parseError);
    if (parseError || !parsed.is_object()) {
        shutdownProcess();
        return fail(error, "NDI helper: probe reply is not a JSON object");
    }
    const boost::json::object& object = parsed.as_object();
    const boost::json::value* ndi = object.if_contains("ndi");
    fNdiVersion = ndi && ndi->is_string() ? std::string(ndi->as_string()) : std::string();
    fProbed = true;
    return true;
}

bool HelperBackend::request(const std::string& commandLine, std::string* reply,
                            std::string* error, int timeoutMs) {
    if (fWriteFd < 0 || fReadFd < 0) {
        return fail(error, "NDI helper is not running");
    }
    if (!writeAll(commandLine + "\n", timeoutMs, error)) {
        return false;
    }
    std::string line;
    if (!readLine(&line, timeoutMs, error)) {
        return false;
    }
    if (reply) {
        *reply = std::move(line);
    }
    return true;
}

bool HelperBackend::writeAll(const std::string& data, int timeoutMs, std::string* error) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining != 0) {
        const auto left = deadline - Clock::now();
        if (left <= Clock::duration::zero()) {
            return fail(error, "NDI helper: timed out sending a command");
        }
        struct pollfd descriptor = {fWriteFd, POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(left).count() + 1));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error, std::string("NDI helper: poll failed: ") + std::strerror(errno));
        }
        if (ready == 0) {
            return fail(error, "NDI helper: timed out sending a command");
        }
        const ssize_t written = writeNoSigPipe(fWriteFd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error, std::string("NDI helper: write failed: ") + std::strerror(errno));
        }
        if (written == 0) {
            return fail(error, "NDI helper: closed its input");
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

bool HelperBackend::readLine(std::string* line, int timeoutMs, std::string* error) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const std::size_t newline = fReadBuffer.find('\n');
        if (newline != std::string::npos) {
            std::string candidate = fReadBuffer.substr(0, newline);
            fReadBuffer.erase(0, newline + 1);
            if (!candidate.empty() && candidate.back() == '\r') {
                candidate.pop_back();
            }
            if (candidate.empty()) {
                continue;
            }
            if (routeEventLine(candidate)) {
                continue;
            }
            *line = std::move(candidate);
            return true;
        }
        if (fReadBuffer.size() > kMaxLineBytes) {
            fReadBuffer.clear();
            return fail(error, "NDI helper: reply is longer than 65536 bytes");
        }

        const auto left = deadline - Clock::now();
        if (left <= Clock::duration::zero()) {
            return fail(error, "NDI helper: did not answer in time");
        }
        struct pollfd descriptor = {fReadFd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(left).count() + 1));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error, std::string("NDI helper: poll failed: ") + std::strerror(errno));
        }
        if (ready == 0) {
            return fail(error, "NDI helper: did not answer in time");
        }
        char buffer[4096];
        const ssize_t got = ::read(fReadFd, buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(error, std::string("NDI helper: read failed: ") + std::strerror(errno));
        }
        if (got == 0) {
            return fail(error, "NDI helper: closed its output");
        }
        fReadBuffer.append(buffer, static_cast<std::size_t>(got));
    }
}

// True when `line` was an asynchronous event and has been queued. A line that
// is not valid JSON is returned to the caller as the reply, which then reports
// a typed protocol error.
bool HelperBackend::routeEventLine(const std::string& line) {
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(line, parseError);
    if (parseError || !parsed.is_object()) {
        return false;
    }
    const boost::json::object& object = parsed.as_object();
    const boost::json::value* kind = object.if_contains("event");
    if (!kind || !kind->is_string()) {
        return false;
    }
    Event event;
    event.kind = std::string(kind->as_string());
    if (const boost::json::value* source = object.if_contains("source");
        source && source->is_string()) {
        event.source = std::string(source->as_string());
    }
    if (const boost::json::value* message = object.if_contains("message");
        message && message->is_string()) {
        event.message = std::string(message->as_string());
    }
    fEvents.push_back(std::move(event));
    return true;
}

// Drains stdout without waiting, so events emitted while no request is in
// flight (a source disappearing) are seen on the next frame read. Complete
// reply-shaped lines with no request outstanding are discarded; the protocol
// has no unsolicited replies.
void HelperBackend::pollEvents() {
    if (fReadFd < 0) {
        return;
    }
    char buffer[4096];
    for (;;) {
        struct pollfd descriptor = {fReadFd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 0);
        if (ready <= 0) {
            break;
        }
        const ssize_t got = ::read(fReadFd, buffer, sizeof buffer);
        if (got <= 0) {
            break;
        }
        fReadBuffer.append(buffer, static_cast<std::size_t>(got));
    }
    std::size_t newline = fReadBuffer.find('\n');
    while (newline != std::string::npos) {
        std::string line = fReadBuffer.substr(0, newline);
        fReadBuffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            routeEventLine(line);
        }
        newline = fReadBuffer.find('\n');
    }
    if (fReadBuffer.size() > kMaxLineBytes) {
        fReadBuffer.clear();
    }
}

bool HelperBackend::list(std::uint32_t waitMs, std::vector<SourceInfo>* out,
                         std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!out) {
        return fail(error, "list: no output");
    }
    if (!ensureStarted(error)) {
        return false;
    }
    out->clear();

    boost::json::object command;
    command["cmd"] = "list";
    command["waitMs"] = waitMs;
    std::string reply;
    if (!request(boost::json::serialize(command), &reply, error, fOptions.requestTimeoutMs)) {
        return false;
    }
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(reply, parseError);
    if (parseError || !parsed.is_object()) {
        return fail(error, "list: reply is not a JSON object");
    }
    const boost::json::object& object = parsed.as_object();
    if (!jsonOk(object)) {
        return fail(error, "list refused: " + replyError(object, reply));
    }
    const boost::json::value* sources = object.if_contains("sources");
    if (!sources || !sources->is_array()) {
        return fail(error, "list: reply carries no source array");
    }
    for (const boost::json::value& item : sources->as_array()) {
        if (!item.is_object()) {
            continue;
        }
        const boost::json::object& entry = item.as_object();
        SourceInfo info;
        if (const boost::json::value* name = entry.if_contains("name");
            name && name->is_string()) {
            info.name = std::string(name->as_string());
        }
        if (const boost::json::value* url = entry.if_contains("url");
            url && url->is_string()) {
            info.url = std::string(url->as_string());
        }
        if (!info.name.empty()) {
            out->push_back(std::move(info));
        }
    }
    return true;
}

bool HelperBackend::open(const std::string& source, OpenInfo* out, std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!out) {
        return fail(error, "open: no output");
    }
    if (fOpenValid) {
        return fail(error, "open: a live source is already open");
    }
    if (source.empty()) {
        return fail(error, "open: the source name is empty");
    }
    if (!ensureStarted(error)) {
        return false;
    }

    boost::json::object command;
    command["cmd"] = "open";
    command["source"] = source;
    std::string reply;
    if (!request(boost::json::serialize(command), &reply, error, fOptions.openTimeoutMs)) {
        return false;
    }
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(reply, parseError);
    if (parseError || !parsed.is_object()) {
        return fail(error, "open: reply is not a JSON object");
    }
    const boost::json::object& object = parsed.as_object();
    if (!jsonOk(object)) {
        return fail(error, "open refused: " + replyError(object, reply));
    }

    OpenInfo info;
    if (const boost::json::value* shm = object.if_contains("shm");
        shm && shm->is_string()) {
        info.shmName = std::string(shm->as_string());
    }
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t stride = 0;
    std::uint64_t bytes = 0;
    if (info.shmName.empty() || !jsonNumber(object, "width", &width) ||
        !jsonNumber(object, "height", &height) || !jsonNumber(object, "stride", &stride)) {
        return fail(error, "open: reply is missing the frame description");
    }
    jsonNumber(object, "bytes", &bytes);
    info.width = static_cast<std::uint32_t>(width);
    info.height = static_cast<std::uint32_t>(height);
    info.stride = static_cast<std::uint32_t>(stride);
    if (const boost::json::value* format = object.if_contains("format");
        format && format->is_string()) {
        info.format = formatFromName(std::string(format->as_string()));
    }
    jsonDouble(object, "fps", &info.fps);
    info.bytes = bytes;

    auto reader = std::make_unique<ShmFrameReader>();
    std::string mapError;
    if (!reader->map(info.shmName, info.bytes, &mapError)) {
        // The helper is streaming into a ring we cannot read; ask it to stop.
        std::string ignored;
        std::string ignoredError;
        request("{\"cmd\":\"close\"}", &ignored, &ignoredError, fOptions.requestTimeoutMs);
        return fail(error, "open: " + mapError);
    }

    fReader = std::move(reader);
    fOpen = info;
    fOpenValid = true;
    *out = info;
    return true;
}

bool HelperBackend::close(std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (fReader) {
        fReader->unmap();
        fReader.reset();
    }
    fOpenValid = false;
    fOpen = OpenInfo{};
    if (fPid <= 0) {
        return true;
    }
    std::string reply;
    std::string requestError;
    if (!request("{\"cmd\":\"close\"}", &reply, &requestError, fOptions.requestTimeoutMs)) {
        return fail(error, "close: " + requestError);
    }
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(reply, parseError);
    if (parseError || !parsed.is_object()) {
        return fail(error, "close: reply is not a JSON object");
    }
    const boost::json::object& object = parsed.as_object();
    if (!jsonOk(object)) {
        return fail(error, "close refused: " + replyError(object, reply));
    }
    return true;
}

bool HelperBackend::streaming() const {
    std::lock_guard<std::mutex> lock(fMutex);
    return fOpenValid;
}

FrameStatus HelperBackend::readFrame(std::uint64_t afterSequence, Frame* frame,
                                    std::string* error) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fReader) {
        if (error) {
            error->clear();
        }
        return FrameStatus::NoFrame;
    }
    pollEvents();
    return fReader->read(afterSequence, frame, error);
}

bool HelperBackend::takeEvent(Event* out) {
    std::lock_guard<std::mutex> lock(fMutex);
    if (fEvents.empty()) {
        return false;
    }
    if (out) {
        *out = std::move(fEvents.front());
    }
    fEvents.pop_front();
    return true;
}

void HelperBackend::stop() {
    std::lock_guard<std::mutex> lock(fMutex);
    if (fReader) {
        fReader->unmap();
        fReader.reset();
    }
    fOpenValid = false;
    fOpen = OpenInfo{};
    if (fPid > 0) {
        std::string reply;
        std::string ignored;
        request("{\"cmd\":\"quit\"}", &reply, &ignored, fOptions.stopTimeoutMs);
    }
    shutdownProcess();
    fEvents.clear();
    fProbed = false;
    fNdiVersion.clear();
}

void HelperBackend::shutdownProcess() {
    if (fWriteFd >= 0) {
        ::close(fWriteFd);
        fWriteFd = -1;
    }
    if (fReadFd >= 0) {
        ::close(fReadFd);
        fReadFd = -1;
    }
    fReadBuffer.clear();
    if (fPid <= 0) {
        return;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(fOptions.stopTimeoutMs);
    int status = 0;
    while (Clock::now() < deadline) {
        const pid_t reaped = ::waitpid(fPid, &status, WNOHANG);
        if (reaped == fPid || (reaped < 0 && errno == ECHILD)) {
            fPid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(fPid, SIGKILL);
    ::waitpid(fPid, &status, 0);
    fPid = -1;
}

}  // namespace openshow::live
