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
#include <QElapsedTimer>
#include <QEventLoop>

#include "openshow/protocol/protocol.h"
#include "openshow/domain/documents.h"
#include "openshow/scene/scene.h"

#include "DocumentController.h"
#include "SceneEdit.h"

namespace {

namespace proto = openshow::protocol;
using openshow::ui::DocumentController;

bool waitForDeckImport(DocumentController& docs, int timeoutMs = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (docs.powerPointImportBusy() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !docs.powerPointImportBusy();
}

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

// output.status reporting this revision as the one on air — "presented" means
// acknowledged, not requested. Kept apart from the present that needs it
// because task TR.3 presents with a transition record of its own and still
// owes the same proof: the revision the record rode on is the landed one.
bool revisionOnAir(int fd, std::int64_t& nextId, std::uint64_t revision, int budgetMs = 6000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::json::value statusReply;
        if (call(fd, nextId++, "output.status", {}, &statusReply)) {
            const boost::json::value& status = statusReply.at("result");
            if (status.as_object().contains("scene") &&
                status.at("scene").as_object().contains("active") &&
                numberValue(status.at("scene").as_object().at("active")) == revision) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
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
    return revisionOnAir(fd, nextId, revision) ? revision : 0;
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
    // the background clear would sit disabled. A theme's colour puts a
    // background on air the way compileSlide produces one; from there
    // each clear derives a scene from the JSON on air (what
    // WorkerController's per-revision mirror holds) and presents it, so
    // the untouched layer rides through unchanged.
    auto withColor = [](const QString& sceneJson) {
        boost::json::value parsed = boost::json::parse(sceneJson.toStdString());
        parsed.as_object()["background"].as_object()["transparent"] = false;
        parsed.as_object()["backgroundColor"] = 0x1a2b3cu;
        return QString::fromStdString(boost::json::serialize(parsed));
    };
    check(openshow::ui::sceneContent(second).words &&
              !openshow::ui::sceneContent(second).background,
          "a typed slide is words over a black background");

    const QString styled = withColor(second);
    const std::uint64_t revThemed = presentJson(fd, nextId, styled);
    check(revThemed != 0, "the styled slide (words + background) is on air");
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
    check(presentRevision(fd, nextId, revThemed) == revThemed,
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

    // --- UI.2: browsing and preparing never go live -----------------------
    // The whole sidebar dance — new presentations, a new playlist, duplicate
    // entries, inserts, moves and selections — runs through the very
    // DocumentController the outline clicks call, while a scene stands on
    // air. The audience's picture must not move: the applied revision stays
    // put across a window of status polls, even though the edited document
    // has visibly moved on. (Task UI.2's "selecting or moving a presentation
    // never goes live", proven on the wire rather than asserted.)
    {
        boost::json::value statusReply;
        std::uint64_t liveRev = 0;
        if (call(fd, nextId++, "output.status", {}, &statusReply) &&
            statusReply.as_object().contains("result")) {
            const boost::json::value& live = statusReply.at("result");
            if (live.as_object().contains("scene") &&
                live.at("scene").as_object().contains("active")) {
                liveRev = numberValue(live.at("scene").as_object().at("active"));
            }
        }
        if (check(liveRev != 0, "a scene is on air before the browsing starts")) {
            check(docs.createSong(QStringLiteral("How Deep")),
                  "the outline creates a presentation");
            const QString browseId = docs.currentSongId();
            docs.addSlide();
            docs.setSlideText(QStringLiteral("How deep the Father's love"));
            docs.flushText();
            check(docs.createPlaylist(QStringLiteral("Sunday")), "the outline creates a playlist");
            check(docs.addShowToPlaylist(browseId) && docs.addShowToPlaylist(browseId),
                  "the running order takes the show twice");
            check(docs.insertShowInPlaylist(browseId, 1), "and one more between the copies");
            const auto entries = docs.playlistEntries();
            check(entries.size() == 3 &&
                      docs.movePlaylistEntry(entries[2].toMap().value("entryId").toString(), 0),
                  "rows move inside the running order");
            docs.selectSlide(0);
            docs.selectSong(browseId);
            docs.selectPlaylist(docs.currentPlaylistId());  // clicking the viewed row again
            bool unchanged = true;
            for (int poll = 0; poll < 8 && unchanged; ++poll) {
                unchanged = call(fd, nextId++, "output.status", {}, &statusReply) &&
                            statusReply.at("result").at("scene").as_object().contains("active") &&
                            numberValue(statusReply.at("result")
                                            .at("scene").as_object().at("active")) == liveRev;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            check(unchanged,
                  "creating, selecting and moving never present, prepare or clear on their own");
            check(docs.currentSongId() == browseId && docs.currentSceneJson() != first,
                  "the edited document has visibly moved on while the audience stayed");
        }
    }

    // --- UI.4a: importing media never goes live ---------------------------
    // The other half of the window, same rule. The media bin's import and
    // remove are document edits (they cost an undo step each) and its row
    // selections are browse state; with a scene on air, none of it may move
    // the picture the audience is looking at.
    {
        boost::json::value statusReply;
        std::uint64_t liveRev = 0;
        if (call(fd, nextId++, "output.status", {}, &statusReply) &&
            statusReply.as_object().contains("result")) {
            liveRev = numberValue(statusReply.at("result").at("scene").as_object().at("active"));
        }
        if (check(liveRev != 0, "a scene is on air before the bin is opened")) {
            const bool imported =
                docs.importMediaFile(QStringLiteral("/bin/loop.mp4")) &&
                docs.importMediaFile(QStringLiteral("/bin/hymn.mp3")) &&
                docs.importMediaFile(QStringLiteral("/bin/notes.txt"));
            check(imported && docs.assets().size() == 3,
                  "the bin's import path registers a video, an audio file and a plain name");
            check(docs.removeAsset(docs.assets().last().toMap().value("id").toString())
                      && docs.assets().size() == 2,
                  "and Remove takes one of them out again");
            bool unchanged = true;
            for (int poll = 0; poll < 8 && unchanged; ++poll) {
                unchanged = call(fd, nextId++, "output.status", {}, &statusReply) &&
                            statusReply.at("result").at("scene").as_object().contains("active") &&
                            numberValue(statusReply.at("result")
                                            .at("scene").as_object().at("active")) == liveRev;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            check(unchanged, "importing and removing media never present, prepare or clear");
        }
    }

    // --- task TH.3: the resolved chain reaches the real worker -------------
    // The full chain — the service's current theme worn over the slide's
    // own, the slide's customization over both — exists only inside the
    // scene compile, and it must arrive over the real socket as ordinary
    // edits: every appearance change is a NEW acknowledged revision, and
    // every answer of the chain (the worn colour, the explicit flat over a
    // graded theme) is visible in the payload the worker accepted.
    {
        openshow::domain::Documents chain;
        const openshow::domain::Id worn = chain.createShow("Worn");
        const openshow::domain::Id lyric = chain.appendSlide(worn);
        chain.setSlideLines(worn, lyric, {"Chain on air"});
        const openshow::domain::Id loud = chain.createTheme(
            "Loud", "#ff0000", "#00ff00", "", "Impact", 96.0, "right", 1.5);
        const openshow::domain::Id bright = chain.createTheme(
            "Bright", "#ffe000", "#200000", "#602020", "DejaVu Serif", 72.0, "center", 1.25);
        chain.setSlideTheme(worn, lyric, loud);
        openshow::scene::Scene compiled;
        const auto compileChain = [&chain, &worn, &lyric, &compiled]() {
            return openshow::scene::compileSlide(chain, worn, lyric, &compiled)
                       ? QString::fromStdString(
                             boost::json::serialize(openshow::scene::toJson(compiled)))
                       : QString();
        };
        const auto itemColor = [](const QString& json) {
            return numberValue(boost::json::parse(json.toStdString())
                                   .at("items").as_array()[0].as_object().at("color"));
        };

        const QString plainJson = compileChain();
        check(itemColor(plainJson) == 0xFF0000u, "before anything is worn the slide draws its own theme");
        const std::uint64_t revLoud = presentJson(fd, nextId, plainJson);
        check(revLoud != 0, "the chain-resolved slide is presented on the real worker");

        check(chain.setCurrentTheme(bright), "the service puts on the current theme");
        const QString wornJson = compileChain();
        const boost::json::value wornPayload = boost::json::parse(wornJson.toStdString());
        check(numberValue(wornPayload.at("items").as_array()[0].as_object().at("color")) ==
                      0xFFE000u &&
                  numberValue(wornPayload.at("backgroundColor")) == 0u &&
                  wornPayload.at("background").as_object().at("transparent").as_bool(),
              "the compiled payload wears the current theme: colour, background, gradient");
        const std::uint64_t revBright = presentJson(fd, nextId, wornJson);
        check(revBright != 0 && revBright != revLoud,
              "the re-skin is a new revision the worker acknowledges");

        openshow::domain::SlideCustomization flat;
        flat.backgroundGradient = "";  // explicitly flat over the graded current theme
        check(chain.setSlideCustomizations(worn, lyric, flat),
              "the slide customizes itself flat");
        const QString flatJson = compileChain();
        check(!boost::json::parse(flatJson.toStdString()).at("background").as_object()
                   .contains("gradient") &&
                  itemColor(flatJson) == 0xFFE000u,
              "the flat answer drops the gradient key and keeps the worn text colour");
        check(presentJson(fd, nextId, flatJson) != 0,
              "and the customized revision goes on air acknowledged");
    }

    // --- task TR.3: the record the controls write is the record this worker
    // takes its lengths from ------------------------------------------------
    // The two ends of the transition contract are tested where they live:
    // workspace_ui_tests proves a gesture lands on a Theme's record or its
    // context's, and scene_frame_tests proves the two numbers drive the blend.
    // What sits between them is the promise that no door re-spells the record,
    // and that is only testable over a real socket with a real worker: the
    // answer DocumentController's resolver gives for a slide a UI-written theme
    // wears goes on scene.present on the keys the cue lane uses and LANDS; a
    // duration outside the band earns the domain's ONE sentence here, naming
    // the key and the range the settings page named; a Cut still carries its
    // durations whole; and a present that says nothing about movement is still
    // the 500/800 this worker has always performed.
    {
        check(docs.createSong(QStringLiteral("Timed")) && docs.addSlide(),
              "the transition leg authors a slide of its own");
        docs.setSlideText(QStringLiteral("Move gently, audience"));
        docs.flushText();
        QString briskId;
        check(docs.createBlankTheme(QStringLiteral("Brisk")),
              "a theme exists to be given a movement");
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("name")).toString()
                    == QStringLiteral("Brisk"))
                briskId = theme.toMap().value(QStringLiteral("id")).toString();
        }
        check(!briskId.isEmpty() && docs.applyThemeToCurrentSlide(briskId),
              "the slide wears it");

        // The payload the panel would send, and the answer main.cpp's callback
        // gives for it — the same resolver, the same documents, no second walk
        // invented for the test.
        const auto resolvedForScene = [&docs]() {
            const boost::json::value parsed =
                boost::json::parse(docs.currentSceneJson().toStdString());
            const openshow::domain::TransitionOptions options =
                docs.transitionForSceneJson(parsed.as_object());
            return boost::json::object{{"transition",
                                        openshow::domain::transitionKindName(options.kind)},
                                       {"textDurationMs", options.textDurationMs},
                                       {"mediaDurationMs", options.mediaDurationMs}};
        };
        const auto messageOf = [](const boost::json::value& reply) {
            if (!reply.as_object().contains("error")) {
                return std::string();
            }
            return std::string(reply.at("error").as_object().at("message").as_string());
        };

        const boost::json::object inherited = resolvedForScene();
        check(inherited.at("transition").as_string() == "fade"
                  && inherited.at("textDurationMs").as_int64() == 500
                  && inherited.at("mediaDurationMs").as_int64() == 800,
              "a theme that says nothing resolves to the service's standing 500 / 800 — the "
              "inheritance the inspector shows is the inheritance the wire carries");

        QVariantMap brisk;
        brisk.insert(QStringLiteral("kind"), QStringLiteral("fade"));
        brisk.insert(QStringLiteral("textDurationMs"), 1800);
        brisk.insert(QStringLiteral("mediaDurationMs"), 2400);
        check(docs.setThemeTransition(briskId, brisk),
              "the Theme Editor gives the view a record of its own");
        const boost::json::object answer = resolvedForScene();
        check(answer.at("textDurationMs").as_int64() == 1800
                  && answer.at("mediaDurationMs").as_int64() == 2400,
              "and the walk reaches it: the theme's milliseconds, over the service's");

        // Each landing below needs its OWN revision — a repeat present of what
        // is already on air is a no-op by design, and a no-op that reports the
        // revision it was already showing is not evidence of anything.
        const std::uint64_t timed = prepareRevision(fd, nextId, docs.currentSceneJson());
        boost::json::object params = answer;
        params["revision"] = timed;
        boost::json::value presentReply;
        check(timed != 0 && call(fd, nextId++, "scene.present", params, &presentReply)
                  && presentReply.as_object().contains("result")
                  && revisionOnAir(fd, nextId, timed),
              "the real worker accepts the record the operator's controls wrote, and the "
              "acknowledged revision is the one that record rode on");

        // The refusal half of the one sentence: the settings page refused 10001
        // naming the key and the band; this worker refuses the same number with
        // the same words, because both doors quote Documents rather than each
        // keeping a ceiling of its own. A record that got through would move
        // the audience by a duration nothing in this build could have stored.
        boost::json::object over = answer;
        over["revision"] = timed;
        over["textDurationMs"] = 10001;
        boost::json::value overReply;
        const std::string overWords = call(fd, nextId++, "scene.present", over, &overReply)
                                          ? messageOf(overReply)
                                          : std::string();
        check(overWords.find("textDurationMs") != std::string::npos
                  && overWords.find("0..10000") != std::string::npos,
              "an over-band words duration is refused on the wire quoting the key and the range "
                  "the settings page quoted");
        check(revisionOnAir(fd, nextId, timed),
              "and the refused cue left the audience where it was");

        // A Cut is a decision about the KIND, and the record is atomic on the
        // wire exactly as it is in the file: the durations ride along, vetted,
        // rather than being dropped because they are ignored.
        docs.addSlide();
        docs.applyThemeToCurrentSlide(briskId);
        docs.setSlideText(QStringLiteral("Or simply be"));
        docs.flushText();
        const std::uint64_t cutRev = prepareRevision(fd, nextId, docs.currentSceneJson());
        boost::json::object cut = answer;
        cut["revision"] = cutRev;
        cut["transition"] = "cut";
        boost::json::value cutReply;
        check(cutRev != 0 && cutRev != timed && call(fd, nextId++, "scene.present", cut, &cutReply)
                  && cutReply.as_object().contains("result") && revisionOnAir(fd, nextId, cutRev),
              "a Cut carries its durations and lands a fresh revision — the record is whole on "
                  "the wire as it is in the document");

        // Silence on the wire means what it has always meant: a peer that says
        // nothing about movement gets the contract's 500 / 800, so a frontend
        // from before TR.1 still cues this worker, and this build's own
        // inherit-and-not-yet-written slide behaves the same way.
        check(presentRevision(fd, nextId, timed) == timed,
              "and a present with no duration keys still lands — the 500 / 800 this worker has "
                  "always performed");

        // Inherit, seen from the wire: taking the view's record off moves the
        // resolver's answer back to the service's, and the record the cue lane
        // would then send is one this worker accepts unchanged.
        check(docs.setThemeTransition(briskId, QVariantMap{}),
              "the view hands its movement back to the chain");
        const boost::json::object back = resolvedForScene();
        check(back.at("textDurationMs").as_int64() == 500
                  && back.at("mediaDurationMs").as_int64() == 800,
              "and the answer is the service's again, not a stuck copy of the theme's");
        boost::json::object backParams = back;
        backParams["revision"] = cutRev;
        boost::json::value backReply;
        check(call(fd, nextId++, "scene.present", backParams, &backReply)
                  && backReply.as_object().contains("result") && revisionOnAir(fd, nextId, cutRev),
              "which the worker accepts on the same keys, so the door the UI opens and the door "
                  "the wire has are one door");
    }

#ifdef OPENSHOW_PPTX_RENDER_BIN
    // --- PX.6: the operator deck's renders actually present ---
    {
        qputenv("OPENSHOW_PPTX_RENDER", QByteArray(OPENSHOW_PPTX_RENDER_BIN));
        const QString deck = QStringLiteral(
            OPENSHOW_SOURCE_DIR "/tools/pptx-render/tests/fixtures/aug-8.pptx");
        DocumentController decks(nullptr, scratch / "decks");
        check(decks.importPowerPointToLibrary(deck),
              "the operator deck starts importing for presentation");
        check(waitForDeckImport(decks), "the operator deck finishes rendering");
        const QString showId = decks.currentSongId();
        const QVariantList pages = decks.slidesOf(showId);
        check(pages.size() == 6, "August's deck has six visible slides to present");
        for (const QVariant& page : pages) {
            const QString slideId =
                page.toMap().value(QStringLiteral("id")).toString();
            const QString json = decks.sceneJsonForSlide(slideId);
            check(!json.isEmpty(), "an imported slide compiles to a scene");
            check(presentJson(fd, nextId, json) != 0,
                  "every imported slide presents on the live worker");
        }
        // The saved package reopens with no renderer, and still presents.
        check(decks.save(), "the imported deck saves with its renders");
        qputenv("OPENSHOW_PPTX_RENDER", "/no/such/pptx-render");
        DocumentController reopened(nullptr, scratch / "decks");
        check(reopened.load(), "the imported deck reopens offline");
        check(reopened.slidesOf(showId).size() == 6,
              "the reopened deck keeps all six slides");
        const QString reopenedJson = reopened.sceneJsonForSlide(
            reopened.slidesOf(showId).at(0).toMap()
                .value(QStringLiteral("id")).toString());
        check(!reopenedJson.isEmpty() && presentJson(fd, nextId, reopenedJson) != 0,
              "the reopened deck presents without the renderer");
    }
#endif

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
