/*
 * RpcClient implementation. Notes:
 *
 * - QLocalSocket with a path containing '/' connects to exactly the file the
 *   worker bound, using the same framing both sides share.
 * - The bounds below are the client's half of task 6.8b: they are checked
 *   before an id is consumed, so a refused request leaves no ledger entry that
 *   could never be answered and never touches the connection.
 * - Nothing here reads a result. The only judgement made about an incoming
 *   message is which request its id names, and whether that request's
 *   connection is still the live one.
 */

#include "RpcClient.h"

#include <QCoreApplication>
#include <QDir>

#include <string_view>

namespace openshow::ui {

namespace proto = openshow::protocol;

namespace {

constexpr int kConnectRetryMs = 100;

// How many requests may be waiting on the worker at once (task 6.8b, review
// R11). A reply is owed for each of them, so this is a bound on the queue the
// UI builds on the worker's behalf — and before it there was none: the status
// poll fired every 500 ms into QLocalSocket whatever the worker was doing, so
// a worker that stopped answering was followed by an ever-longer list of
// requests that could never be answered. Sixty-four is far beyond anything the
// UI issues at once (a go is a prepare plus at most a present), and past it the
// honest answer is "the worker has not replied to what you already sent", not
// "here is one more".
constexpr int kMaxInFlightRequests = 64;

// How much unwritten request traffic the socket may hold. Generous by design,
// because it only binds when the WORKER has stopped draining: 4 MiB is ten of
// the largest scenes this build's own budgets allow (the every-budget slide
// measured in task 6.8a is 408 KiB inside its frame) or thousands of ordinary
// commands. Beyond it a request is refused with the reason rather than added to
// a backlog nobody is reading, and if the backlog is still there a deadline
// later the connection is closed — a worker that has taken no byte in that long
// is not coming back, and the operator needs to know rather than keep clicking.
constexpr qint64 kMaxQueuedWriteBytes = 4 * 1024 * 1024;
// How long the worker may take NO byte at all before the connection is ended,
// and how often that clock is read while the watch is armed. The window runs
// from the last byte this side managed to hand over, exactly as the worker's own
// write deadline does, because the two things that look like a verdict are not:
// the queue's depth says nothing (a queue that is refusing requests is by
// construction already at its bound), and bytesToWrite() moves by ~100 KiB on
// accounting alone with a peer that has read nothing — both measured on this
// task's fixtures. Whether bytes are actually leaving is the only question that
// separates a worker that is slow from one that is gone, and slow must never
// cost a worker its session.
constexpr int kWriteStallMs = 2000;
constexpr int kWriteStallTickMs = 250;  // only ticks while a refusal has armed it

}  // namespace

RpcClient::RpcClient(QObject* parent) : QObject(parent) {
    // The socket lives under XDG_RUNTIME_DIR with the UI's pid in its name, so
    // two UIs never collide and the worker's bind always has a fresh path (the
    // worker also unlinks a stale one).
    fSocketPath = QDir(qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath()))
                      .filePath(QStringLiteral("openshow-ui-%1.sock")
                                    .arg(QCoreApplication::applicationPid()));

    connect(&fSocket, &QLocalSocket::connected, this, &RpcClient::onSocketConnected);
    connect(&fSocket, &QLocalSocket::disconnected, this, &RpcClient::onSocketDisconnected);
    connect(&fSocket, &QLocalSocket::readyRead, this, &RpcClient::onSocketReady);
    // The worker binds its socket a moment after we spawn it, so the first
    // connect attempts fail; retry on a short timer until it answers, the
    // supervisor says stop, or patience runs out.
    fConnectRetry.setInterval(kConnectRetryMs);
    connect(&fConnectRetry, &QTimer::timeout, this, &RpcClient::tryConnect);
    // Armed by a refusal to queue more (watchWriteBacklog) and then read on a
    // short tick: what ends the connection is the worker taking no byte for
    // kWriteStallMs, not how deep the queue happens to be (task 6.8b, review R11).
    fWriteStallDeadline.setSingleShot(false);
    fWriteStallDeadline.setInterval(kWriteStallTickMs);
    connect(&fWriteStallDeadline, &QTimer::timeout, this, &RpcClient::onWriteStallExpired);
    // The one fact that clock needs: when the worker last took a byte. Qt emits
    // this as it hands its write buffer to the kernel, which on a stream socket
    // only proceeds as far as the peer's window allows — so when it stops firing,
    // the peer has stopped reading. The 0-length emission at the end of a flush is
    // not progress and does not count.
    connect(&fSocket, &QLocalSocket::bytesWritten, this, [this](qint64 bytes) {
        if (bytes > 0) {
            fLastByteOutAt = std::chrono::steady_clock::now();
        }
    });
}

void RpcClient::setStopCondition(std::function<bool()> stop) {
    fStopCondition = std::move(stop);
}

