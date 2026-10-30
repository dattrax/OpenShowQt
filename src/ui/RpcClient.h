/*
 * RpcClient: the transport half of the frontend's worker conversation.
 *
 * Everything this class decides can be decided without knowing what any
 * message means: retrying the connect against the socket the worker binds, the
 * openshow::protocol framing in both directions, the ledger of requests the
 * worker owes replies to, the bounds a message has to pass before it goes on
 * the wire, and the deadline that ends a connection whose peer has stopped
 * reading (task 6.8b, review R11).
 *
 * What it deliberately does NOT do is interpret an answer. A reply travels back
 * to whoever sent its request carrying the method name that request was sent
 * under, the connection generation it was written on, and whatever intent that
 * owner attached to it (task 6.3a, review R3). Deciding what a result MEANS is
 * the session's job, which is why adding a method to the protocol never
 * requires a change here — and why the rules that protect the session from a
 * reordered or stale reply live in exactly one place.
 */

#pragma once

#include <boost/json.hpp>

#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "openshow/protocol/protocol.h"

namespace openshow::ui {

// One request the worker owes an answer to, as the transport knows it.
//
// `method` and `connection` are the whole of the R3 rule from this side: a
// reply belongs to the request its id names and to the connection that request
// was written on, never to "whatever result shape arrived". `intent` is the
// caller's own record of what it meant; this class stores it and hands it back
// untouched, so the two are always the same object and a decoration made after
// a send cannot be missed by the reply that follows.
struct RpcRequest {
    std::int64_t id = 0;
    std::string method;
    std::uint64_t connection = 0;
    std::shared_ptr<void> intent;

    // The caller's record, back as the type it sent (empty when the request
    // carried no intent).
    template <typename T>
    std::shared_ptr<T> intentAs() const {
        return std::static_pointer_cast<T>(intent);
    }
};

class RpcClient : public QObject {
    Q_OBJECT
public:
    explicit RpcClient(QObject* parent = nullptr);

    // The socket this client connects to. The path is the UI's own, built with
    // its pid so two UIs never collide and bind always has a fresh name; the
    // supervisor removes the file when the session is over.
    const QString& socketPath() const { return fSocketPath; }

    bool connected() const { return fConnected; }
    // Bumped on every successful connect. A request records the generation it
    // was written on, and a reply is only passed on when that generation is the
    // current one — so bytes of a dead stream can never answer a new session.
    std::uint64_t connectionGeneration() const { return fConnectionGeneration; }

    // Why to stop reaching for the worker. The supervisor owns that answer (a
    // shutdown this side asked for, or a worker process that is gone); unset
    // means "keep trying". Read on every retry tick and by the stall watch.
    void setStopCondition(std::function<bool()> stop);

    // Begins (or restarts) the connection attempt: the worker binds its socket
    // a moment after we spawn it, so the first attempts fail and the retry timer
    // carries on until it answers, the stop condition says enough, or patience
    // runs out.
    void startConnecting();

    // Sends a request and returns its id, or 0 when it was refused HERE. Every
    // refusal names its reason through statusMessage() and leaves the
    // connection up: a request that will never be answered must not exist, and
    // the worker gets the chance to come back on its own. `intent` is carried
    // to the reply unchanged (see RpcRequest).
    std::int64_t sendRequest(const char* method, boost::json::object params = {},
                             std::shared_ptr<void> intent = nullptr);
    // Sends a message that owes no answer. Droppable by contract, so it is the
    // first thing to lose when the worker has stopped reading.
    void sendNotification(const char* method, boost::json::object params = {});

    // The ledger. `pending` returns the live record for an id, for an owner that
    // only decided what a request meant after sending it; `eachPending` visits
    // every live record, and `clearPending` drops all of them — which is what a
    // connection that is gone owes: nothing written on it can be confirmed any
    // more. Most senders need neither: they pass their intent to sendRequest and
    // fill that object in once the id comes back.
    RpcRequest* pending(std::int64_t id);
    void eachPending(const std::function<void(RpcRequest&)>& visit);
    void clearPending();

    // Hand the kernel whatever is queued, for a caller that is about to wait on
    // the peer having received the last message (a shutdown request).
    void flush();

    // True when this side ended the connection because the worker had taken not
    // one byte for the whole stall window. The reason has to outlive the socket
    // because both the disconnect path and the process-exit path report it, and
    // whichever lands last is the one the operator reads.
    bool endedByWriteStall() const { return fWriteStalled; }
    QString writeStallMessage() const;

signals:
    // The socket is up and the conversation is new: the decoder is reset, the
    // ledger is empty and the stall watch is disarmed, so nothing of the old
    // connection can be answered or replayed.
    void socketConnected();
    void socketDisconnected();
    // A reply for a live request of THIS connection. Its ledger entry has
    // already been consumed, so the caller owns the intent from here.
    void replyReceived(const RpcRequest& request, const boost::json::object& message);
    // A message that names a method: the worker originating, not answering.
    void notificationReceived(const boost::json::object& message);
    // The operator-facing line: a message this side refused to send, or a peer
    // that broke the framing.
    void statusMessage(const QString& text);

private slots:
    void tryConnect();
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketReady();
    void onWriteStallExpired();

private:
    void drainMessages();
    // Arms the stalled-write deadline when a message had to be refused for want
    // of backlog room (task 6.8b, review R11). When the window closes, a
    // backlog that has shrunk at all means the worker is slow and stays
    // connected; one that has not lost a single byte means it is gone, and the
    // connection is closed rather than buffered indefinitely.
    void watchWriteBacklog();

    QLocalSocket fSocket;
    QString fSocketPath;
    openshow::protocol::Decoder fDecoder;
    QTimer fConnectRetry;
    int fConnectAttempts = 0;
    std::function<bool()> fStopCondition;

    std::int64_t fNextId = 1;
    std::uint64_t fConnectionGeneration = 0;
    QHash<std::int64_t, RpcRequest> fPendingRequests;

    // Armed by a refusal to queue more and then read on a short tick: what ends
    // the connection is the worker taking no byte for kWriteStallMs, not the
    // depth of the queue. fWriteStalled carries the reason into the disconnect
    // message, and fWriteStalledBytes is the backlog as it stood when the
    // deadline fired — abort() discards the buffer the number came from.
    QTimer fWriteStallDeadline;
    bool fWriteStalled = false;
    qint64 fWriteStalledBytes = 0;
    // When the worker last took a byte, as Qt reports it flushing the backlog.
    // The deadline is measured from this and not from the depth of the queue: a
    // worker that is slow keeps its session, one that has taken nothing for the
    // whole window does not (task 6.8b, review R11).
    std::chrono::steady_clock::time_point fLastByteOutAt{};

    bool fConnected = false;
};

}  // namespace openshow::ui
