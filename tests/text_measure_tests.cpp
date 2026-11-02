/*
 * Task BI.4's other half: the numbers come from the WORKER'S OWN layout.
 *
 * The scripted-engine tests (scripture_pagination_tests) prove the
 * pagination's properties; this file proves the engine behind the port is
 * the SkParagraph/ICU pass that draws the audience's slides — by asking a
 * real openshow-worker over a real socket: the same text at twice the size
 * measures taller, the same text in a wider box wraps to fewer lines, a
 * Unicode run measures like text, and an empty block measures like nothing.
 * A stub answering from character counts would fail the width and size
 * rows; a second renderer answering here would be the very thing BI.4
 * forbids, so these rows run against the production binary.
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
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openshow/protocol/protocol.h"

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
    "/tmp/openshow-measure-" + std::to_string(getpid()) + ".sock";

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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pending{fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 100);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready > 0) {
            char buffer[65536];
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

boost::json::object block(const std::string& key, std::vector<std::string> lines,
                          double fontSize, double width, double lineSpacing = 1.25) {
    boost::json::object out;
    boost::json::array jsonLines;
    for (const std::string& line : lines) {
        jsonLines.push_back(boost::json::value(line));
    }
    out["key"] = key;
    out["lines"] = std::move(jsonLines);
    out["fontSize"] = fontSize;
    out["width"] = width;
    out["lineSpacing"] = lineSpacing;
    return out;
}

struct Measured {
    bool ok = false;
    std::string key;
    double height = 0.0;
    int lineCount = 0;
};

// One block, one answer — the failure sentences come back through here too
// as {ok:false, error:...} so a refusal is a value the rows can name.
Measured measureOne(int fd, std::int64_t& nextId, const boost::json::object& oneBlock,
                    std::string* error) {
    boost::json::value reply;
    Measured measured;
    if (!call(fd, nextId++, proto::kMethodTextMeasure,
              boost::json::object{{"blocks", boost::json::array{oneBlock}}}, &reply)) {
        if (error) {
            *error = "no reply";
        }
        return measured;
    }
    if (reply.as_object().contains("error")) {
        if (error) {
            *error = std::string(reply.at("error").at("message").as_string());
        }
        return measured;
    }
    const boost::json::array& rows = reply.at("result").at("measured").as_array();
    if (rows.size() != 1) {
        if (error) {
            *error = "not exactly one answer";
        }
        return measured;
    }
    measured.ok = true;
    measured.key = std::string(rows[0].as_object().at("key").as_string());
    measured.height = rows[0].as_object().at("height").to_number<double>();
    measured.lineCount = static_cast<int>(rows[0].as_object().at("lineCount").to_number<int>());
    return measured;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }
    const pid_t worker = spawnWorker();
    const int fd = connectRetry();
    if (fd < 0) {
        std::fprintf(stderr, "could not reach the worker\n");
        kill(worker, SIGTERM);
        waitpid(worker, nullptr, 0);
        return 1;
    }
    std::int64_t nextId = 1;

    // The hello is the contract: minor 14, and text.measure.v1 named.
    {
        boost::json::value reply;
        if (!call(fd, nextId++, "hello",
                  proto::helloParams("text-measure-tests", proto::kProtocolMajor,
                                     proto::kProtocolMinor),
                  &reply)) {
            std::fprintf(stderr, "hello failed\n");
            close(fd);
            kill(worker, SIGTERM);
            waitpid(worker, nullptr, 0);
            return 1;
        }
        const boost::json::value& negotiated = reply.at("result").at("protocol");
        check(negotiated.at("minor").to_number<std::uint64_t>() >= 14,
              "the worker serves protocol minor 14");
        bool hasMeasure = false;
        for (const boost::json::value& capability :
             reply.at("result").at("capabilities").as_array()) {
            hasMeasure = hasMeasure || capability.as_string() == "text.measure.v1";
        }
        check(hasMeasure, "the hello names text.measure.v1");
    }

    // Same words, twice the size: taller, still one line. SkParagraph
    // honours the explicit line box, so the growth is the size's own.
    double height40 = 0.0;
    {
        std::string error;
        const Measured small =
            measureOne(fd, nextId, block("a", {"Hello world"}, 40.0, 800.0), &error);
        check(small.ok && small.height > 0.0 && small.lineCount == 1,
              "one line at size 40 measures as one real line");
        height40 = small.height;
        const Measured large =
            measureOne(fd, nextId, block("b", {"Hello world"}, 80.0, 800.0), &error);
        check(large.ok && large.height > height40 * 1.5 && large.lineCount == 1,
              "the same line at size 80 measures taller at the same line count — the engine "
              "is honouring the chosen size, not an estimate");
    }

    // Same words, same size, different widths: the narrow box WRAPS to more
    // lines than the wide one. Character counts alone could not know this.
    {
        std::string text;
        for (int i = 0; i < 12; ++i) {
            text += "the quick brown fox jumps over the lazy dog ";
        }
        std::string error;
        const Measured narrow = measureOne(fd, nextId, block("n", {text}, 40.0, 400.0), &error);
        const Measured wide = measureOne(fd, nextId, block("w", {text}, 40.0, 1600.0), &error);
        check(narrow.ok && wide.ok && narrow.lineCount > wide.lineCount &&
                  narrow.height > wide.height,
              "the same text at the same size wraps to more lines in a narrower box — "
              "real layout answered");
    }

    // Unicode measures like text: Greek, CJK and an em dash give a real
    // height, and the CJK run wraps in a narrow box like any other.
    {
        std::string error;
        const Measured uni = measureOne(
            fd, nextId,
            block("u", {"中文文本，测试经文的换行。Ωμέγα — ἰδοὺ ἡ δόξα"}, 40.0, 300.0), &error);
        check(uni.ok && uni.height > 0.0 && uni.lineCount >= 2,
              "Unicode words measure and wrap in the worker's own engine");
        const Measured doubled = measureOne(
            fd, nextId, block("u2", {"中文文本，测试经文的换行。Ωμέγα — ἰδοὺ ἡ δόξα"}, 80.0, 300.0),
            &error);
        check(doubled.ok && doubled.height > uni.height,
              "the Unicode run grows with the chosen size too");
    }

    // An empty block measures as nothing — no phantom row.
    {
        std::string error;
        const Measured empty = measureOne(fd, nextId, block("e", {}, 40.0, 800.0), &error);
        check(empty.ok && empty.height == 0.0 && empty.lineCount == 0,
              "an empty block measures zero and lines zero");
    }

    // A named family measures; an unknown one falls back rather than
    // failing (per-run fallback is the engine's own rule).
    {
        std::string error;
        const Measured named =
            measureOne(fd, nextId,
                       [&] {
                           boost::json::object b = block("f", {"Styled words"}, 40.0, 800.0);
                           b["fontFamily"] = "DejaVu Sans";
                           return b;
                       }(),
                       &error);
        check(named.ok && named.height > 0.0, "a named theme family measures");
        const Measured unknown =
            measureOne(fd, nextId,
                       [&] {
                           boost::json::object b = block("f2", {"Fallback words"}, 40.0, 800.0);
                           b["fontFamily"] = "NoSuchFont Whatever";
                           return b;
                       }(),
                       &error);
        check(unknown.ok && unknown.height > 0.0,
              "an unknown family falls back to a measurement, not a refusal");
    }

    // A batch answers whole and in order, keys echoed exactly.
    {
        std::string error;
        boost::json::value reply;
        const bool sent = call(
            fd, nextId++, proto::kMethodTextMeasure,
            boost::json::object{{"blocks",
                                 boost::json::array{block("k1", {"first"}, 40.0, 800.0),
                                                    block("k2", {"second", "lines"}, 40.0, 800.0),
                                                    block("k3", {"third"}, 60.0, 200.0)}}},
            &reply);
        check(sent && reply.as_object().contains("result"), "the three-block batch answered");
        if (sent && reply.as_object().contains("result")) {
            const boost::json::array& rows = reply.at("result").at("measured").as_array();
            check(rows.size() == 3 && rows[0].as_object().at("key").as_string() == "k1" &&
                      rows[1].as_object().at("key").as_string() == "k2" &&
                      rows[2].as_object().at("key").as_string() == "k3",
                  "the batch answers whole, in order, under the keys it was sent");
            check(rows[1].as_object().at("height").to_number<double>() >
                      rows[0].as_object().at("height").to_number<double>(),
                  "two lines measure taller than one at the same size");
        }
    }

    // Refusals, each naming its own limit, each leaving the session usable.
    {
        boost::json::array tooMany;
        for (int i = 0; i <= static_cast<int>(proto::kMaxMeasureBatch); ++i) {
            tooMany.push_back(block("x" + std::to_string(i), {"one line"}, 40.0, 800.0));
        }
        boost::json::value reply;
        std::string error;
        if (call(fd, nextId++, proto::kMethodTextMeasure,
                 boost::json::object{{"blocks", std::move(tooMany)}}, &reply) &&
            reply.as_object().contains("error")) {
            error = std::string(reply.at("error").at("message").as_string());
        }
        check(error.find(std::to_string(proto::kMaxMeasureBatch)) != std::string::npos,
              "an over-cap batch is refused naming the cap");

        const auto refusedWith = [&](const boost::json::object& oneBlock, const char* needle,
                                     const char* what) {
            std::string refusal;
            measureOne(fd, nextId, oneBlock, &refusal);
            check(refusal.find(needle) != std::string::npos, what);
        };
        boost::json::object badSpacing = block("s", {"words"}, 40.0, 800.0, 9.0);
        refusedWith(badSpacing, "lineSpacing",
                    "a line spacing outside the layout's band is refused naming it");
        boost::json::object badWidth = block("s", {"words"}, 40.0, 0.0);
        refusedWith(badWidth, "width", "a zero width is refused naming it");
        boost::json::object noKey = block("", {"words"}, 40.0, 800.0);
        refusedWith(noKey, "key", "a block with no key is refused naming it");
        boost::json::object noLines;
        noLines["key"] = "s";
        noLines["fontSize"] = 40.0;
        noLines["width"] = 800.0;
        noLines["lineSpacing"] = 1.25;
        refusedWith(noLines, "lines", "a block with no lines array is refused naming it");
        std::vector<std::string> manyLines(512, "a line");
        refusedWith(block("s", manyLines, 40.0, 800.0), "lines",
                    "a block over the line cap is refused naming the lines");
        // And the session lives on: the very next honest ask is answered.
        std::string error2;
        const Measured after = measureOne(fd, nextId, block("ok", {"still here"}, 40.0, 800.0),
                                          &error2);
        check(after.ok && after.height > 0.0, "a refusal is per-request; the session lives on");
    }

    // An empty batch is legal and answers empty.
    {
        boost::json::value reply;
        if (call(fd, nextId++, proto::kMethodTextMeasure,
                 boost::json::object{{"blocks", boost::json::array{}}}, &reply) &&
            reply.as_object().contains("result")) {
            check(reply.at("result").at("measured").as_array().empty(),
                  "an empty batch answers with an empty measured list");
        } else {
            check(false, "an empty batch answers with an empty measured list");
        }
    }

    boost::json::value shutdownReply;
    call(fd, nextId++, "shutdown", {}, &shutdownReply);
    close(fd);
    int status = 0;
    waitpid(worker, &status, 0);
    unlink(gSocket.c_str());

    if (gFailed == 0) {
        std::printf("text_measure_tests: all fixtures passed\n");
        return 0;
    }
    std::printf("text_measure_tests: %d FAILURES\n", gFailed);
    return 1;
}
