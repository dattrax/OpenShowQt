/*
 * EW.2/EW.3: the WYSIWYG editor's geometry comes from the REAL renderer.
 *
 * These rows ask a production openshow-worker over a real socket for the
 * caret, selection and hit-test of a compiled slide item — the same
 * `SceneTextEngine::layoutInBox` result that paints the audience's pixels.
 * A stubbed caret (Qt's own font metrics, a character-count estimate, or a
 * second layout) would fail the wrap, draft, capitals and round-trip rows,
 * which is exactly the gate EW.2 set: an approximate overlaid text field
 * does not pass.
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openshow/protocol/protocol.h"
#include "openshow/scene/scene.h"

namespace {

namespace proto = openshow::protocol;

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++gFailed;
    }
    return condition;
}

std::string gWorkerBinary = "./openshow-worker";
const std::string gSocket =
    "/tmp/openshow-editor-" + std::to_string(getpid()) + ".sock";

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

bool awaitReply(int fd, std::int64_t id, boost::json::value* reply) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pending{fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready > 0) {
            char buffer[65536];
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) return false;
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

bool call(int fd, std::int64_t id, const char* method, boost::json::object params,
          boost::json::value* reply) {
    std::string wire;
    proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire);
    if (::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) < 0) return false;
    return awaitReply(fd, id, reply);
}

bool callValue(int fd, std::int64_t id, const char* method, boost::json::value params,
               boost::json::value* reply) {
    boost::json::object request;
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = std::string(method);
    request["params"] = std::move(params);
    std::string wire;
    proto::appendFrame(boost::json::value(std::move(request)), &wire);
    if (::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) < 0) return false;
    return awaitReply(fd, id, reply);
}

// One text item's scene as the Edit page compiles it: the authored box,
// centred lyric defaults, one item in draw order.
boost::json::object slideScene(const std::vector<std::string>& lines,
                               bool allCaps = false, double fontSize = 96.0,
                               double width = 1600.0) {
    openshow::scene::Scene scene;
    scene.showId = "show1";
    scene.slideId = "slide1";
    openshow::scene::SceneItem item;
    item.lines = lines;
    item.fontSize = fontSize;
    item.width = width;
    item.height = 880.0;
    item.allCaps = allCaps;
    item.align = "left";
    scene.items.push_back(std::move(item));
    return openshow::scene::toJson(scene).as_object();
}

struct Answer {
    bool ok = false;
    std::string error;
    int item = -1;
    double boxX = 0.0;
    double boxY = 0.0;
    double boxWidth = 0.0;
    double boxHeight = 0.0;
    double fontSize = 0.0;
    int lineCount = 0;
    int textLength = 0;
    bool hasCaret = false;
    double caretX = 0.0;
    double caretY = 0.0;
    double caretHeight = 0.0;
    int caretOffset = -1;
    int caretLine = -1;
    int selectionRects = 0;
    double selectionLeft = 0.0;
    double selectionRight = 0.0;
    double selectionTop = 0.0;
    double selectionBottom = 0.0;
    bool hasHit = false;
    int hitOffset = -1;
    int hitLine = -1;
};

Answer ask(int fd, std::int64_t& nextId, const boost::json::object& scene, int item,
           const boost::json::object& extra = {}) {
    boost::json::object params;
    params["request"] = nextId;
    params["scene"] = scene;
    params["item"] = item;
    for (const auto& entry : extra) params[entry.key()] = entry.value();
    boost::json::value reply;
    Answer answer;
    if (!call(fd, nextId++, proto::kMethodTextEditor, std::move(params), &reply)) {
        answer.error = "no reply";
        return answer;
    }
    if (reply.as_object().contains("error")) {
        answer.error = std::string(reply.at("error").at("message").as_string());
        return answer;
    }
    const boost::json::object& body = reply.at("result").as_object();
    answer.ok = true;
    answer.item = body.at("item").to_number<int>();
    const boost::json::object& box = body.at("box").as_object();
    answer.boxX = box.at("x").to_number<double>();
    answer.boxY = box.at("y").to_number<double>();
    answer.boxWidth = box.at("width").to_number<double>();
    answer.boxHeight = box.at("height").to_number<double>();
    answer.fontSize = body.at("fontSize").to_number<double>();
    answer.lineCount = body.at("lineCount").to_number<int>();
    answer.textLength = body.at("textLength").to_number<int>();
    const auto caret = body.find("caret");
    if (caret != body.end() && caret->value().is_object()) {
        const boost::json::object& row = caret->value().as_object();
        answer.hasCaret = true;
        answer.caretX = row.at("x").to_number<double>();
        answer.caretY = row.at("y").to_number<double>();
        answer.caretHeight = row.at("height").to_number<double>();
        answer.caretOffset = row.at("offset").to_number<int>();
        answer.caretLine = row.at("line").to_number<int>();
    }
    const auto selection = body.find("selection");
    if (selection != body.end() && selection->value().is_array()) {
        const boost::json::array& rects = selection->value().as_array();
        answer.selectionRects = static_cast<int>(rects.size());
        bool first = true;
        for (const boost::json::value& entry : rects) {
            const boost::json::object& rect = entry.as_object();
            const double x = rect.at("x").to_number<double>();
            const double y = rect.at("y").to_number<double>();
            const double w = rect.at("width").to_number<double>();
            const double h = rect.at("height").to_number<double>();
            if (first) {
                answer.selectionLeft = x;
                answer.selectionRight = x + w;
                answer.selectionTop = y;
                answer.selectionBottom = y + h;
                first = false;
            } else {
                answer.selectionLeft = std::min(answer.selectionLeft, x);
                answer.selectionRight = std::max(answer.selectionRight, x + w);
                answer.selectionTop = std::min(answer.selectionTop, y);
                answer.selectionBottom = std::max(answer.selectionBottom, y + h);
            }
        }
    }
    const auto hit = body.find("hit");
    if (hit != body.end() && hit->value().is_object()) {
        const boost::json::object& row = hit->value().as_object();
        answer.hasHit = true;
        answer.hitOffset = row.at("offset").to_number<int>();
        answer.hitLine = row.at("line").to_number<int>();
    }
    return answer;
}

bool refusedWith(int fd, std::int64_t& nextId, boost::json::value params,
                 const char* needle, const char* what) {
    boost::json::value reply;
    const bool answered = callValue(fd, nextId++, proto::kMethodTextEditor,
                                    std::move(params), &reply);
    const bool refused = answered && reply.as_object().contains("error");
    const std::string message = refused
        ? std::string(reply.at("error").at("message").as_string()) : std::string();
    const bool good = refused && message.find(needle) != std::string::npos;
    return check(good, what);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) gWorkerBinary = argv[1];
    const pid_t worker = spawnWorker();
    const int fd = connectRetry();
    if (fd < 0) {
        std::fprintf(stderr, "could not reach the worker\n");
        kill(worker, SIGTERM);
        waitpid(worker, nullptr, 0);
        return 1;
    }
    std::int64_t nextId = 1;

    // The hello is the contract: the capability and a minor that carries it.
    {
        boost::json::value reply;
        if (call(fd, nextId++, "hello",
                 proto::helloParams("editor-layout-tests", proto::kProtocolMajor,
                                    proto::kProtocolMinor),
                 &reply)) {
            check(reply.at("result").at("protocol").at("minor").to_number<int>() >= 21,
                  "the worker serves protocol minor 21");
            bool capable = false;
            for (const boost::json::value& capability :
                 reply.at("result").at("capabilities").as_array()) {
                capable = capable || capability.as_string() ==
                                         proto::kCapabilityTextEditorV1;
            }
            check(capable, "the hello names text.editor.v1");
        } else {
            check(false, "the hello answers");
        }
    }

    const auto scene = slideScene({"People come together",
                                   "Strange as neighbours, our blood is one"});
    // Caret geometry follows the shaped paragraph: offsets along one line
    // move x, the y is shared, and a line break starts a second line.
    const int firstLineEnd = 20;  // "People come together" in UTF-16 units
    const int sourceEnd = 60;
    Answer start = ask(fd, nextId, scene, 0, {{"caret", 0}});
    check(start.ok && start.hasCaret && start.caretOffset == 0 && start.caretLine == 0,
          "caret 0 answers on the first line");
    check(start.ok && start.boxX == 160.0 && start.boxY == 100.0 &&
              start.boxWidth == 1600.0 && start.boxHeight == 880.0,
          "the reply names the authored box");
    check(start.ok && start.fontSize > 90.0 && start.lineCount == 3 &&
              start.textLength == sourceEnd,
          "the reply names the fitted size, wrapped line count and source length in UTF-16");
    Answer mid = ask(fd, nextId, scene, 0, {{"caret", 6}});
    Answer lineEnd = ask(fd, nextId, scene, 0, {{"caret", firstLineEnd}});
    Answer secondLine = ask(fd, nextId, scene, 0, {{"caret", firstLineEnd + 1}});
    Answer end = ask(fd, nextId, scene, 0, {{"caret", sourceEnd}});
    check(mid.ok && mid.hasCaret && mid.caretX > start.caretX,
          "a caret further along a line moves right");
    check(mid.ok && mid.caretLine == 0 && std::abs(mid.caretY - start.caretY) < 0.5,
          "the same line shares one caret y");
    check(lineEnd.ok && lineEnd.hasCaret &&
              lineEnd.caretX >= mid.caretX,
          "the end of a line is to the right of its middle");
    check(secondLine.ok && secondLine.hasCaret && secondLine.caretLine == 1 &&
              secondLine.caretY > start.caretY + 1.0,
          "the first character of the second line sits on a lower caret");
    check(end.ok && end.hasCaret && end.caretOffset == sourceEnd && end.caretLine == 2,
          "the end caret names the last source offset on the last wrapped line");

    // Round trip: a hit test at the caret's own point answers that offset.
    {
        Answer hit = ask(fd, nextId, scene, 0,
                         {{"point", boost::json::object{{"x", mid.caretX},
                                                        {"y", mid.caretY
                                                             + mid.caretHeight / 2.0}}}});
        check(hit.ok && hit.hasHit && hit.hitOffset == 6,
              "a hit test at the caret's own point names the caret's offset");
        Answer above = ask(fd, nextId, scene, 0,
                           {{"point", boost::json::object{{"x", mid.caretX},
                                                          {"y", -500.0}}}});
        check(above.ok && above.hasHit && above.hitLine == 0,
              "a point above the box clamps to the first line");
        Answer below = ask(fd, nextId, scene, 0,
                           {{"point", boost::json::object{{"x", 1900.0},
                                                          {"y", 2000.0}}}});
        check(below.ok && below.hasHit && below.hitOffset == sourceEnd,
              "a point below and right clamps to the end of the text");
    }

    // Selection: the paragraph's own range rects, one or more per line, all
    // inside the authored box and covering the span's drawn width.
    {
        Answer selected = ask(fd, nextId, scene, 0,
                              {{"selection", boost::json::array{0, firstLineEnd}}});
        check(selected.ok && selected.selectionRects >= 1,
              "a within-line selection answers at least one rect");
        check(selected.selectionLeft >= start.boxX - 1.0 &&
                  selected.selectionRight <= start.boxX + start.boxWidth + 1.0,
              "selection rects stay inside the authored box");
        check(selected.selectionRight - selected.selectionLeft > 10.0,
              "the selection covers drawn width, not a guessed zero");
        Answer across = ask(fd, nextId, scene, 0,
                            {{"selection", boost::json::array{0, sourceEnd}}});
        check(across.ok && across.selectionRects >= 2,
              "a selection across a wrap answers one rect per line");
        check(across.selectionTop < start.caretY + 1.0 &&
                  across.selectionBottom > secondLine.caretY,
              "the wrapped selection spans both lines vertically");
        Answer reversed = ask(fd, nextId, scene, 0,
                              {{"selection", boost::json::array{sourceEnd, 0}}});
        check(reversed.ok && reversed.selectionRects == across.selectionRects,
              "an anchor/focus pair is answered in order");
    }

    // Draft override: the ask measures and places the WORDS IT WAS GIVEN,
    // so the caret follows uncommitted typing without waiting for a
    // document edit. The scene itself is untouched.
    {
        Answer draft = ask(fd, nextId, scene, 0,
                           {{"lines", boost::json::array{"Draft only"}},
                            {"caret", 10}});
        check(draft.ok && draft.textLength == 10 && draft.lineCount == 1 &&
                  draft.hasCaret && draft.caretOffset == 10,
              "a draft override answers the draft's own length and caret");
        Answer again = ask(fd, nextId, scene, 0, {{"caret", sourceEnd}});
        check(again.ok && again.textLength == sourceEnd,
              "the draft changes nothing in the scene");
    }

    // All caps: the display transform expands ß → SS, and the editor still
    // speaks AUTHORED offsets. The caret for source offset 1 (after 's')
    // sits inside the expansion and must not run past the source end, and
    // every source offset maps to a monotonic x.
    {
        const auto capsScene = slideScene({"straße"}, true);
        Answer capsStart = ask(fd, nextId, capsScene, 0, {{"caret", 0}});
        Answer capsAfterS = ask(fd, nextId, capsScene, 0, {{"caret", 1}});
        Answer capsAfterSs = ask(fd, nextId, capsScene, 0, {{"caret", 2}});
        Answer capsEnd = ask(fd, nextId, capsScene, 0, {{"caret", 6}});
        check(capsStart.ok && capsEnd.ok && capsEnd.textLength == 6,
              "the capitals scene still reports authored source length");
        check(capsAfterS.hasCaret && capsAfterSs.hasCaret && capsEnd.hasCaret &&
                  capsStart.caretX < capsAfterS.caretX &&
                  capsAfterS.caretX < capsAfterSs.caretX &&
                  capsAfterSs.caretX < capsEnd.caretX,
              "authored offsets inside an expansion map to increasing display x");
        Answer hit = ask(fd, nextId, capsScene, 0,
                         {{"point", boost::json::object{{"x", capsAfterSs.caretX},
                                                        {"y", capsAfterSs.caretY
                                                             + capsAfterSs.caretHeight / 2.0}}}});
        check(hit.ok && hit.hasHit && hit.hitOffset >= 0 && hit.hitOffset <= 6,
              "a hit test in a capitals line answers an authored offset");
        // The literal-capitals scene and the transformed one must place the
        // same authored caret at the same x: the expansion is the renderer's,
        // and the editor follows its glyphs.
        const auto literalScene = slideScene({"STRASSE"}, false);
        Answer literal = ask(fd, nextId, literalScene, 0, {{"caret", 7}});
        check(literal.ok && literal.hasCaret &&
                  std::abs(literal.caretX - capsEnd.caretX) < 2.0,
              "the capitals caret lands where literal capitals draw");
    }

    // Fractional/odd scales do not exist on this side of the wire: the
    // answer is authored units, and the frontend's zoom multiplies once.
    // That keeps the caret correct at every DPI instead of baking one in.

    // Refusals: every one is a named sentence and the session lives on.
    {
        check(refusedWith(fd, nextId,
                          boost::json::object{{"scene", scene}, {"item", 3},
                                              {"caret", 0}},
                          "outside the scene",
                          "an item index past the scene is refused by name"),
              "an out-of-scene item is refused");
        boost::json::object imageScene;
        {
            openshow::scene::Scene assembled;
            assembled.showId = "show1";
            assembled.slideId = "slide1";
            openshow::scene::SceneItem picture;
            picture.isImage = true;
            picture.assetId = "asset1";
            picture.assetPath = "/tmp/picture.png";
            assembled.items.push_back(std::move(picture));
            imageScene = openshow::scene::toJson(assembled).as_object();
        }
        check(refusedWith(fd, nextId,
                          boost::json::object{{"scene", imageScene}, {"item", 0},
                                              {"caret", 0}},
                          "picture",
                          "an image item is refused as not text"),
              "an image item is refused");
        check(refusedWith(fd, nextId,
                          boost::json::object{{"scene", scene}, {"item", 0}},
                          "caret, a selection or a point",
                          "an ask with no question is refused"),
              "an ask with no caret/selection/point is refused");
        Answer tolerated = ask(fd, nextId, scene, 0,
                               {{"lines", boost::json::array{"x"}}, {"caret", 0},
                                {"extra", 1}});
        check(tolerated.ok && tolerated.textLength == 1,
              "unknown parameters are ignored rather than refused");
        boost::json::value malformed = boost::json::object{{"scene", "not a scene"},
                                                           {"item", 0},
                                                           {"caret", 0}};
        check(refusedWith(fd, nextId, std::move(malformed), "compiled slide",
                          "a scene that is not the codec's object is refused"),
              "a malformed scene is refused");
        std::string longLine(openshow::scene::kMaxLineChars + 1, 'a');
        check(refusedWith(fd, nextId,
                          boost::json::object{{"scene", scene}, {"item", 0},
                                              {"lines", boost::json::array{longLine}},
                                              {"caret", 0}},
                          "draft lines",
                          "an oversized draft line is refused by name"),
              "an oversized draft line is refused");
        Answer alive = ask(fd, nextId, scene, 0, {{"caret", 3}});
        check(alive.ok && alive.hasCaret && alive.caretOffset == 3,
              "refusals leave the session answering");
    }

    boost::json::value shutdownReply;
    call(fd, nextId++, "shutdown", {}, &shutdownReply);
    close(fd);
    int status = 0;
    waitpid(worker, &status, 0);
    unlink(gSocket.c_str());

    if (gFailed == 0) {
        std::printf("editor_layout_tests: all fixtures passed\n");
        return 0;
    }
    std::printf("editor_layout_tests: %d FAILURES\n", gFailed);
    return 1;
}
