/*
 * RpcMethods: the worker's half of the wire's MEANING — the method table and
 * the encoders that put session state on the socket (task 6.19a4).
 *
 * RpcServer owns the connection: accept, framed read, the handshake gate, the
 * bounded outbound queue, the write deadline, the drain and the shutdown. This
 * class owns what a decoded request DOES: it decodes each method's arguments
 * (every number read in its own width, every range checked before any
 * narrowing — the R11 rule that an argument nobody sent must not move the
 * audience), calls the session operation the method names, and encodes the
 * answer or the refusal. The status/notification encoders are here for the
 * same reason: the `output.status` result, the display list and the
 * `preview.ready`/`media.error` payloads are one description of the session in
 * one place, so what the frontend decodes has exactly one worker-side author.
 *
 * The names and shapes themselves stay where they always were — the shared
 * typed definitions in openshow::protocol — so the frontend and the server
 * cannot drift: this file adds no new wire vocabulary, it only routes the
 * existing one.
 *
 * It holds no socket and no thread. The one thing it cannot encode inline is
 * a deferred answer: `scene.prepare` replies when the prepare executor
 * finishes, from that executor's thread. Whatever owns the connection injects
 * that sink; it must be safe to call from any thread and after dispatch has
 * long returned, and it is exactly what a late completion is required of —
 * the serve loop's ids correlate, ordering was never promised (task 6.3a).
 */

#pragma once

#include <boost/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "openshow/protocol/protocol.h"

#include "session/OutputSession.h"

namespace openshow::worker {

// --- the status and notification encoders -----------------------------------
// Pure functions over session snapshots: the caller takes the session's lock,
// copies a Status (or pops a notification), and encodes OUTSIDE it, so no
// session mutex is ever held near the socket (R11).

// The `output.status` result: the same object the frontend's status parser
// reads field by field, including the preview block's three targets and the
// two picture caches whose separate budgets are the evidence on the wire.
boost::json::value statusResult(const OutputSession::Status& status);

// The `output.displays` answer: a bare array, index by index.
boost::json::value displaysResult(const std::vector<OutputSession::DisplayInfo>& displays);

// The `preview.ready` params: whose picture this is and which ask it answers
// (task 6.16a), and the stream identity the release must echo (task 6.5).
boost::json::object previewReadyParams(const OutputSession::PreviewReady& ready);

// The `media.error` params (task 5.5d): the same text output.status.media
// carries until a successful open replaces it.
boost::json::object mediaErrorParams(const OutputSession::MediaError& error);

// --- the method table ---------------------------------------------------------

class MethodTable {
public:
    // Hands a finished, already-shaped reply (a `scene.prepare` ack or error)
    // to the connection that made the request. Called from the prepare
    // executor thread; the connection's loop delivers it.
    using DeferredSink = std::function<void(boost::json::value frame)>;

    MethodTable(OutputSession& session, DeferredSink defer);

    // Answers one envelope whose handshake gate the caller has already passed:
    // appends the result or error frame to *outbound (possibly none, when the
    // answer is deferred), and performs the method's session call. An unknown
    // method is answered, not dropped; nothing here can end the connection —
    // that decision belongs to the loop alone.
    void dispatch(const openshow::protocol::RequestEnvelope& envelope, std::string* outbound);

private:
    OutputSession& fSession;
    DeferredSink fDefer;
};

}  // namespace openshow::worker