void RpcClient::startConnecting() {
    fConnectAttempts = 0;
    fConnectRetry.start();
    tryConnect();
}

void RpcClient::tryConnect() {
    const QLocalSocket::LocalSocketState state = fSocket.state();
    if (state == QLocalSocket::ConnectedState || state == QLocalSocket::ConnectingState) {
        return;
    }
    if (fStopCondition && fStopCondition()) {
        fConnectRetry.stop();
        return;
    }
    if (++fConnectAttempts > 100) {  // ten seconds of patience
        fConnectRetry.stop();
        emit statusMessage(QStringLiteral("could not reach the worker on %1").arg(fSocketPath));
        return;
    }
    fSocket.connectToServer(fSocketPath);
}

void RpcClient::onSocketConnected() {
    fConnectRetry.stop();
    fConnected = true;
    // A fresh connection owns a fresh conversation: no request of the old
    // one may be answered, and no half-read frame of the old stream may
    // join the new one (task 6.3a).
    ++fConnectionGeneration;
    fDecoder.reset();
    fPendingRequests.clear();
    fWriteStallDeadline.stop();  // a new connection has a new backlog to mean it with
    fWriteStalled = false;
    fWriteStalledBytes = 0;
    fLastByteOutAt = std::chrono::steady_clock::now();
    emit socketConnected();
}

void RpcClient::onSocketDisconnected() {
    fConnected = false;
    fWriteStallDeadline.stop();
    emit socketDisconnected();
}

void RpcClient::onSocketReady() {
    const QByteArray incoming = fSocket.readAll();
    fDecoder.feed(std::string_view(incoming.constData(),
                                   static_cast<std::size_t>(incoming.size())));
    drainMessages();
}

void RpcClient::drainMessages() {
    boost::json::value message;
    std::string error;
    while (fDecoder.next(&message, &error)) {
        if (!message.is_object()) {
            continue;
        }
        if (message.as_object().contains("method")) {  // worker-originated
            emit notificationReceived(message.as_object());
            continue;
        }
        const boost::json::object& reply = message.as_object();
        // Reply correlation (task 6.3a, review R3): a reply is only what the
        // request its id names said it would be. Shape-matching replies let a
        // burst of prepares misattribute payloads and fire stale auto-presents.
        const auto idEntry = reply.find("id");
        if (idEntry == reply.end() ||
            !(idEntry->value().is_int64() || idEntry->value().is_uint64())) {
            continue;  // a reply with no usable id answers nothing
        }
        const std::int64_t id = idEntry->value().to_number<std::int64_t>();
        const auto pendingEntry = fPendingRequests.find(id);
        if (pendingEntry == fPendingRequests.end()) {
            continue;  // duplicate, superseded, or from a previous connection
        }
        // Consumed here, read by the owner: the answer has now cost its place in
        // the ledger, so an owner that keeps the intent acts on the newest
        // request it made and nothing else.
        const RpcRequest request = pendingEntry.value();
        fPendingRequests.erase(pendingEntry);
        if (request.connection != fConnectionGeneration) {
            continue;  // written on a connection that is gone
        }
        emit replyReceived(request, reply);
    }
    if (fDecoder.failed()) {
        emit statusMessage(QStringLiteral("worker broke the protocol: %1")
                               .arg(QString::fromStdString(error)));
        fSocket.disconnectFromServer();
    }
}

std::int64_t RpcClient::sendRequest(const char* method, boost::json::object params,
                                    std::shared_ptr<void> intent) {
    // The frame is built BEFORE the request exists (task 6.8a, review R11).
    // kMaxMessageBytes is the budget of BOTH directions, and the review's probe
    // measured what an unchecked sender costs: it wrote the length of a
    // 1,048,650-byte payload, the worker's decoder refused the frame and hung
    // up, and the operator's paste of large text took the whole output down
    // with it. Refusing here instead writes no byte, consumes no id (so no
    // pending entry exists that could never be answered) and leaves the
    // connection up; the status line is the operator's answer.
    const std::int64_t id = fNextId;
    std::string wire;
    if (!proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire)) {
        emit statusMessage(QStringLiteral("%1 was not sent: it is over the %2 KiB this "
                                          "transport allows, so the worker would refuse it")
                               .arg(QString::fromLatin1(method))
                               .arg(static_cast<qulonglong>(proto::kMaxMessageBytes / 1024u)));
        return 0;
    }
    // The two bounds of task 6.8b, checked before the id is consumed for the
    // same reason as above: a request that is not going to be answered must not
    // exist. Both refuse in the client with the reason, and neither touches the
    // connection — the worker gets the chance to come back on its own.
    if (fSocket.bytesToWrite() + static_cast<qint64>(wire.size()) > kMaxQueuedWriteBytes) {
        emit statusMessage(QStringLiteral("%1 was not sent: %2 KiB of requests are already "
                                          "waiting for the worker, which is not reading them")
                               .arg(QString::fromLatin1(method))
                               .arg(static_cast<qulonglong>(fSocket.bytesToWrite() / 1024)));
        watchWriteBacklog();
        return 0;
    }
    if (fPendingRequests.size() >= static_cast<size_t>(kMaxInFlightRequests)) {
        emit statusMessage(QStringLiteral("%1 was not sent: %2 requests are already waiting "
                                          "for the worker to reply")
                               .arg(QString::fromLatin1(method))
                               .arg(kMaxInFlightRequests));
        return 0;
    }
    ++fNextId;
    // Record before writing: the reply to this id must find its request,
    // tagged with the connection it belongs to (task 6.3a).
    RpcRequest request;
    request.id = id;
    request.method = method;
    request.connection = fConnectionGeneration;
    request.intent = std::move(intent);
    fPendingRequests.insert(id, std::move(request));
    fSocket.write(wire.data(), static_cast<qint64>(wire.size()));
    return id;
}

