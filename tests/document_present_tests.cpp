/*
 * Task 3.3 acceptance, end to end: a newly created song can be presented.
 *
 * DocumentController — the very class the lyric panel binds to — creates a
 * song and types two slides; the compiled scene JSON then travels the
 * WorkerController hand-off (scene.prepare, then scene.present on the
 * acknowledgement) to a real openshow-worker over a real socket, exactly
 * as the panel drives it. The worker's output.status must report each
 * revision on air — that is "presented".
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display.
 */

#include <boost/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <QCoreApplication>

#include "openshow/protocol/protocol.h"

#include "DocumentController.h"

namespace {

namespace proto = openshow::protocol;
using openshow::ui::DocumentController;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::string gWorkerBinary = "./openshow-worker";
const std::string gSocket =
    "/tmp/openshow-present-" + std::to_string(getpid()) + ".sock";

pid_t spawnWorker() {
    const pid_t pid = fork();
    if (pid == 0) {
        execl(gWorkerBinary.c_str(), "openshow-worker", "--socket", gSocket.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid;
}

int connectRetry() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, gSocket.c_str(), sizeof(address.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
}

proto::Decoder gDecoder;

bool call(int fd, std::int64_t id, const char* method, boost::json::object params,
          boost::json::value* reply) {
    std::string wire;
    proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire);
    if (::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) < 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pending{fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 100);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready > 0) {
            char buffer[4096];
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return false;
            }
            gDecoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        }
        std::string error;
        boost::json::value message;
        while (gDecoder.next(&message, &error)) {
            if (message.is_object() && !message.as_object().contains("method")) {
                const auto found = message.as_object().find("id");
                if (found != message.as_object().end() && found->value().is_int64() &&
                    found->value().as_int64() == id) {
                    *reply = std::move(message);
                    return true;
                }
            }
        }
    }
    return false;
}

std::uint64_t numberValue(const boost::json::value& value) {
    return value.is_uint64() ? value.as_uint64()
                             : static_cast<std::uint64_t>(value.as_int64());
}

// The panel's one-call hand-off: prepare the compiled JSON and present
// the revision the worker acknowledges (WorkerController::
// presentSceneJson does exactly this with fAutoPresentScene). Returns the
// revision on presentation, 0 on failure.
std::uint64_t presentJson(int fd, std::int64_t& nextId, const QString& sceneJson) {
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(sceneJson.toStdString(), parseError);
    if (parseError || !parsed.is_object()) {
        return 0;
    }
    boost::json::value reply;
    if (!call(fd, nextId++, "scene.prepare", parsed.as_object(), &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
    const std::uint64_t revision =
        numberValue(reply.at("result").as_object().at("revision"));
    if (!call(fd, nextId++, "scene.present",
              boost::json::object{{"revision", revision}}, &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
    // Wait for output.status to report the revision actually on air.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::json::value statusReply;
        if (call(fd, nextId++, "output.status", {}, &statusReply)) {
            const boost::json::value& status = statusReply.at("result");
            if (status.as_object().contains("scene") &&
                status.at("scene").as_object().contains("active") &&
                numberValue(status.at("scene").as_object().at("active")) == revision) {
                return revision;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }

    const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                          ("openshow-present-" + std::to_string(getpid()));
    std::filesystem::remove_all(scratch);
    ::unlink(gSocket.c_str());

    const pid_t worker = spawnWorker();
    const int fd = connectRetry();
    if (!check(fd >= 0, "connected to a live worker")) {
        kill(worker, SIGKILL);
        waitpid(worker, nullptr, 0);
        std::filesystem::remove_all(scratch);
        return 1;
    }

    std::int64_t nextId = 1;
    boost::json::value reply;
    check(call(fd, nextId++, "hello", proto::helloParams("present-test", 0, 1), &reply) &&
              reply.as_object().contains("result"),
          "handshake completed");

    // --- the acceptance act: create a song, type lyrics, present them ---
    DocumentController docs(nullptr, scratch);
    check(docs.createSong(QStringLiteral("Just As I Am")), "the song is created");
    check(docs.addSlide(), "the first slide is added");
    docs.setSlideText(QStringLiteral("Just as I am, without one plea\nBut that thy blood was shed for me"));
    docs.flushText();

    const QString first = docs.currentSceneJson();
    check(!first.isEmpty(), "the new slide compiles to a scene");
    check(presentJson(fd, nextId, first) != 0,
          "the newly created slide is presented and reports itself on air");

    // Navigation and the second slide: the same loop an operator runs.
    check(docs.addSlide(), "the second slide is added");
    docs.setSlideText(QStringLiteral("And that thou bidst me come to thee\nO Lamb of God, I come"));
    docs.flushText();
    const QString second = docs.currentSceneJson();
    check(!second.isEmpty() && second != first, "the second slide compiles differently");
    check(presentJson(fd, nextId, second) != 0,
          "navigating onward presents the second slide");

    // Undo rolls the typing back; the slide then compiles its pre-typing
    // state (an empty slide is an interlude, which still presents).
    docs.undo();
    const QString undone = docs.currentSceneJson();
    check(!undone.isEmpty() && undone != second,
          "after undo the slide compiles its pre-typing state");
    check(presentJson(fd, nextId, undone) != 0, "the undone slide presents cleanly");

    call(fd, nextId++, "shutdown", {}, &reply);
    int status = 0;
    waitpid(worker, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the worker exited cleanly");

    close(fd);
    ::unlink(gSocket.c_str());
    std::filesystem::remove_all(scratch);

    if (gFailures > 0) {
        return 1;
    }
    std::printf("document_present_tests: newly created songs presented end to end\n");
    return 0;
}
