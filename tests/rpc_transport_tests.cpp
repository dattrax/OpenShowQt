/*
 * Task 6.8b acceptance against a real openshow-worker (review R11): what the
 * control transport does with a frontend that stops paying attention.
 *
 * The finding was a blocking send() in the middle of a polled loop. One client
 * that stopped reading was enough to stop command handling, stop disconnect
 * detection, and leave shutdown waiting on a thread parked inside the kernel —
 * and the bytes queued in front of it had no shape at all. Four properties are
 * pinned here, because each fails in its own way:
 *
 *   T1 — a peer that reads NOTHING is dropped by the write deadline alone: no
 *        further traffic from the peer, nothing that would wake the worker. It
 *        then exits, promptly, so shutdown stayed bounded with a frontend
 *        frozen mid-conversation, and the log says the frontend stopped reading
 *        rather than leaving a silent hangup.
 *   T2 — a peer that asks for four times the queue budget receives no more than
 *        the budget plus the sockets' own room, is ended by that bound LONG
 *        before the deadline would have, and the log names the budget.
 *   T3 — a peer that hiccups has every one of its commands answered: a full
 *        queue is allowed to be full, never forgetful — and the preview
 *        notifications it missed arrive reduced to what is current instead of a
 *        transcript of every frame, with the connection and the session intact.
 *   T4 — a peer that reads a trickle for longer than the deadline is never
 *        dropped, because the deadline measures PROGRESS and not how long the
 *        queue has happened to be non-empty.
 *
 * The counter-mutants are the reason for the split: a deadline measured from
 * "queue not empty" instead of "no byte has gone out" passes T1 and fails T4; a
 * queue that drops the wrong thing passes T1 and T2 and fails T3.
 *
 * Needs a display (the worker opens its output window), so it carries the
 * `display` label. Every scenario owns its worker: a dropped frontend ends the
 * process, so there is nothing to reuse across scenarios. Pass T1..T4 as the
 * second argument to run one of them while tuning.
 */

#include <boost/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openshow/protocol/preview_wire.h"
#include "openshow/protocol/protocol.h"

#include "rpc/OutboundQueue.h"

namespace {

namespace proto = openshow::protocol;
namespace queue = openshow::worker;

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

std::string gWorkerBinary = "./openshow-worker";

std::string socketPath(const char* tag) {
    return "/tmp/openshow-68b-" + std::to_string(getpid()) + "-" + tag + ".sock";
}

std::string logPath(const char* tag) {
    return "/tmp/openshow-68b-" + std::to_string(getpid()) + "-" + tag + ".log";
}

std::string readFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    std::string text;
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        text.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return text;
}

// --- one worker, one frontend, one log --------------------------------------

/*
 * A frontend that reads when the scenario says so.
 *
 * Everything decoded goes into `inbox` and only the message a caller is
 * waiting for is taken out of it. That matters here more than in an ordinary
 * fixture: several of these scenarios are precisely about the notifications the
 * worker pushes while the frontend is looking at something else, and a helper
 * that answered a request by reading a buffer and throwing away whatever else
 * was in it would lose exactly the evidence the scenario is about (and would
 * then starve the preview pipeline, because slots are only freed by a release
 * the frontend could not have sent).
 */
struct Session {
    pid_t pid = -1;
    int fd = -1;
    proto::Decoder decoder;
    std::string log;
    std::int64_t nextId = 1;
    bool eof = false;
    std::deque<boost::json::value> inbox;
};