void RpcClient::sendNotification(const char* method, boost::json::object params) {
    std::string wire;
    if (!proto::appendFrame(proto::makeNotification(method, std::move(params)), &wire)) {
        // A notification has no reply to carry a reason, and a partially built
        // frame would poison the stream for the messages behind it: drop it and
        // keep the connection. Every notification this client sends names a
        // slot and a sequence, so arriving here means a bug rather than input.
        emit statusMessage(QStringLiteral("%1 could not be sent: it does not fit this transport")
                               .arg(QString::fromLatin1(method)));
        return;
    }
    // Same backlog bound as a request: a notification is droppable by contract
    // (the preview consumer works off the newest frame it is given, and the
    // worker re-reports state), so it is the first thing to lose when the worker
    // has stopped reading.
    if (fSocket.bytesToWrite() + static_cast<qint64>(wire.size()) > kMaxQueuedWriteBytes) {
        emit statusMessage(QStringLiteral("%1 was dropped: the worker is not reading, and %2 "
                                          "KiB of traffic is already queued for it")
                               .arg(QString::fromLatin1(method))
                               .arg(static_cast<qulonglong>(fSocket.bytesToWrite() / 1024)));
        watchWriteBacklog();
        return;
    }
    fSocket.write(wire.data(), static_cast<qint64>(wire.size()));
}

void RpcClient::watchWriteBacklog() {
    // Called by the two paths that had to refuse a message for want of backlog
    // room. The refusal IS the signal: by the time a request is accepted the
    // queue is under the budget by construction, so testing the depth here armed
    // nothing at exactly the depth that was refusing — an earlier version did,
    // and its deadline never fired.
    if (!fWriteStallDeadline.isActive()) {
        fWriteStallDeadline.start();
    }
}

void RpcClient::onWriteStallExpired() {
    if (!fConnected || (fStopCondition && fStopCondition())) {
        fWriteStallDeadline.stop();
        return;
    }
    const qint64 queued = fSocket.bytesToWrite();
    if (queued == 0) {
        fWriteStallDeadline.stop();  // the worker caught up: nothing is owed
        return;
    }
    const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - fLastByteOutAt)
                            .count();
    if (idleMs < kWriteStallMs) {
        // Bytes are moving, so this worker is behind rather than gone, and a
        // worker that is merely slow must never lose its session. This is the
        // same measurement the worker's own write deadline makes from its side.
        return;
    }
    // Not one byte has reached the worker in the whole window while this side
    // still owes what it has queued. Keeping the connection would only mean more
    // of the same, so: close, and let the disconnect path say WHY, with the depth
    // captured here because abort() discards the buffer the number came from
    // (task 6.3a's reset of the intents that can no longer be confirmed follows
    // on disconnected()).
    fWriteStallDeadline.stop();
    fWriteStalled = true;
    fWriteStalledBytes = queued;
    fSocket.abort();  // the backlog cannot be delivered, and must not be replayed
}

QString RpcClient::writeStallMessage() const {
    return QStringLiteral("the worker has not read a byte for %1 s with %2 KiB of requests "
                          "queued: the connection is closed")
        .arg(kWriteStallMs / 1000)
        .arg(static_cast<qulonglong>(fWriteStalledBytes / 1024));
}

RpcRequest* RpcClient::pending(std::int64_t id) {
    const auto entry = fPendingRequests.find(id);
    return entry == fPendingRequests.end() ? nullptr : &entry.value();
}

void RpcClient::eachPending(const std::function<void(RpcRequest&)>& visit) {
    for (auto entry = fPendingRequests.begin(); entry != fPendingRequests.end(); ++entry) {
        visit(entry.value());
    }
}

void RpcClient::clearPending() {
    fPendingRequests.clear();
}

void RpcClient::flush() {
    fSocket.flush();
}

}  // namespace openshow::ui
