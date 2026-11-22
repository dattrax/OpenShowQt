/*
 * Stage HDMI destination over the real worker (stage delivery FO.5a.2 /
 * FO.5b.2 / FO.5c.1).
 *
 * What this pins, at the production socket boundary:
 *
 *  1. The hello names stage.output.v1 and protocol minor 23.
 *  2. A stage configuration quoting a stale inventory generation is refused;
 *     acceptance records the request but `active` becomes true only after the
 *     worker has presented to the target.
 *  3. A prepared current/next bundle presents on the stage alone: the
 *     audience's published revision never moves, `stage.presented` names the
 *     bundle's revision, and `next` says the overlay was drawn.
 *  4. Clear All's stage half clears current/next (presented 0) while the
 *     audience keeps whatever it had; disabling tears the target down and
 *     leaves the audience answering status.
 *  5. Route collisions are refused in both directions; an invalid scene is
 *     refused without changing the retained stage or the audience.
 *
 * Needs a display and at least two attached monitors (one for the audience,
 * one for the stage). Skips with exit 77 when the environment cannot supply
 * them; offscreen success would not be this check.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openshow/protocol/protocol.h"

namespace {

namespace proto = openshow::protocol;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::string gWorkerBinary = "./openshow-worker";
const std::string gSocket = "/tmp/openshow-stage-" + std::to_string(getpid()) + ".sock";

pid_t spawnWorker() {
    const pid_t pid = fork();
    if (pid == 0) {
        execl(gWorkerBinary.c_str(), "openshow-worker", "--socket", gSocket.c_str(),
              "--monitor", "1", static_cast<char*>(nullptr));
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

void sendRaw(int fd, const boost::json::value& message) {
    std::string wire;
    proto::appendFrame(message, &wire);
    ::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL);
}

bool readExact(int fd, char* bytes, std::size_t count) {
    std::size_t got = 0;
    while (got < count) {
        const ssize_t n = ::recv(fd, bytes + got, count - got, 0);
        if (n <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

bool receiveOne(int fd, boost::json::value* out) {
    std::uint8_t header[4] = {};
    if (!readExact(fd, reinterpret_cast<char*>(header), sizeof(header))) {
        return false;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(header[0]) |
                                 (static_cast<std::uint32_t>(header[1]) << 8) |
                                 (static_cast<std::uint32_t>(header[2]) << 16) |
                                 (static_cast<std::uint32_t>(header[3]) << 24);
    if (length == 0 || length > proto::kMaxMessageBytes) {
        return false;
    }
    std::vector<char> payload(length);
    if (!readExact(fd, payload.data(), payload.size())) {
        return false;
    }
    try {
        *out = boost::json::parse(std::string(payload.begin(), payload.end()));
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

struct Client {
    int fd = -1;
    std::int64_t nextId = 1;

    // The raw `result` value of one request. Empty on a transport failure or
    // a worker error reply.
    boost::json::value callValue(const char* method, boost::json::object params = {},
                                 bool* transportOk = nullptr) {
        boost::json::object message{{"jsonrpc", "2.0"},
                                    {"id", nextId},
                                    {"method", method},
                                    {"params", std::move(params)}};
        const std::int64_t id = nextId++;
        sendRaw(fd, message);
        for (;;) {
            boost::json::value reply;
            if (!receiveOne(fd, &reply)) {
                if (transportOk) {
                    *transportOk = false;
                }
                return {};
            }
            if (!reply.is_object()) {
                continue;
            }
            const boost::json::object& object = reply.as_object();
            const auto replyId = object.find("id");
            if (replyId == object.end() || !replyId->value().is_number() ||
                static_cast<std::int64_t>(replyId->value().to_number<std::int64_t>()) != id) {
                continue;  // a notification from the worker
            }
            if (transportOk) {
                *transportOk = true;
            }
            if (const auto result = object.find("result"); result != object.end()) {
                return result->value();
            }
            return {};
        }
    }

    boost::json::object call(const char* method, boost::json::object params = {},
                             bool* transportOk = nullptr) {
        const boost::json::value result = callValue(method, std::move(params), transportOk);
        return result.is_object() ? result.as_object() : boost::json::object{};
    }

    boost::json::object status() { return call("output.status"); }
};

bool waitForStatus(Client* client, const std::function<bool(const boost::json::object&)>& done,
                   int attempts = 80) {
    for (int i = 0; i < attempts; ++i) {
        const boost::json::object status = client->status();
        if (done(status)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

boost::json::object stageBlock(const boost::json::object& status) {
    const auto stage = status.find("stage");
    return stage != status.end() && stage->value().is_object() ? stage->value().as_object()
                                                              : boost::json::object{};
}

bool readBool(const boost::json::object& object, const char* key, bool fallback = false) {
    const auto found = object.find(key);
    return found != object.end() && found->value().is_bool() ? found->value().as_bool()
                                                             : fallback;
}

std::int64_t readInt(const boost::json::object& object, const char* key,
                     std::int64_t fallback = 0) {
    const auto found = object.find(key);
    if (found == object.end() || !found->value().is_number()) {
        return fallback;
    }
    const boost::json::value& value = found->value();
    if (value.is_int64()) {
        return value.as_int64();
    }
    if (value.is_uint64()) {
        return static_cast<std::int64_t>(value.as_uint64());
    }
    return static_cast<std::int64_t>(value.as_double());
}

std::string readText(const boost::json::object& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->value().is_string()
               ? std::string(found->value().as_string())
               : std::string();
}

boost::json::object textScene(const char* slide, const std::vector<std::string>& lines,
                              double x, double y, double width, double height,
                              double fontSize) {
    boost::json::array itemLines;
    for (const std::string& line : lines) {
        itemLines.emplace_back(line);
    }
    boost::json::object item{
        {"kind", "text"},
        {"lines", std::move(itemLines)},
        {"color", 0xFFFFFF},
        {"fontFamily", ""},
        {"fontSize", fontSize},
        {"x", x},
        {"y", y},
        {"width", width},
        {"height", height},
        {"align", "center"},
        {"lineSpacing", 1.25},
        {"fontWeight", 400},
        {"fontStyle", "normal"},
        {"underline", false},
        {"fitMode", "shrink"},
        {"minFontSize", 12.0},
        {"verticalAlign", "center"},
        {"allCaps", false},
        {"stroke", boost::json::object{{"enabled", false}, {"color", 0}, {"width", 0.0}}},
        {"shadow",
         boost::json::object{{"enabled", false},
                             {"color", 0},
                             {"opacity", 0.0},
                             {"offsetX", 0.0},
                             {"offsetY", 0.0},
                             {"blurSigma", 0.0}}},
    };
    return boost::json::object{
        {"scene", 8},
        {"showId", "stage-test"},
        {"slideId", slide},
        {"backgroundColor", 0},
        {"background",
         boost::json::object{{"transparent", false},
                             {"image", false},
                             {"video", false},
                             {"fit", "cover"},
                             {"assetId", ""},
                             {"assetPath", ""}}},
        {"items", boost::json::array{std::move(item)}}};
}

boost::json::object bundleOf(const boost::json::object& current, bool withNext) {
    boost::json::object bundle{{"current", current}, {"overlap", false}};
    if (withNext) {
        bundle["next"] = textScene("next", {"NEXT CUE"}, 160.0, 740.0, 1600.0, 260.0, 52.8);
    }
    return bundle;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }
    const pid_t worker = spawnWorker();
    if (worker <= 0) {
        std::fprintf(stderr, "could not fork the worker\n");
        return 1;
    }
    Client client;
    client.fd = connectRetry();
    if (client.fd < 0) {
        std::fprintf(stderr, "could not connect to the worker\n");
        kill(worker, SIGTERM);
        return 1;
    }
    auto cleanup = [&]() {
        client.call("shutdown");
        close(client.fd);
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            if (waitpid(worker, &status, WNOHANG) == worker) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(worker, SIGTERM);
        waitpid(worker, &status, 0);
    };

    const boost::json::object hello = client.call(
        "hello", proto::helloParams("stage-output-test", proto::kProtocolMajor,
                                    proto::kProtocolMinor));
    if (hello.empty()) {
        std::fprintf(stderr, "hello failed\n");
        cleanup();
        return 1;
    }
    const boost::json::object protocol = hello.at("protocol").as_object();
    check(static_cast<int>(protocol.at("minor").to_number<std::uint64_t>()) == 23,
          "the worker speaks protocol minor 23");
    bool hasStageCapability = false;
    for (const boost::json::value& capability : hello.at("capabilities").as_array()) {
        if (capability.is_string() && capability.as_string() == proto::kCapabilityStageOutput) {
            hasStageCapability = true;
        }
    }
    check(hasStageCapability, "hello names stage.output.v1");

    const boost::json::value displaysValue = client.callValue("output.displays");
    const boost::json::array& displays =
        displaysValue.is_array() ? displaysValue.as_array() : boost::json::array{};
    if (displays.size() < 2) {
        std::fprintf(stderr, "SKIP: fewer than two displays attached\n");
        cleanup();
        return 77;
    }
    // The audience is on monitor 1 (the spawn argument); choose a different
    // attached panel for the stage, preferring a 1920x1080 target.
    boost::json::object stagePanel;
    for (const boost::json::value& row : displays) {
        const boost::json::object& panel = row.as_object();
        const int index = static_cast<int>(panel.at("index").to_number<std::int64_t>());
        if (index == 1) {
            continue;
        }
        if (stagePanel.empty() || static_cast<int>(panel.at("width").to_number<std::int64_t>()) ==
                                      1920) {
            stagePanel = panel;
        }
    }
    if (stagePanel.empty()) {
        std::fprintf(stderr, "SKIP: no stage display distinct from the audience\n");
        cleanup();
        return 77;
    }
    const std::int64_t stageIndex = stagePanel.at("index").to_number<std::int64_t>();
    const std::string stageIdentity = readText(stagePanel, "identity");
    check(!stageIdentity.empty(), "the display inventory carries connection-scoped identities");

    // The audience has to be on its fullscreen monitor before a collision can
    // be tested, and status is the acknowledgement.
    const bool audienceLanded = waitForStatus(&client, [](const boost::json::object& status) {
        return readInt(status, "monitor", -1) == 1;
    });
    check(audienceLanded, "the audience reached its requested monitor");

    // Stale inventory: refused by name, nothing enabled.
    boost::json::object configure =
        client.call("stage.configure",
                    boost::json::object{
                        {"enabled", true},
                        {"monitor", boost::json::object{{"index", stageIndex},
                                                        {"identity", stageIdentity},
                                                        {"inventory", 1}}},
                        {"layers", boost::json::array{"words", "next"}},
                        {"generation", 1}});
    check(!readBool(configure, "accepted", true),
          "a stale inventory generation is refused");
    check(readText(configure, "reason").find("display list changed") != std::string::npos,
          "the stale-inventory refusal names the reason");

    // Collision with the audience's panel (monitor 1).
    const boost::json::object audiencePanel = [&]() {
        for (const boost::json::value& row : displays) {
            if (row.as_object().at("index").to_number<std::int64_t>() == 1) {
                return row.as_object();
            }
        }
        return boost::json::object{};
    }();
    configure = client.call("stage.configure",
                            boost::json::object{
                                {"enabled", true},
                                {"monitor", boost::json::object{{"index", 1},
                                                                {"identity", readText(audiencePanel,
                                                                                      "identity")},
                                                                {"inventory", 0}}},
                                {"layers", boost::json::array{"words", "next"}},
                                {"generation", 2}});
    check(!readBool(configure, "accepted", true),
          "a stage on the audience's panel is refused");

    // Notes have no production data source: offering the cell in the UI is
    // wrong and the worker refuses it at the wire too.
    configure = client.call("stage.configure",
                            boost::json::object{
                                {"enabled", true},
                                {"monitor", boost::json::object{{"index", stageIndex},
                                                                {"identity", stageIdentity},
                                                                {"inventory", 0}}},
                                {"layers", boost::json::array{"words", "notes"}},
                                {"generation", 3}});
    check(!readBool(configure, "accepted", true),
          "speaker notes are refused on the stage");
    check(readText(configure, "reason").find("notes") != std::string::npos,
          "the notes refusal names the missing source");

    // Correct configuration: accepted, then active only after the worker has
    // actually presented (black counts).
    configure = client.call("stage.configure",
                            boost::json::object{
                                {"enabled", true},
                                {"monitor", boost::json::object{{"index", stageIndex},
                                                                {"identity", stageIdentity},
                                                                {"inventory", 0}}},
                                {"layers", boost::json::array{"words", "next"}},
                                {"generation", 3}});
    check(readBool(configure, "accepted", false), "a valid stage configuration is accepted");
    const bool stageActivated = waitForStatus(&client, [](const boost::json::object& status) {
        const boost::json::object stage = stageBlock(status);
        return readBool(stage, "active") && readInt(stage, "width") > 0;
    });
    check(stageActivated, "the stage target becomes active with a real extent");

    const boost::json::object current =
        textScene("current", {"CURRENT", "CUE"}, 160.0, 120.0, 1600.0, 560.0, 96.0);
    const boost::json::object before = client.status();
    const boost::json::object prepared =
        client.call("stage.prepare",
                    boost::json::object{{"bundle", bundleOf(current, true)},
                                        {"generation", 3}});
    check(readBool(prepared, "accepted", false), "the current/next bundle is filed");
    check(readBool(prepared, "next", false), "the bundle carries a next overlay");
    const std::int64_t revision = readInt(prepared, "revision");
    check(revision > 0, "the stage bundle has its own revision");

    const boost::json::object presented =
        client.call("stage.present", boost::json::object{{"revision", revision},
                                                         {"generation", 3}});
    check(readBool(presented, "accepted", false), "the stage bundle is eligible");
    const bool stagePresented = waitForStatus(
        &client, [revision](const boost::json::object& status) {
            const boost::json::object stage = stageBlock(status);
            return readInt(stage, "presented") == revision && readBool(stage, "next");
        });
    check(stagePresented, "the stage presents the current cue and the next overlay");
    const boost::json::object after = client.status();
    const boost::json::object stageState = stageBlock(after);
    check(readInt(stageState, "frames") > 0, "the stage frame counter advanced");
    check(readInt(stageState, "presentFailures") == 0,
          "no stage acquire/present failures were recorded");
    const auto worstPresent = stageState.find("worstPresentMs");
    if (worstPresent != stageState.end() && worstPresent->value().is_number()) {
        const double ms = worstPresent->value().to_number<double>();
        std::printf("stage worst present %.3f ms, worst acquire %.3f ms\n", ms,
                    stageState.at("worstAcquireMs").to_number<double>());
        check(ms < 100.0, "the stage present stays inside its measured budget");
    }
    check(readInt(after, "revision") == readInt(before, "revision"),
          "the stage present moves no audience revision");
    check(readInt(after, "monitor") == 1, "the stage present moves no audience monitor");

    // Clear All's stage half keeps the target up but clears current/next.
    client.call("stage.clear", boost::json::object{{"generation", 4}});
    check(waitForStatus(&client, [](const boost::json::object& status) {
              const boost::json::object stage = stageBlock(status);
              return readInt(stage, "presented") == 0 && !readBool(stage, "next");
          }),
          "Clear All clears the stage current and next regions");

    // An invalid scene is refused without disturbing the retained bundle.
    boost::json::object invalid = textScene("bad", {"BAD"}, 160.0, 120.0, 1600.0, 560.0, 96.0);
    invalid["scene"] = 9;
    const boost::json::object refused =
        client.call("stage.prepare",
                    boost::json::object{{"bundle", bundleOf(invalid, false)},
                                        {"generation", 4}});
    check(refused.empty(), "an unsupported scene format is refused by the worker");
    check(client.status().contains("stage"), "the worker still answers status after refusal");

    // Disabling is target-local: stage inactive, audience still live.
    const boost::json::object disabled =
        client.call("stage.configure",
                    boost::json::object{{"enabled", false},
                                        {"layers", boost::json::array{"words", "next"}},
                                        {"generation", 5}});
    check(readBool(disabled, "accepted", false), "disabling the stage is always accepted");
    check(waitForStatus(&client, [](const boost::json::object& status) {
              const boost::json::object stage = stageBlock(status);
              return !readBool(stage, "active");
          }),
          "the stage target is retired");
    const boost::json::object stillLive = client.status();
    check(readInt(stillLive, "monitor") == 1,
          "the audience keeps presenting while the stage is disabled");

    // With the stage disabled, the audience may use the panel, and then the
    // stage may not take it back in the other direction.
    client.call("output.select", boost::json::object{{"monitor", stageIndex}});
    check(waitForStatus(&client, [stageIndex](const boost::json::object& status) {
              return readInt(status, "monitor") == stageIndex;
          }),
          "the audience may move onto a panel no stage owns");
    configure = client.call("stage.configure",
                            boost::json::object{
                                {"enabled", true},
                                {"monitor", boost::json::object{{"index", stageIndex},
                                                                {"identity", stageIdentity},
                                                                {"inventory", 0}}},
                                {"layers", boost::json::array{"words", "next"}},
                                {"generation", 6}});
    check(!readBool(configure, "accepted", true),
          "a stage cannot take the audience's current panel");

    cleanup();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d stage output check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("stage output checks passed\n");
    return 0;
}