bool spawn(Session* session, const char* tag) {
    const std::string path = socketPath(tag);
    session->log = logPath(tag);
    const pid_t pid = fork();
    if (pid == 0) {
        const int out = ::open(session->log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0) {
            ::dup2(out, STDOUT_FILENO);
            ::dup2(out, STDERR_FILENO);
        }
        execl(gWorkerBinary.c_str(), "openshow-worker", "--socket", path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    session->pid = pid;
    return pid > 0;
}

/*
 * `receiveBufferBytes` goes on the frontend socket BEFORE connecting: a unix
 * stream pair takes its sizing from the connecting socket, so a small value
 * makes the worker run out of socket buffer after a few frames rather than
 * after a couple of hundred. Without it these scenarios would be about how
 * generous this machine's defaults are instead of about a stalled frontend.
 */
bool connectTo(Session* session, const std::string& path, unsigned receiveBufferBytes) {
    for (int attempt = 0; attempt < 120; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        if (receiveBufferBytes > 0 &&
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receiveBufferBytes,
                         sizeof(receiveBufferBytes)) != 0) {
            close(fd);
            return false;
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            session->fd = fd;
            return true;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Writes a framed message without waiting for anything to come back. Failure is
// expected once the worker has had enough of a frontend that will not read.
bool sendOnly(Session* session, const boost::json::value& message) {
    std::string wire;
    if (!proto::appendFrame(message, &wire)) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const ssize_t written =
            ::send(session->fd, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool sendRequestOnly(Session* session, const char* method, boost::json::object params) {
    return sendOnly(session,
                    proto::makeRequest(session->nextId++, method, std::move(params)));
}

/*
 * Reads for up to `ms`, at most `maxBytes`, and puts everything decoded in the
 * inbox. An empty result is an observation, not a failure: it is what a stalled
 * server with nothing to say produces. A stream the fixture's own decoder
 * rejects is always a failure here — a torn, truncated or reordered frame is the
 * exact bug class of a queue that replaced bytes already on their way out.
 */
std::size_t pump(Session* session, int ms, std::size_t maxBytes) {
    std::size_t bytesRead = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (bytesRead < maxBytes && std::chrono::steady_clock::now() < deadline) {
        pollfd pending{session->fd, POLLIN, 0};
        const int ready = ::poll(&pending, 1, 20);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }
        // The recv is capped by what the caller still wants, not only by the
        // buffer: a scenario that asks for a trickle of 1 KiB per tick gets
        // one, which is how T5 keeps a 200 KiB debt outstanding across its
        // verdict instead of drinking the whole thing in the first read.
        char buffer[8192];
        const std::size_t want = std::min(sizeof(buffer), maxBytes - bytesRead);
        const ssize_t count = ::recv(session->fd, buffer, want, 0);
        if (count == 0) {
            session->eof = true;
            break;
        }
        if (count < 0) {
            // ECONNRESET is how a peer that closed with bytes still unread
            // announces itself: the stream is over either way.
            session->eof = true;
            break;
        }
        bytesRead += static_cast<std::size_t>(count);
        session->decoder.feed(std::string_view(buffer, static_cast<std::size_t>(count)));
        boost::json::value message;
        std::string error;
        while (session->decoder.next(&message, &error)) {
            session->inbox.push_back(std::move(message));
        }
        if (session->decoder.failed()) {
            std::fprintf(stderr, "  the frontend's decoder rejected the stream: %s\n",
                         error.c_str());
            ++gFailures;
            return bytesRead;
        }
    }
    return bytesRead;
}

bool isReplyTo(const boost::json::value& message, std::int64_t id) {
    if (!message.is_object() || message.as_object().contains("method")) {
        return false;
    }
    const auto field = message.as_object().find("id");
    if (field == message.as_object().end()) {
        return false;
    }
    return field->value().is_int64() && field->value().as_int64() == id;
}

// Takes one reply out of the inbox, leaving every other message where it was.
bool takeReply(Session* session, std::int64_t id, boost::json::value* reply) {
    for (auto entry = session->inbox.begin(); entry != session->inbox.end(); ++entry) {
        if (isReplyTo(*entry, id)) {
            *reply = std::move(*entry);
            session->inbox.erase(entry);
            return true;
        }
    }
    return false;
}

// Takes every notification of one method out of the inbox, in arrival order.
std::vector<boost::json::value> takeNotifications(Session* session, std::string_view method) {
    std::vector<boost::json::value> taken;
    for (auto entry = session->inbox.begin(); entry != session->inbox.end();) {
        const boost::json::value& message = *entry;
        const auto field = message.is_object() ? message.as_object().find("method") : nullptr;
        if (field != nullptr && field->value().is_string() &&
            std::string_view(field->value().as_string()) == method) {
            taken.push_back(std::move(*entry));
            entry = session->inbox.erase(entry);
        } else {
            ++entry;
        }
    }
    return taken;
}

// Sends a request and waits for its answer, keeping everything else that came
// with it. The five seconds is generous on purpose: these scenarios want to
// know whether an answer exists at all, and T1..T4 bound the interesting
// durations themselves.
bool call(Session* session, std::int64_t id, const char* method, boost::json::object params,
          boost::json::value* reply) {
    if (!sendOnly(session, proto::makeRequest(id, method, std::move(params)))) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (takeReply(session, id, reply)) {
            return true;
        }
        pump(session, 40, 1u << 20);
    }
    return takeReply(session, id, reply);
}

std::int64_t numberAt(const boost::json::value& object, const char* key) {
    if (!object.is_object()) {
        return -1;
    }
    const auto field = object.as_object().find(key);
    if (field == object.as_object().end() || !field->value().is_number()) {
        return -1;
    }
    return static_cast<std::int64_t>(field->value().to_number<std::uint64_t>());
}

// Frees every slot the worker has handed over, so the preview pipeline keeps
// claiming and publishing instead of filling its three slots and going quiet.
int releaseAll(Session* session, std::vector<boost::json::value>* ready) {
    int released = 0;
    for (const auto& message : takeNotifications(session, proto::kMethodPreviewReady)) {
        const boost::json::value& params = message.at("params");
        sendOnly(session, proto::makeNotification(proto::kMethodPreviewReleased,
                                                  boost::json::object{
                                                      {"slot", numberAt(params, "slot")},
                                                      {"seq", numberAt(params, "seq")},
                                                      {"stream", std::string(params.at("stream").as_string())}}));
        if (ready) {
            ready->push_back(message);
        }
        ++released;
    }
    return released;
}

// Waits (bounded) for the worker to exit on its own. Returns the milliseconds it
// took, -2 for a nonzero exit, or -1 when it is still running at the deadline.
long waitExit(Session* session, int ms) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        int status = 0;
        const pid_t gone = ::waitpid(session->pid, &status, WNOHANG);
        if (gone == session->pid) {
            session->pid = -1;
            const long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? elapsed : -2;
        }
        if (gone < 0) {
            session->pid = -1;
            return -3;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count() >= ms) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void killWorker(Session* session) {
    if (session->pid > 0) {
        ::kill(session->pid, SIGKILL);
        ::waitpid(session->pid, nullptr, 0);
        session->pid = -1;
    }
    if (session->fd >= 0) {
        close(session->fd);
        session->fd = -1;
    }
}

void closeFrontend(Session* session) {
    if (session->fd >= 0) {
        close(session->fd);
        session->fd = -1;
    }
}

// Spawns, connects with a deliberately small receive buffer, handshakes.
bool openReady(Session* session, const char* tag, unsigned receiveBufferBytes) {
    const std::string path = socketPath(tag);
    ::unlink(path.c_str());
    if (!spawn(session, tag)) {
        return false;
    }
    if (!connectTo(session, path, receiveBufferBytes)) {
        std::fprintf(stderr, "  %s: could not connect to the worker\n", tag);
        return false;
    }
    boost::json::value reply;
    return call(session, session->nextId++, "hello",
                proto::helloParams("transport-test", proto::kProtocolMajor,
                                   proto::kProtocolMinor),
                &reply) &&
           reply.as_object().contains("result");
}

/*
 * The exact size, on the wire, of one output.status reply — measured from a
 * reply the worker actually sent rather than assumed, because T1 has to flood
 * the frontend's socket buffers WITHOUT approaching the worker's own queue
 * budget, and those two are only a factor of a few apart. A framed message is
 * its four-byte length plus its serialization, and re-serializing the same
 * values here costs the same number of bytes the worker spent.
 */
std::size_t measureStatusFrameBytes(Session* session) {
    std::size_t largest = 0;
    for (int i = 0; i < 2; ++i) {
        boost::json::value reply;
        if (!call(session, session->nextId++, "output.status", {}, &reply)) {
            return 0;
        }
        largest = std::max(largest, boost::json::serialize(reply).size() + 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    return largest;
}

// --- T1: a frontend that reads nothing is dropped by the deadline -----------

/*
 * The old blocking write left this situation parked: the connection stayed
 * "up", the commands stayed unanswered, and shutdown waited on a thread the
 * kernel was holding. Sized so the flood is far more than the frontend's
 * buffers can hold and far less than the worker's queue budget — so it is
 * unambiguously the deadline that ends it, which the log check confirms.
 */
void testStalledPeerIsDroppedInBoundedTime() {
    Session session;
    if (!openReady(&session, "stalled", 32u * 1024u)) {
        check(false, "T1: a worker and a handshaked frontend");
        killWorker(&session);
        return;
    }
    const std::size_t frameBytes = measureStatusFrameBytes(&session);
    if (frameBytes == 0) {
        check(false, "T1: a measured status reply to size the flood from");
        killWorker(&session);
        return;
    }

    // 45 % of ONE FRAME BUDGET, stated in the transport's own limit: enough
    // that the queue is the thing holding the bytes, and little enough that the
    // byte bound is nowhere near being reached, so what ends this scenario can
    // only be the deadline. Not the queue's constant, for the reason T2 gives.
    const int requests = std::max(8, static_cast<int>(proto::kMaxMessageBytes * 45 / 100 /
                                                      frameBytes));
    int written = 0;
    for (int i = 0; i < requests; ++i) {
        if (!sendRequestOnly(&session, "output.status", {})) {
            break;
        }
        ++written;
    }
    check(written == requests, "T1: the flood of commands was written to the worker");

    // Nothing is read from here on. The socket buffers fill, the worker's queue
    // fills behind them, and from that moment the worker is on its own with a
    // peer that has stopped — with no further traffic to wake it.
    const auto start = std::chrono::steady_clock::now();
    const long exitedIn = waitExit(&session, 8000);
    const long elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    check(exitedIn >= 0 || exitedIn == -2,
          "T1: the worker gave up on a frontend that read nothing, without being touched "
          "again (a blocking send() would sit here forever)");
    check(exitedIn != -2, "T1: and it exits with its own status, cleanly, mid-stall");
    check(exitedIn == -1 || elapsed >= 1500,
          "T1: it did not drop the frontend the instant the socket buffer filled — a full "
          "buffer is a busy peer, not a dead one");
    check(elapsed <= 6000, "T1: and it did not take much longer than the write deadline");
    std::printf("  T1: %d replies of %zu bytes owed; the frontend was dropped and the worker "
                "gone %ld ms after it last took a byte\n",
                written, frameBytes, elapsed);

    const std::string log = readFile(session.log);
    check(log.find("has not taken a byte") != std::string::npos,
          "T1: the worker says in its own words that the frontend stopped reading");
    check(log.find("budget") == std::string::npos,
          "T1: and it was the deadline that ended it, not the byte budget — the queue "
          "stayed inside its bound");
    check(log.find("frontend disconnected, stopping") != std::string::npos,
          "T1: dropping the stalled frontend ends the worker's life, as any disconnect does");
    if (log.find("has not taken a byte") == std::string::npos) {
        std::fprintf(stderr, "  T1 log tail: %s\n",
                     log.substr(log.size() > 600 ? log.size() - 600 : 0).c_str());
    }
    closeFrontend(&session);
    ::unlink(socketPath("stalled").c_str());
    ::unlink(session.log.c_str());
}

// --- T2: what is owed stays bounded -----------------------------------------

/*
 * Four times the queue's budget in requested output, from a frontend that reads
 * nothing until the flood is over. The bound has to be what answers: the stream
 * ends while the write deadline is still far off, the frontend never receives
 * more than the budget plus the kernel's own buffers, and the worker says which
 * limit it ran into instead of vanishing.
 *
 * The flood is sized from the TRANSPORT's frame budget and the ceiling is
 * stated in it, not from the queue's own constant. Measuring a bound against
 * the number it is bounded by is how this scenario's first mutation run — a
 * budget raised to 512 MiB — passed: the fixture dutifully asked for four times
 * the new budget, wrote as much as its own socket allowed, and compared the
 * result against a ceiling that had been raised with it.
 */
void testOwedBytesStayBounded() {
    Session session;
    if (!openReady(&session, "bounded", 32u * 1024u)) {
        check(false, "T2: a worker and a handshaked frontend");
        killWorker(&session);
        return;
    }
    const std::size_t frameBytes = measureStatusFrameBytes(&session);
    if (frameBytes == 0) {
        check(false, "T2: a measured status reply to size the flood from");
        killWorker(&session);
        return;
    }

    const int requests =
        static_cast<int>(4 * proto::kMaxMessageBytes / frameBytes) + 10;
    const auto start = std::chrono::steady_clock::now();
    int written = 0;
    while (written < requests) {
        if (!sendRequestOnly(&session, "output.status", {})) {
            break;
        }
        ++written;
    }

    // Reading starts only now: whatever arrives is what the worker let into the
    // kernel's buffers before it closed the door — never the ~4x asked for. The
    // wait is bounded, because a build with no byte bound never ends the stream
    // at all and would otherwise sit here being served its own four megabytes
    // until the harness killed it, which is a failure the fixture should report
    // rather than survive on someone else's timeout.
    long received = 0;
    while (!session.eof &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
                   .count() < 6000) {
        received += static_cast<long>(pump(&session, 200, 256u * 1024u));
    }
    const long elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    check(session.eof,
          "T2: a frontend that asks for four times the budget and reads nothing is ended");
    check(elapsed < 1900,
          "T2: the queue's bound ended it, not the write deadline — a bound that waits "
          "for a clock is not a bound");
    check(received > 4096,
          "T2: the frontend did receive some of what it asked for, so the ceiling below is "
          "not passing because nothing was sent at all");
    check(static_cast<std::size_t>(received) <= proto::kMaxMessageBytes + 512u * 1024u,
          "T2: and never more than one frame budget plus the sockets' own room");
    std::printf("  T2: %d of %d requests written, ~%zu KiB asked for; the frontend received "
                "%ld bytes and the stream ended after %ld ms\n",
                written, requests,
                static_cast<std::size_t>(requests) * frameBytes / 1024, received, elapsed);

    const std::string log = readFile(session.log);
    check(log.find("budget") != std::string::npos,
          "T2: the worker names the budget it ran out of rather than vanishing silently");
    check(waitExit(&session, 4000) >= 0, "T2: the worker exits cleanly after ending it");
    if (log.find("budget") == std::string::npos) {
        std::fprintf(stderr, "  T2 log tail: %s\n",
                     log.substr(log.size() > 600 ? log.size() - 600 : 0).c_str());
    }
    closeFrontend(&session);
    ::unlink(socketPath("bounded").c_str());
    ::unlink(session.log.c_str());
}

// --- T3: a hiccup costs nothing the frontend can see ------------------------

/*
 * Commands go out while the frontend is not reading, and then it reads again.
 * Two things have to be true afterwards, and they fail independently: every
 * command is answered (a queue that made room by dropping a reply, or a worker
 * that hung up the moment its socket filled, shows up here as a missing id),
 * and the preview notifications that piled up arrive REDUCED — the consumer of
 * preview.ready copies the current frame, so what it wants is the newest of each
 * slot and not a transcript, and a queue full of stale notifications is what
 * would have starved these replies.
 */
void testSlowStallKeepsRepliesServiceable() {
    Session session;
    if (!openReady(&session, "serviceable", 8u * 1024u)) {
        check(false, "T3: a worker and a handshaked frontend");
        killWorker(&session);
        return;
    }

    boost::json::value reply;
    const bool started =
        call(&session, session.nextId++, "preview.start",
             boost::json::object{{"width", 64}, {"height", 48}, {"fps", 60}}, &reply) &&
        reply.as_object().contains("result");
    check(started,
          "T3: the preview stream is started, so the render thread has notifications to send");
    const std::string stream =
        started ? std::string(reply.at("result").at("name").as_string()) : "";

    // Publishing is content-driven and slots are release-gated, so the fixture
    // both gives the worker something to report and gives its slots back.
    std::vector<boost::json::value> readySeen;
    for (int i = 0; i < 20 && readySeen.size() < 2; ++i) {
        boost::json::value answer;
        call(&session, session.nextId++, "layer.setEnabled",
             boost::json::object{{"layer", "clock"}, {"enabled", (i & 1) == 0}}, &answer);
        pump(&session, 80, 1u << 20);
        releaseAll(&session, &readySeen);
    }
    check(readySeen.size() >= 2,
          "T3: preview.ready notifications flow while the frontend reads");
    bool streamsMatch = !readySeen.empty();
    for (const auto& message : readySeen) {
        streamsMatch = streamsMatch && message.at("params").at("stream").as_string() == stream;
    }
    check(streamsMatch, "T3: and they name the live stream");

    boost::json::value before;
    if (!call(&session, session.nextId++, "output.status", {}, &before)) {
        check(false, "T3: the status before the stall");
        killWorker(&session);
        return;
    }
    const std::int64_t publishedBefore =
        numberAt(before.at("result").at("preview"), "published");
    const std::int64_t droppedBefore = numberAt(before.at("result").at("preview"), "dropped");

    // The stall: commands go out, nothing comes back, for well under the write
    // deadline — a frontend that hiccups rather than one that has gone. The
    // socket buffers are deliberately small, so its queue really does back up.
    std::vector<std::int64_t> during;
    for (int i = 0; i < 200; ++i) {
        const std::int64_t id = session.nextId;
        if (!sendRequestOnly(&session, "layer.setEnabled",
                             boost::json::object{{"layer", "clock"},
                                                 {"enabled", (i & 1) == 0}})) {
            break;
        }
        during.push_back(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    check(during.size() > 100, "T3: the commands were written while nothing was read");

    // And now it reads again: every one of those commands has to be answered.
    std::vector<bool> answered(during.size(), false);
    std::size_t duplicateReplies = 0;
    std::int64_t readyDuringDrain = 0;
    int quietRounds = 0;
    for (int round = 0; round < 120 && quietRounds < 3; ++round) {
        const std::size_t before = session.inbox.size();
        pump(&session, 60, 1u << 20);
        std::vector<boost::json::value> ready;
        readyDuringDrain += releaseAll(&session, &ready);
        for (auto entry = session.inbox.begin(); entry != session.inbox.end();) {
            bool matched = false;
            for (std::size_t i = 0; i < during.size(); ++i) {
                if (isReplyTo(*entry, during[i])) {
                    duplicateReplies += answered[i] ? 1u : 0u;
                    answered[i] = true;
                    matched = true;
                }
            }
            if (matched) {
                entry = session.inbox.erase(entry);
            } else {
                ++entry;
            }
        }
        quietRounds = session.inbox.size() == before && ready.empty() ? quietRounds + 1 : 0;
        if (session.eof) {
            break;
        }
    }
    std::size_t missing = 0;
    for (const bool one : answered) {
        if (!one) {
            ++missing;
        }
    }
    check(missing == 0,
          "T3: every command written during the stall was answered — a full queue is allowed "
          "to be full, never forgetful");
    check(duplicateReplies == 0,
          "T3: and none of them twice, which is what rewriting a frame that had already "
          "started going out would do to the stream behind it");
    check(!session.eof, "T3: and the connection survived a frontend that hiccupped");

    boost::json::value after;
    if (!call(&session, session.nextId++, "output.status", {}, &after)) {
        check(false, "T3: the status after the stall, on the same connection");
        killWorker(&session);
        return;
    }
    const std::int64_t publishedAfter = numberAt(after.at("result").at("preview"), "published");
    const std::int64_t droppedAfter = numberAt(after.at("result").at("preview"), "dropped");
    const std::int64_t owed =
        (publishedAfter - publishedBefore) + (droppedAfter - droppedBefore);
    check(owed > readyDuringDrain,
          "T3: the render thread owed more preview frames than the frontend received, so the "
          "count below is evidence of a reduced notification stream and not of an idle one");
    std::printf("  T3: %zu commands during the stall, %zu unanswered; %ld preview frames owed, "
                "%ld notifications delivered\n",
                during.size(), missing, owed, readyDuringDrain);

    killWorker(&session);
    ::unlink(socketPath("serviceable").c_str());
    ::unlink(session.log.c_str());
}

// --- T4: slow is not gone ---------------------------------------------------

/*
 * The deadline's other direction: a frontend that reads a trickle for longer
 * than the write deadline must still be connected at the end of it, with
 * nothing lost. A deadline started when serving began rather than when bytes
 * last moved passes T1 and should fail here — though "should" is doing work in
 * this sentence: this scenario's own first mutation run showed that at four
 * requests per round the queue is drained as fast as it fills, so the deadline
 * is never even armed and the mutant walked free. T5 is the scenario that owes
 * more than the socket can hold and so actually keeps the deadline in front of
 * the peer. Both are kept: T4 is a live connection under a slow reader, T5 the
 * same verdict with a full queue behind it.
 */
void testSlowReaderIsNeverDropped() {
    Session session;
    if (!openReady(&session, "slow", 16u * 1024u)) {
        check(false, "T4: a worker and a handshaked frontend");
        killWorker(&session);
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::int64_t> sent;
    int rounds = 0;
    long trickleBytes = 0;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
               .count() < 3500) {
        // Four status requests every ~300 ms, and only a trickle back: the
        // worker owes more than the socket can hold for the whole run, and
        // makes a little progress every 150 ms.
        for (int i = 0; i < 4; ++i) {
            const std::int64_t id = session.nextId;
            if (!sendRequestOnly(&session, "output.status", {})) {
                break;
            }
            sent.push_back(id);
        }
        ++rounds;
        trickleBytes += static_cast<long>(pump(&session, 150, 1024));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    check(trickleBytes > 8192,
          "T4: the trickle did take bytes throughout, so the connection being alive below is "
          "a worker that kept up with a slow peer and not one that never got behind");
    check(!session.eof,
          "T4: a frontend that reads a trickle for longer than the write deadline is still "
          "connected: the deadline measures progress, not a busy queue");

    std::vector<bool> answered(sent.size(), false);
    std::size_t duplicateReplies = 0;
    int quietRounds = 0;
    for (int round = 0; round < 200 && quietRounds < 3; ++round) {
        const std::size_t before = session.inbox.size();
        pump(&session, 60, 1u << 20);
        for (auto entry = session.inbox.begin(); entry != session.inbox.end();) {
            bool matched = false;
            for (std::size_t i = 0; i < sent.size(); ++i) {
                if (isReplyTo(*entry, sent[i])) {
                    duplicateReplies += answered[i] ? 1u : 0u;
                    answered[i] = true;
                    matched = true;
                }
            }
            if (matched) {
                entry = session.inbox.erase(entry);
            } else {
                ++entry;
            }
        }
        quietRounds = session.inbox.size() == before ? quietRounds + 1 : 0;
        if (session.eof) {
            break;
        }
    }
    std::size_t missing = 0;
    for (const bool one : answered) {
        if (!one) {
            ++missing;
        }
    }
    check(missing == 0, "T4: every status request of the slow run was answered");
    check(duplicateReplies == 0,
          "T4: and each of them exactly once — a queue that rewrote a frame mid-write "
          "duplicates or drops what follows it");
    std::printf("  T4: %d rounds of 4 requests over 3.5 s of trickle reading, %zu unanswered\n",
                rounds, missing);

    boost::json::value ping;
    check(call(&session, session.nextId++, "ping", {}, &ping),
          "T4: a new command on that long-suffering connection still works");
    killWorker(&session);
    ::unlink(socketPath("slow").c_str());
    ::unlink(session.log.c_str());
}

// --- T5: a debt that is being paid is not a reason to hang up ---------------

/*
 * T4 tricks one thing and this tricks the other. Reading T4 at its own pace
 * keeps the WORKER'S QUEUE nearly empty — four requests every 300 ms is less
 * than the socket pair itself holds — so a deadline started when the loop began
 * rather than when bytes last moved would have two seconds to fire in while
 * nothing was owed, and it slips through. Here the frontend owes itself a debt
 * first: several hundred status replies queued up while the peer reads nothing,
 * far more than the pair's buffers, and then it reads a trickle so slow that
 * the backlog is still there, measurably shrinking, long after the deadline has
 * expired. This is the case the deadline exists to distinguish and the one a
 * clock measured from the wrong instant cannot distinguish: the queue is full
 * AND bytes are moving, and the only correct answer is to keep serving it.
 */
void testDeepBacklogBeingDrainedIsNotDropped() {
    Session session;
    if (!openReady(&session, "debt", 16u * 1024u)) {
        check(false, "T5: a worker and a handshaked frontend");
        killWorker(&session);
        return;
    }
    const std::size_t frameBytes = measureStatusFrameBytes(&session);
    if (frameBytes == 0) {
        check(false, "T5: a measured status reply to size the debt from");
        killWorker(&session);
        return;
    }

    // Deep enough that the pair's own buffers cannot hide it, and still under
    // the queue's budget so nothing here is about to be refused: the whole point
    // is a worker that owes a lot and is behind, not one that has run out.
    constexpr int kFlood = 400;
    std::vector<std::int64_t> sent;
    int flooded = 0;
    for (int i = 0; i < kFlood; ++i) {
        const std::int64_t id = session.nextId;
        if (!sendRequestOnly(&session, "output.status", {})) {
            break;
        }
        sent.push_back(id);
        ++flooded;
    }
    const long owed = static_cast<long>(flooded) * static_cast<long>(frameBytes);
    check(flooded == kFlood, "T5: the whole debt went in, unanswered");

    // One KiB every 50 ms: 20 KiB a second against a debt of ~200 KiB. Three
    // seconds of that is past the write deadline with the backlog still two
    // thirds in front of the peer, which is the moment the deadline is asked
    // about and gets the answer "still connected".
    const auto start = std::chrono::steady_clock::now();
    long taken = 0;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
               .count() < 3000 && !session.eof) {
        taken += static_cast<long>(pump(&session, 20, 1024));
        // The sleep is the pace. Without it the loop reads a KiB and comes back
        // to find another waiting, and "a trickle" turns into draining the
        // whole debt at full speed, which is a different scenario — a fine one,
        // and not this one.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const long windowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    check(taken > 16 * 1024,
          "T5: bytes really were moving throughout, so what survived below is a worker "
          "paying a debt and not one that was simply idle");
    check(taken < owed / 2,
          "T5: and most of the debt was still owed when the window closed, so the queue was "
          "non-empty across the whole deadline rather than clear for the last minute of it");
    check(windowMs > 2000 && !session.eof,
          "T5: a frontend that is slower than its debt, past the write deadline, is still "
          "connected — the deadline measures whether bytes move, not how much is owed");

    // And the debt is paid in full: nothing was thrown away on the way through
    // the stress, and no reply came twice because the queue shuffled the bytes.
    std::vector<bool> answered(sent.size(), false);
    std::size_t duplicateReplies = 0;
    int quietRounds = 0;
    for (int round = 0; round < 400 && quietRounds < 3; ++round) {
        const std::size_t before = session.inbox.size();
        pump(&session, 60, 1u << 20);
        for (auto entry = session.inbox.begin(); entry != session.inbox.end();) {
            bool matched = false;
            for (std::size_t i = 0; i < sent.size(); ++i) {
                if (isReplyTo(*entry, sent[i])) {
                    duplicateReplies += answered[i] ? 1u : 0u;
                    answered[i] = true;
                    matched = true;
                }
            }
            if (matched) {
                entry = session.inbox.erase(entry);
            } else {
                ++entry;
            }
        }
        quietRounds = session.inbox.size() == before ? quietRounds + 1 : 0;
        if (session.eof) {
            break;
        }
    }
    std::size_t missing = 0;
    for (const bool one : answered) {
        if (!one) {
            ++missing;
        }
    }
    check(!session.eof,
          "T5: the connection outlived the backlog it was carrying");
    check(missing == 0, "T5: every request of the debt was answered");
    check(duplicateReplies == 0,
          "T5: and each exactly once, in order, across a queue that was full the whole time");

    const std::string log = readFile(session.log);
    check(log.find("has not taken a byte") == std::string::npos &&
              log.find("budget") == std::string::npos,
          "T5: the worker never once mistook this frontend for a gone one");
    std::printf("  T5: %d replies owed (~%ld KiB), %ld KiB taken over %ld ms of trickle, "
                "%zu unanswered\n",
                flooded, owed / 1024, taken / 1024, windowMs, missing);
    if (session.eof || missing != 0) {
        std::fprintf(stderr, "  T5 log tail: %s\n",
                     log.substr(log.size() > 600 ? log.size() - 600 : 0).c_str());
    }

    killWorker(&session);
    ::unlink(socketPath("debt").c_str());
    ::unlink(session.log.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const char* only = argc > 2 ? argv[2] : nullptr;
    const bool all = only == nullptr;
    if (argc > 1) {
        gWorkerBinary = argv[1];
    }
    if (all || std::strcmp(only, "T1") == 0) {
        testStalledPeerIsDroppedInBoundedTime();
    }
    if (all || std::strcmp(only, "T2") == 0) {
        testOwedBytesStayBounded();
    }
    if (all || std::strcmp(only, "T3") == 0) {
        testSlowStallKeepsRepliesServiceable();
    }
    if (all || std::strcmp(only, "T4") == 0) {
        testSlowReaderIsNeverDropped();
    }
    if (all || std::strcmp(only, "T5") == 0) {
        testDeepBacklogBeingDrainedIsNotDropped();
    }

    if (gFailures != 0) {
        std::fprintf(stderr, "%d RPC transport check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("ok: a stalled frontend is bounded, dropped in time, and never dropped for "
                "merely being slow\n");
    return 0;
}
