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
#include <fstream>
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
#include "SceneEdit.h"

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

// scene.prepare: hand the compiled JSON to the worker and return the
// revision it stores it under (0 when the prepare is refused — an
// invalid scene, say, is refused here and never reaches the store).
std::uint64_t prepareRevision(int fd, std::int64_t& nextId, const QString& sceneJson) {
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
    return numberValue(reply.at("result").as_object().at("revision"));
}

// scene.present, then wait for output.status to report the revision
// actually on air — "presented" means acknowledged, not requested.
// Returns the revision once it is on air, 0 otherwise.
std::uint64_t presentRevision(int fd, std::int64_t& nextId, std::uint64_t revision) {
    if (revision == 0) {
        return 0;
    }
    boost::json::value reply;
    if (!call(fd, nextId++, "scene.present",
              boost::json::object{{"revision", revision}}, &reply) ||
        !reply.as_object().contains("result")) {
        return 0;
    }
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

// The panel's one-call hand-off (WorkerController::presentSceneJson with
// fAutoPresentScene): prepare the JSON and present what the worker
// acknowledges. Returns the revision on air, 0 on failure.
std::uint64_t presentJson(int fd, std::int64_t& nextId, const QString& sceneJson) {
    return presentRevision(fd, nextId, prepareRevision(fd, nextId, sceneJson));
}


// A service holding a slide with THREE text boxes and one with NONE. The
// editor cannot author a second box yet (task 6.17a2 owns that), so this is
// how a stacked slide reaches the panel for real: as a file, written by a
// build that has the handles. The claim under test is the seam the editor will
// use — a stacked slide's compiled scene must clear scene.prepare's validator
// and go on air, and a backdrop slide's empty item list must still present.
// One raw literal with its own delimiter: JSON contains no ')', so nothing in
// the data can end the string early.
void writeStackedService(const std::filesystem::path& service) {
    std::filesystem::create_directories(service);
    std::ofstream out(service / "service.json");
    out << R"svc({"format":2,"nextOrdinal":90,"shows":[{"id":"show1","name":"Stacked","notes":"","defaultStyleId":"","slides":[{"id":"slide1","group":"","items":[{"id":"text1","lines":["Verse one"],"x":160,"y":100,"width":1600,"height":880},{"id":"text2","lines":["TITLE STRIP"],"x":100,"y":40,"width":800,"height":120},{"id":"text3","lines":["photo: someone"],"x":1400,"y":980,"width":460,"height":60}],"notes":"","styleId":"","backgroundAssetId":""},{"id":"slide2","group":"","items":[],"notes":"","styleId":"","backgroundAssetId":""}]}],"styles":[],"assets":[],"playlists":[]})svc";
    out.close();
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
    const std::uint64_t revFirst = presentJson(fd, nextId, first);
    check(revFirst != 0, "the newly created slide is presented and reports itself on air");

    // Navigation and the second slide: the same loop an operator runs.
    check(docs.addSlide(), "the second slide is added");
    docs.setSlideText(QStringLiteral("And that thou bidst me come to thee\nO Lamb of God, I come"));
    docs.flushText();
    const QString second = docs.currentSceneJson();
    check(!second.isEmpty() && second != first, "the second slide compiles differently");
    const std::uint64_t revSecond = presentJson(fd, nextId, second);
    check(revSecond != 0, "navigating onward presents the second slide");

    // --- task 3.4: independent clears and restore, on the live worker ---
    // A freshly typed slide is words over black — which is exactly why
    // the background clear would sit disabled. A style's colour puts a
    // background on air the way compileSlide produces one; from there
    // each clear derives a scene from the JSON on air (what
    // WorkerController's per-revision mirror holds) and presents it, so
    // the untouched layer rides through unchanged.
    auto withColor = [](const QString& sceneJson) {
        boost::json::value parsed = boost::json::parse(sceneJson.toStdString());
        parsed.as_object()["backgroundColor"] = 0x1a2b3cu;
        return QString::fromStdString(boost::json::serialize(parsed));
    };
    check(openshow::ui::sceneContent(second).words &&
              !openshow::ui::sceneContent(second).background,
          "a typed slide is words over a black background");

    const QString styled = withColor(second);
    const std::uint64_t revStyled = presentJson(fd, nextId, styled);
    check(revStyled != 0, "the styled slide (words + background) is on air");
    check(openshow::ui::sceneContent(styled).words &&
              openshow::ui::sceneContent(styled).background,
          "the styled slide's content flags name both layers");

    const QString wordsGone = openshow::ui::withWordsCleared(styled);
    check(!wordsGone.isEmpty() && presentJson(fd, nextId, wordsGone) != 0,
          "clear-slide derives a revision the worker acknowledges");
    check(openshow::ui::sceneContent(wordsGone).background &&
              !openshow::ui::sceneContent(wordsGone).words,
          "clearing the slide leaves the background — clears are independent");

    const QString backgroundGone = openshow::ui::withBackgroundCleared(styled);
    check(openshow::ui::sceneContent(backgroundGone).words &&
              !openshow::ui::sceneContent(backgroundGone).background,
          "clearing the background leaves the words");

    // Clear all, then the single-step restore: re-present the revision
    // the clear sequence cut from — the store still holds it.
    check(presentJson(fd, nextId, openshow::ui::clearedSceneJson()) != 0,
          "clear-all presents the empty scene cleanly");
    check(presentRevision(fd, nextId, revStyled) == revStyled,
          "restore re-presents the remembered revision; it reports itself on air");

    // Undo rolls the typing back; the slide then compiles its pre-typing
    // state (an empty slide is an interlude, which still presents).
    docs.undo();
    const QString undone = docs.currentSceneJson();
    check(!undone.isEmpty() && undone != second,
          "after undo the slide compiles its pre-typing state");
    check(presentJson(fd, nextId, undone) != 0, "the undone slide presents cleanly");

    // --- task 6.17a: a slide with SEVERAL text boxes, on a live worker ---
    {
        const std::filesystem::path stackedDir = scratch / "stacked";
        writeStackedService(stackedDir / "service");
        DocumentController stacked(nullptr, stackedDir);
        check(stacked.songs().size() == 1, "the stacked service loads at startup");
        const QVariantList rows = stacked.slides();
        check(rows.size() == 2, "both of its slides are listed");
        const QString stackedId = rows.at(0).toMap().value(QStringLiteral("id")).toString();
        const QString backdropId = rows.at(1).toMap().value(QStringLiteral("id")).toString();

        const QString stackedJson = stacked.sceneJsonForSlide(stackedId);
        std::size_t wireItems = 0;
        double secondBoxX = -1.0;
        if (!stackedJson.isEmpty()) {
            const boost::json::value parsed = boost::json::parse(stackedJson.toStdString());
            const boost::json::array& items = parsed.as_object().at("items").as_array();
            wireItems = items.size();
            if (items.size() > 1) {
                secondBoxX = items[1].as_object().at("x").to_number<double>();
            }
        }
        check(wireItems == 3 && secondBoxX == 100.0,
              "the stacked slide compiles to three items, each at its own box");
        const std::uint64_t revStacked = presentJson(fd, nextId, stackedJson);
        check(revStacked != 0,
              "a slide with three text boxes is prepared, presented and reports itself on air");

        // A slide drained of boxes is a backdrop, not a malformed scene: it
        // compiles with no items at all and still presents.
        const QString backdropJson = stacked.sceneJsonForSlide(backdropId);
        const bool noItems = !backdropJson.isEmpty() &&
                             boost::json::parse(backdropJson.toStdString()).as_object()
                                 .at("items").as_array().empty();
        check(noItems && presentJson(fd, nextId, backdropJson) != 0,
              "the box-less slide compiles to no items and presents cleanly");

        // The wire's own ceiling, measured at the door rather than assumed:
        // one item past the limit the MODEL refuses at (scene_tests pins the two
        // constants equal) must be refused by the worker, with the limit named.
        // The frontend cannot build this slide; a different build or a hand-
        // edited package can, and the audience must not be the one to find out.
        boost::json::value tooMany = boost::json::parse(stackedJson.toStdString());
        boost::json::array& items = tooMany.as_object()["items"].as_array();
        while (items.size() <= 8) {
            items.push_back(items[0]);
        }
        boost::json::value refusedReply;
        // An error IS an answer, so the refusal is "answered, and what came
        // back was an error naming the limit" — not "no reply", which would
        // also be true of a timeout and would pass for the wrong reason.
        const bool answered = call(fd, nextId++, "scene.prepare", tooMany.as_object(),
                                   &refusedReply);
        std::string refusalText;
        const bool refused =
            answered && refusedReply.is_object() && !refusedReply.as_object().contains("result");
        if (refused) {
            const auto error = refusedReply.as_object().find("error");
            if (error != refusedReply.as_object().end() && error->value().is_object()) {
                const auto message = error->value().as_object().find("message");
                if (message != error->value().as_object().end() && message->value().is_string()) {
                    refusalText = std::string(message->value().as_string());
                }
            }
        }
        check(refused && refusalText.find("over the limit of 8") != std::string::npos,
              "nine text items are refused at scene.prepare, naming the limit");
        check(presentJson(fd, nextId, stackedJson) != 0,
              "and the store is unharmed: the stacked slide presents again after the refusal");
    }

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
