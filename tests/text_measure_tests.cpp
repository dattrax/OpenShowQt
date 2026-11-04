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

#include <algorithm>
#include <cerrno>
#include <chrono>
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
    // Task TS.3's additive reply keys: -1 unknown (no key), 0 no, 1 yes.
    int fits = -1;
    bool hasBounds = false;
    bool boundsEmpty = true;
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
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
    const boost::json::object& row = rows[0].as_object();
    measured.ok = true;
    measured.key = std::string(row.at("key").as_string());
    measured.height = row.at("height").to_number<double>();
    measured.lineCount = row.at("lineCount").to_number<int>();
    // TS.3: the additive keys. The rows the worker answers carry them now;
    // reading them optionally keeps the harness honest about what is a
    // verdict and what is silence.
    const auto fitsEntry = row.find("fits");
    if (fitsEntry != row.end() && fitsEntry->value().is_bool()) {
        measured.fits = fitsEntry->value().as_bool() ? 1 : 0;
    }
    const auto boundsEntry = row.find("bounds");
    if (boundsEntry != row.end() && boundsEntry->value().is_object()) {
        const boost::json::object& box = boundsEntry->value().as_object();
        measured.hasBounds = true;
        measured.boundsEmpty = box.at("empty").as_bool();
        measured.left = box.at("left").to_number<double>();
        measured.top = box.at("top").to_number<double>();
        measured.right = box.at("right").to_number<double>();
        measured.bottom = box.at("bottom").to_number<double>();
    }
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
        bool hasMeasureV2 = false;
        for (const boost::json::value& capability :
             reply.at("result").at("capabilities").as_array()) {
            hasMeasure = hasMeasure || capability.as_string() == "text.measure.v1";
            hasMeasureV2 = hasMeasureV2 || capability.as_string() == proto::kCapabilityTextMeasureV2;
        }
        check(hasMeasure, "the hello names text.measure.v1");
        // TS.3: a frontend may only put a styled block to a worker that
        // names this. The check here is the worker half of that gate.
        check(hasMeasureV2, "the hello names text.measure.v2");
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

    // --- Task TS.3: the style block measures, the bounds answer, the
    // verdict says what it means ---
    {
        std::string error;
        const auto refusedWith = [&](const boost::json::object& oneBlock, const char* needle,
                                     const char* what) {
            std::string refusal;
            measureOne(fd, nextId, oneBlock, &refusal);
            check(refusal.find(needle) != std::string::npos, what);
        };

        // Every answer grows the additive keys — even a v1-shaped ask.
        const Measured plain =
            measureOne(fd, nextId, block("t1", {"Hello world"}, 40.0, 800.0), &error);
        check(plain.ok && plain.fits == 1 && plain.hasBounds && !plain.boundsEmpty,
              "a v1-shaped measurement still answers with fits and bounds");
        check(plain.right > plain.left && plain.bottom > plain.top &&
                  plain.left >= -2.0 && plain.right <= 802.0 &&
                  plain.bottom - plain.top <= plain.height + 2.0,
              "the bounds sit inside the measured line-metrics box when no effect is enabled");

        // The spelled defaults ARE the absent keys: a v1 payload means
        // exactly what it meant before — same numbers, same verdict,
        // same box, to the last bit the layout produced.
        const Measured implicitDefault =
            measureOne(fd, nextId, block("t2", {"Hello world again"}, 40.0, 800.0), &error);
        boost::json::object explicitDefault = block("t2", {"Hello world again"}, 40.0, 800.0);
        explicitDefault["fontWeight"] = 400;
        explicitDefault["fontStyle"] = "normal";
        explicitDefault["fitMode"] = "shrink";
        explicitDefault["minFontSize"] = 12.0;
        explicitDefault["stroke"] = boost::json::object{
            {"enabled", false}, {"color", std::uint64_t(0x000000)}, {"width", 2.0}};
        explicitDefault["shadow"] = boost::json::object{{"enabled", false},
                                                        {"color", std::uint64_t(0x000000)},
                                                        {"opacity", 0.65},
                                                        {"offsetX", 2.0},
                                                        {"offsetY", 2.0},
                                                        {"blurSigma", 4.0}};
        const Measured spelledDefault = measureOne(fd, nextId, explicitDefault, &error);
        check(spelledDefault.ok && implicitDefault.ok &&
                  spelledDefault.height == implicitDefault.height &&
                  spelledDefault.lineCount == implicitDefault.lineCount &&
                  spelledDefault.left == implicitDefault.left &&
                  spelledDefault.right == implicitDefault.right &&
                  spelledDefault.top == implicitDefault.top &&
                  spelledDefault.bottom == implicitDefault.bottom &&
                  spelledDefault.fits == implicitDefault.fits,
              "spelling the style defaults measures bit-identically to sending no style at all");

        // Weight is not decoration: it changes WHICH FACE measures the
        // words, and the face is wider. The single-line probe proves the
        // typography swapped; the wrap probe proves the growth reaches
        // the packer's numbers (more width cannot fit fewer lines).
        boost::json::object light = block("w100", {"Handgloves and weight"}, 40.0, 800.0);
        light["fontWeight"] = 100;
        boost::json::object heavy = block("w800", {"Handgloves and weight"}, 40.0, 800.0);
        heavy["fontWeight"] = 800;
        const Measured lightLine = measureOne(fd, nextId, light, &error);
        const Measured heavyLine = measureOne(fd, nextId, heavy, &error);
        std::string flowing;
        for (int i = 0; i < 10; ++i) {
            flowing += "the quick brown fox jumps over the lazy dog ";
        }
        boost::json::object lightWrap = block("w100b", {flowing}, 40.0, 700.0);
        lightWrap["fontWeight"] = 100;
        boost::json::object heavyWrap = block("w800b", {flowing}, 40.0, 700.0);
        heavyWrap["fontWeight"] = 800;
        const Measured lightWrapped = measureOne(fd, nextId, lightWrap, &error);
        const Measured heavyWrapped = measureOne(fd, nextId, heavyWrap, &error);
        check(lightLine.ok && heavyLine.ok && heavyLine.right > lightLine.right + 0.5,
              "weight 800 measures WIDER than weight 100 — the ask reached the face, "
              "not the defaults");
        check(lightWrapped.ok && heavyWrapped.ok &&
                  heavyWrapped.lineCount >= lightWrapped.lineCount &&
                  heavyWrapped.height >= lightWrapped.height,
              "bolder words wrap to no fewer lines in the same box");

        // A named slant measures without disturbing the verdict.
        boost::json::object italic = block("it", {"Slanted words"}, 40.0, 800.0);
        italic["fontStyle"] = "italic";
        const Measured slanted = measureOne(fd, nextId, italic, &error);
        check(slanted.ok && slanted.height > 0.0 && slanted.fits == 1,
              "italic measures with its own verdict");

        // The stroke grows the visual extent by exactly half its pen on
        // every side — the pen reaches that far beyond the outline.
        const Measured bare = measureOne(fd, nextId, block("e1", {"Edged words"}, 40.0, 800.0),
                                         &error);
        boost::json::object stroked = block("e2", {"Edged words"}, 40.0, 800.0);
        stroked["stroke"] = boost::json::object{
            {"enabled", true}, {"color", std::uint64_t(0xFFFFFF)}, {"width", 20.0}};
        const Measured edged = measureOne(fd, nextId, stroked, &error);
        check(bare.ok && edged.ok && edged.left <= bare.left - 9.0 &&
                  edged.right >= bare.right + 9.0 && edged.top <= bare.top - 9.0 &&
                  edged.bottom >= bare.bottom + 9.0,
              "an enabled stroke grows the bounds by its half-width on every side");

        // The shadow unions the offset copy grown by the finite blur
        // support (~3.1 sigma): the extent must be honest in both
        // directions — big enough for any pixel (the reach uses a
        // deliberately smaller 3.0 sigma), and no wild margin beyond
        // the 3.1 the code claims. A positive offset still lets the
        // blur pull the OPPOSITE edge in: dx=10 with a 12.4 reach moves
        // the left edge to fill.left − 2.4, and dy=14 pulls nothing off
        // the top while lifting nothing above bare.top either.
        boost::json::object shaded = block("s1", {"Edged words"}, 40.0, 800.0);
        shaded["shadow"] = boost::json::object{{"enabled", true},
                                               {"color", std::uint64_t(0x000000)},
                                               {"opacity", 0.5},
                                               {"offsetX", 10.0},
                                               {"offsetY", 14.0},
                                               {"blurSigma", 4.0}};
        const Measured castShadow = measureOne(fd, nextId, shaded, &error);
        const double reachLow = 3.0 * 4.0;   // below Skia's ~3.09 sigma support
        const double reachHigh = 3.1 * 4.0;  // the support the code claims
        check(castShadow.ok &&
                  castShadow.right >= bare.right + 10.0 + reachLow &&
                  castShadow.bottom >= bare.bottom + 14.0 + reachLow &&
                  castShadow.right <= bare.right + 10.0 + reachHigh + 0.5 &&
                  castShadow.bottom <= bare.bottom + 14.0 + reachHigh + 0.5 &&
                  castShadow.left >= bare.left + std::min(0.0, 10.0 - reachHigh) - 0.5 &&
                  castShadow.left <= bare.left + 0.5 &&
                  castShadow.top >= bare.top + std::min(0.0, 14.0 - reachHigh) - 0.5 &&
                  castShadow.top <= bare.top + 0.5,
              "an offset blur shadow unions the offset copy grown by its blur support — "
              "and nothing further");

        // The fits verdict is about the FILL; the bounds carry the spill.
        // Words that fit stay "fits" even when the stroke's outer half
        // hangs past the box — moving words cannot shorten a pen. The
        // box here is the fill's natural width plus a hair: the centred
        // lines stay inside it (verdict: fits) while the pen's outer
        // half clearly crosses the edge (bounds: past width).
        {
            const double naturalWidth = bare.right - bare.left;  // centred at 800
            boost::json::object tight = block("spill", {"Edged words"}, 40.0, naturalWidth + 5.0);
            tight["stroke"] = boost::json::object{
                {"enabled", true}, {"color", std::uint64_t(0xFFFFFF)}, {"width", 20.0}};
            const Measured spilled = measureOne(fd, nextId, tight, &error);
            check(spilled.ok && spilled.fits == 1 && spilled.right > naturalWidth + 5.0,
                  "a stroke whose outer edge passes the box still measures fits — "
                  "the spill is reported in the bounds, not decided in the verdict");
        }

        // A word wider than the box does not overflow it: the layout
        // breaks the word across lines, so the FILL stays inside and the
        // words keep their verdict. (This is the promise the packer
        // relies on: nothing is ever clipped; the words move.)
        const Measured wideWord = measureOne(
            fd, nextId, block("t4", {"Supercalifragilisticexpialidocious"}, 60.0, 120.0), &error);
        check(wideWord.ok && wideWord.lineCount >= 2 && wideWord.fits == 1 &&
                  wideWord.right <= 122.0,
              "a word wider than the box breaks across lines — words move, nothing clips");
        boost::json::object shortBox =
            block("t5", {"line one", "line two", "line three"}, 40.0, 800.0);
        shortBox["height"] = 30.0;
        const Measured squeezed = measureOne(fd, nextId, shortBox, &error);
        check(squeezed.ok && squeezed.fits == 0,
              "a block asked against a box shorter than itself answers fits=false");
        boost::json::object tallBox =
            block("t6", {"line one", "line two", "line three"}, 40.0, 800.0);
        tallBox["height"] = 400.0;
        const Measured roomy = measureOne(fd, nextId, tallBox, &error);
        check(roomy.ok && roomy.fits == 1, "the same block against a tall enough box fits");

        // Refusals: the style block keeps the wire's discipline — every
        // band, every closed record, every key said by name.
        boost::json::object badWeight = block("r", {"words"}, 40.0, 800.0);
        badWeight["fontWeight"] = 50;
        refusedWith(badWeight, "fontWeight", "a weight under 100 is refused naming it");
        badWeight["fontWeight"] = 450.5;
        refusedWith(badWeight, "fontWeight", "a fractional weight is refused naming it");
        boost::json::object badSlant = block("r", {"words"}, 40.0, 800.0);
        badSlant["fontStyle"] = "oblique";
        refusedWith(badSlant, "fontStyle", "a slant outside normal/italic is refused naming it");
        boost::json::object badFit = block("r", {"words"}, 40.0, 800.0);
        badFit["fitMode"] = "squeeze";
        refusedWith(badFit, "fitMode", "a fit mode outside shrink/fixed is refused naming it");
        boost::json::object badFloor = block("r", {"words"}, 40.0, 800.0);
        badFloor["minFontSize"] = 0.5;
        refusedWith(badFloor, "minFontSize", "a floor under one pixel is refused naming it");
        boost::json::object badHeight = block("r", {"words"}, 40.0, 800.0);
        badHeight["height"] = 0.0;
        refusedWith(badHeight, "height", "a zero box height is refused naming it");
        boost::json::object strokeNotRecord = block("r", {"words"}, 40.0, 800.0);
        strokeNotRecord["stroke"] = 3;
        refusedWith(strokeNotRecord, "stroke", "a stroke that is not a record is refused naming it");
        boost::json::object strokeNoColor = block("r", {"words"}, 40.0, 800.0);
        strokeNoColor["stroke"] = boost::json::object{
            {"enabled", true}, {"width", 2.0}};
        refusedWith(strokeNoColor, "color", "a half-written stroke record is refused naming the member");
        boost::json::object strokeWide = block("r", {"words"}, 40.0, 800.0);
        strokeWide["stroke"] = boost::json::object{
            {"enabled", true}, {"color", std::uint64_t(0)}, {"width", 40.0}};
        refusedWith(strokeWide, "width", "a stroke over the pen cap is refused naming it");
        boost::json::object strokeUnknown = block("r", {"words"}, 40.0, 800.0);
        strokeUnknown["stroke"] = boost::json::object{
            {"enabled", true}, {"color", std::uint64_t(0)}, {"width", 2.0}, {"glow", false}};
        refusedWith(strokeUnknown, "glow", "a stroke member this contract does not have is refused by name");
        boost::json::object shadowOpacity = block("r", {"words"}, 40.0, 800.0);
        shadowOpacity["shadow"] = boost::json::object{{"enabled", true},
                                                      {"color", std::uint64_t(0)},
                                                      {"opacity", 2.0},
                                                      {"offsetX", 0.0},
                                                      {"offsetY", 0.0},
                                                      {"blurSigma", 0.0}};
        refusedWith(shadowOpacity, "opacity", "a shadow opacity over 1 is refused naming it");
        boost::json::object shadowOffset = block("r", {"words"}, 40.0, 800.0);
        shadowOffset["shadow"] = boost::json::object{{"enabled", true},
                                                     {"color", std::uint64_t(0)},
                                                     {"opacity", 0.5},
                                                     {"offsetX", 0.0},
                                                     {"offsetY", -300.0},
                                                     {"blurSigma", 0.0}};
        refusedWith(shadowOffset, "offsetY", "a shadow offset past its band is refused naming it");
        boost::json::object shadowBlur = block("r", {"words"}, 40.0, 800.0);
        shadowBlur["shadow"] = boost::json::object{{"enabled", true},
                                                   {"color", std::uint64_t(0)},
                                                   {"opacity", 0.5},
                                                   {"offsetX", 0.0},
                                                   {"offsetY", 0.0},
                                                   {"blurSigma", 100.0}};
        refusedWith(shadowBlur, "blurSigma", "a blur past the cap is refused naming it");
        boost::json::object shadowColor = block("r", {"words"}, 40.0, 800.0);
        shadowColor["shadow"] = boost::json::object{{"enabled", true},
                                                    {"color", std::uint64_t(0x1000000)},
                                                    {"opacity", 0.5},
                                                    {"offsetX", 0.0},
                                                    {"offsetY", 0.0},
                                                    {"blurSigma", 0.0}};
        refusedWith(shadowColor, "color", "a colour past 0xFFFFFF is refused naming it");

        // And the session lives on after all of it.
        const Measured after = measureOne(fd, nextId, block("live", {"still here"}, 40.0, 800.0),
                                          &error);
        check(after.ok && after.height > 0.0,
              "style refusals are per-request; the measurement session lives on");
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
