/*
 * RpcMethods implementation (task 6.19a4). Every handler is the one that
 * already answered on the wire, ported unchanged: the same argument widths,
 * the same refusals with the same texts, the same result shapes, the same
 * session calls in the same order. What moved here is the meaning of a
 * request; the connection, the queue and the deadline stayed with RpcServer.
 */

#include "rpc/RpcMethods.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "openshow/protocol/preview_wire.h"
#include "openshow/protocol/protocol.h"
#include "openshow/scene/scene.h"

#include "render/Layer.h"

namespace openshow::worker {

namespace {

namespace proto = openshow::protocol;

// An integer argument in a stated range, checked BEFORE any narrowing
// (task 6.8a, review R11). Casting first and validating after is how
// 4294967296 becomes 0: a caller that meant "no such monitor" gets monitor
// zero, which is a real destination, and the audience's output moves on an
// argument nobody sent. Absent, non-integer, or out of range all answer
// false, and the handler says which.
bool jsonIntInRange(const boost::json::object& args, const char* key, std::int64_t low,
                    std::int64_t high, std::int64_t* out) {
    const auto entry = args.find(key);
    if (entry == args.end()) {
        return false;
    }
    std::int64_t value = 0;
    if (entry->value().is_int64()) {
        value = entry->value().as_int64();
    } else if (entry->value().is_uint64() &&
               entry->value().as_uint64() <=
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        value = static_cast<std::int64_t>(entry->value().as_uint64());
    } else {
        return false;
    }
    if (value < low || value > high) {
        return false;
    }
    *out = value;
    return true;
}

// A 32-bit argument — the same rule as above, spelled for the values the
// session's own range checks are written in.
bool jsonUint32(const boost::json::object& args, const char* key, std::uint32_t* out) {
    std::int64_t value = 0;
    if (!jsonIntInRange(args, key, 0, std::numeric_limits<std::uint32_t>::max(), &value)) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

// Reads a non-negative JSON integer at full width. Boost.JSON stores small
// numbers as int64 and only reaches for uint64 past the signed range, so
// checking is_uint64() alone silently rejects every slot index a client
// sends. Nothing narrows the result, so no range is imposed here.
bool jsonUint64(const boost::json::object& args, const char* key, std::uint64_t* out) {
    const auto entry = args.find(key);
    if (entry == args.end()) {
        return false;
    }
    if (entry->value().is_uint64()) {
        *out = entry->value().as_uint64();
        return true;
    }
    if (entry->value().is_int64() && entry->value().as_int64() >= 0) {
        *out = static_cast<std::uint64_t>(entry->value().as_int64());
        return true;
    }
    return false;
}

// A ceiling on a seek target, in seconds (task 6.15a): a bound on
// nonsense, not a promise about durations — no service runs 24 hours of
// background video, and a target past any real clip ends the same way a
// target at its length does.
constexpr double kMaxSeekSeconds = 86400.0;

// Reads a JSON number as a double within [low, high] (task 6.15a): seek
// targets and gain are fractional, and every number kind Boost.JSON
// stores — int64, uint64, double — is a legitimate spelling of "1" or
// "0.5". NaN and both infinities are refused by the range test after a
// finite-only accept, because comparisons lie about NaN: an out-of-range
// check written alone would wave it through.
bool jsonDoubleInRange(const boost::json::object& args, const char* key, double low,
                       double high, double* out) {
    const auto entry = args.find(key);
    if (entry == args.end()) {
        return false;
    }
    double value = 0.0;
    if (entry->value().is_double()) {
        value = entry->value().as_double();
    } else if (entry->value().is_int64()) {
        value = static_cast<double>(entry->value().as_int64());
    } else if (entry->value().is_uint64()) {
        value = static_cast<double>(entry->value().as_uint64());
    } else {
        return false;
    }
    if (!(value >= low) || !(value <= high)) {  // false for NaN on either leg
        return false;
    }
    *out = value;
    return true;
}

boost::json::value layersObject(uint32_t mask) {
    boost::json::object layers;
    for (uint32_t i = 0; i < static_cast<uint32_t>(LayerId::kCount); ++i) {
        const LayerId id = static_cast<LayerId>(i);
        layers[std::string(layerName(id))] = (mask & layerBit(id)) != 0;
    }
    return layers;
}

// One entry per registered audio source (task 6.15a, review R15). An
// empty list honestly means "no audio identity is live", which is also
// the state of a clip with no decodable stream.
boost::json::array sourcesArray(const OutputSession::MediaStatus& media) {
    boost::json::array sources;
    for (const OutputSession::MediaStatus::Source& source : media.sources) {
        sources.push_back(boost::json::object{{"name", source.name},
                                              {"gain", source.gain},
                                              {"muted", source.muted}});
    }
    return sources;
}

boost::json::object standaloneStatus(const OutputSession::MediaStatus::Standalone& standalone) {
    // The track's own block (task 6.15b): its lifetime is not the
    // background's, so its report is not the background's either. The
    // 3.4 rule as usual — media.standalone answers what the selection
    // became.
    return boost::json::object{{"live", standalone.live},
                               {"path", standalone.path},
                               {"playing", standalone.playing},
                               {"position", standalone.position},
                               {"duration", standalone.duration},
                               {"error", standalone.error},
                               {"audio", standalone.audio},
                               {"loops", standalone.loops}};
}

// The `output.status` result: the session snapshot encoded outside the
// session's lock, which is what keeps that lock out of the socket's path.
boost::json::object statusParams(const OutputSession::Status& status) {
    const OutputSession::PreviewStatus& preview = status.preview;
    // The editor's off-air target of the same stream (task 6.16a, review
    // R16). `active` is the accept of a preview.select; revision/request name
    // what it accepted. Then the same three questions the mirror answers —
    // published counts frames that reached the socket, dropped the ones the
    // consumer's slots blocked, deferred the ones that stood down for the
    // audience — plus the one only a generation-managed target has: stale,
    // the frames a newer ask superseded before they could be published.
    const boost::json::object selection{
        {"active", preview.selection.active},
        {"revision", preview.selection.revision},
        {"request", preview.selection.request},
        {"published", preview.selection.published},
        {"dropped", preview.selection.dropped},
        {"deferred", preview.selection.deferred},
        {"stale", preview.selection.stale}};
    // The slide list's target (task 6.16b, review R16). Same questions as
    // the other two, answered for a BATCH: queued is what the worker still
    // owes a picture of and decodePending the part of it waiting on a codec
    // on the command thread, which is the difference between "working through
    // the list" and "idle with a list it will never draw". `images` is the
    // list's own decoded-picture cache: its budget differs from
    // status.scene.images' by design, and that is the evidence on the wire
    // that a scrolling document costs a bounded, SEPARATE pool.
    const boost::json::object thumbnails{
        {"active", preview.thumbnails.active},
        {"request", preview.thumbnails.request},
        {"width", preview.thumbnails.width},
        {"height", preview.thumbnails.height},
        {"queued", preview.thumbnails.queued},
        {"decodePending", preview.thumbnails.decodePending},
        {"published", preview.thumbnails.published},
        {"dropped", preview.thumbnails.dropped},
        {"deferred", preview.thumbnails.deferred},
        {"stale", preview.thumbnails.stale},
        {"images", boost::json::object{{"decodedBytes", preview.thumbnails.decodedBytes},
                                       {"entries", preview.thumbnails.entries},
                                       {"evictions", preview.thumbnails.evictions},
                                       {"budget", preview.thumbnails.budget}}}};
    // Per-item foreground failures (task 6.20d, review R18): the stack
    // pictures the operator composed but the worker could not decode, named
    // per asset path with the last attempt's reason (clipped to the echo
    // budget, the rule every string leaving the worker honours), plus the
    // lifetime attempt count behind the capped list. This is the media
    // lane's per-item honesty, beside — never inside — the lane it was
    // deliberately kept out of.
    boost::json::array foregroundErrors;
    for (const auto& failure : status.scene.foregroundFailures) {
        foregroundErrors.push_back(boost::json::object{{"path", failure.path},
                                                       {"error",
                                                        proto::clippedForEcho(failure.error)}});
    }
    return boost::json::object{{"device", status.deviceName},
                               {"width", status.width},
                               {"height", status.height},
                               {"frames", status.frames},
                               // Failed acquires/presents since start (task 6.4): the honest
                               // evidence that a confirmation took the long way around.
                               {"presentFailures", status.presentFailures},
                               {"revision", status.revision},
                               {"monitor", status.monitor},
                               {"monitorName", status.monitorName},
                               {"layers", layersObject(status.layerMask)},
                               {"preview", boost::json::object{{"running", preview.running},
                                                                {"name", preview.name},
                                                                {"width", preview.width},
                                                                {"height", preview.height},
                                                                {"fps", preview.fps},
                                                                {"published", preview.published},
                                                                {"dropped", preview.dropped},
                                                                // Task 6.11a (review R13): what the
                                                                // preview costs the output, and how
                                                                // often it stood down for the
                                                                // audience's sake.
                                                                {"deferred", preview.deferred},
                                                                {"worstMs", preview.worstMs},
                                                                {"selection", selection},
                                                                {"thumbnail", thumbnails},
                                                                {"error", preview.error}}},
                                {"scene", boost::json::object{{"active", status.scene.active},
                                                              {"pending", status.scene.pending},
                                                              {"transitioning",
                                                               status.scene.transitioning},
                                                              {"prepared", status.scene.prepared},
                                                              {"preparing", status.scene.preparing},
                                                              // The decoded-still cache (task 6.10,
                                                              // review R12): what the worker holds
                                                              // in CPU picture memory, what of it
                                                              // is pinned content, and whether the
                                                              // budget has begun evicting.
                                                              {"images",
                                                               boost::json::object{
                                                                   {"decodedBytes",
                                                                    status.scene.images.decodedBytes},
                                                                   {"entries",
                                                                    status.scene.images.entries},
                                                                   {"pins", status.scene.images.pins},
                                                                   {"evictions",
                                                                    status.scene.images.evictions},
                                                                   // preloadHits (task 6.9a): the
                                                                   // worker's own word that a
                                                                   // re-prepare found the still
                                                                   // already decoded, which no ack
                                                                   // latency on the wire can say.
                                                                   {"preloadHits",
                                                                    status.scene.images
                                                                        .preloadHits},
                                                                   // The draw-boundary trio
                                                                   // (task 6.9b, review R10):
                                                                   // what the frame loop still
                                                                   // had to build for itself.
                                                                   {"drawDecodes",
                                                                    status.scene.images
                                                                        .drawDecodes},
                                                                   {"drawBackdrops",
                                                                    status.scene.images
                                                                        .drawBackdrops},
                                                                   {"layoutBuilds",
                                                                    status.scene.images
                                                                        .layoutBuilds}}},
                                                              {"foregroundErrors",
                                                               foregroundErrors},
                                                              {"foregroundFailureEvents",
                                                               status.scene.foregroundFailureEvents}}},
                                // Background media (task 5.5d): the video
                                // transport's live state; error is sticky
                                // until a successful open replaces it.
                                // "audio" (task 6.7c, review R18) is the
                                // typed audible lane: none | silent |
                                // no-output | degraded | paused | live.
                                // loop/duration/sources (task 6.15a,
                                // review R15) acknowledge the transport
                                // commands: the rewind intent EOF will
                                // honour, the clip's probed length (-1 =
                                // no transport or unknown), and every
                                // REGISTERED audio source with its mixer
                                // echo — what status says is what the
                                // audience has.
                                {"media", boost::json::object{{"playing", status.media.playing},
                                                              {"path", status.media.path},
                                                              {"error", status.media.error},
                                                              {"audio", status.media.audio},
                                                              {"position", status.media.position},
                                                              {"live", status.media.live},
                                                              {"loop", status.media.loop},
                                                              {"duration", status.media.duration},
                                                              {"sources", sourcesArray(status.media)},
                                                              {"standalone",
                                                               standaloneStatus(status.media.standalone)}}}};
}

}  // namespace

// The `output.status` result (public): the session snapshot encoded outside
// the session's lock, which is what keeps that lock out of the socket's path.
boost::json::value statusResult(const OutputSession::Status& status) {
    return statusParams(status);
}

boost::json::value displaysResult(const std::vector<OutputSession::DisplayInfo>& displays) {
    boost::json::array array;
    for (const auto& display : displays) {
        array.push_back(boost::json::object{{"index", display.index},
                                            {"name", display.name},
                                            {"width", display.width},
                                            {"height", display.height},
                                            {"refreshHz", display.refreshHz}});
    }
    return array;
}

boost::json::object previewReadyParams(const OutputSession::PreviewReady& ready) {
    return {{"slot", ready.slot},
            {"seq", ready.seq},
            {"width", ready.width},
            {"height", ready.height},
            {"rowBytes", ready.rowBytes},
            {"revision", ready.revision},
            // Whose picture this is and which ask it answers (task 6.16a). The
            // consumer needs them BEFORE copying: one stream now carries the
            // audience's mirror and the editor's off-air render, and a frame
            // of the wrong kind under the right handles is precisely the stale
            // imagery R16 was about. They are the same values the slot header
            // carries, published under the same seqlock, so metadata and
            // pixels agree.
            {"target", ready.target},
            {"request", ready.request},
            {"publishedNs", ready.publishedNs},
            // Stream identity: the release must echo it (task 6.5).
            {"stream", ready.stream}};
}

boost::json::object mediaErrorParams(const OutputSession::MediaError& error) {
    return {{"path", error.path}, {"message", error.message}};
}

MethodTable::MethodTable(OutputSession& session, DeferredSink defer)
    : fSession(session), fDefer(std::move(defer)) {}

void MethodTable::dispatch(const proto::RequestEnvelope& envelope, std::string* outbound) {
    // The envelope, the notification policy and the hello gate are the
    // connection's (RpcServer::handle): by here the method is routable, the id
    // is one a reply can honestly carry, and the version is agreed.
    const std::string_view name = envelope.method;
    const boost::json::value nullParams = nullptr;
    const boost::json::value& given =
        envelope.params == nullptr ? nullParams : *envelope.params;
    const std::int64_t id = envelope.id;

    if (name == "ping") {
        proto::appendFrame(
            proto::makeResult(id, boost::json::object{{"pong", true}}), outbound);
    } else if (name == "output.status") {
        proto::appendFrame(
            proto::makeResult(id, statusParams(fSession.status())), outbound);
    } else if (name == "output.displays") {
        proto::appendFrame(
            proto::makeResult(id, displaysResult(fSession.displays())), outbound);
    } else if (name == "output.select") {
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "output.select requires params"),
                               outbound);
            return;
        }
        // The range is checked here, at the wire, and NOT by the cast below:
        // `static_cast<int>(4294967296)` is 0, which is a real destination, so
        // an argument nobody sent would move the audience's output (review
        // R11). -1 is windowed; the top of the range is what an `int` index
        // can name at all, and the session still decides whether a panel
        // exists.
        std::int64_t index = 0;
        if (!jsonIntInRange(given.as_object(), "monitor", -1,
                            std::numeric_limits<int>::max(), &index)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "output.select requires \"monitor\": -1 (windowed) "
                                 "or an index from output.displays"),
                outbound);
            return;
        }
        std::uint64_t revision = 0;
        if (!fSession.selectMonitor(static_cast<int>(index), &revision)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "output.select requires \"monitor\": -1 (windowed) "
                                 "or an index from output.displays"),
                outbound);
            return;
        }
        // The move happens on the render thread's next frame; revision
        // tells the frontend which status confirms arrival.
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"revision", revision},
                                                     {"pending", true}}),
                           outbound);
    } else if (name == "layer.setEnabled") {
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "layer.setEnabled requires params"),
                               outbound);
            return;
        }
        const boost::json::object& args = given.as_object();
        const auto layer = args.find("layer");
        const auto enabled = args.find("enabled");
        if (layer == args.end() || !layer->value().is_string() ||
            enabled == args.end() || !enabled->value().is_bool()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "layer.setEnabled requires a layer name "
                                                "and an enabled boolean"),
                               outbound);
            return;
        }
        std::uint64_t revision = 0;
        if (!fSession.setLayerEnabled(layer->value().as_string(),
                                      enabled->value().as_bool(), &revision)) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "unknown layer '" +
                                                    proto::clippedForEcho(
                                                        layer->value().as_string()) +
                                                    "'"),
                               outbound);
            return;
        }
        // The toggle lands on the next composed frame; revision tells the
        // frontend which status confirms it.
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"revision", revision},
                                                     {"pending", true}}),
                           outbound);
    } else if (name == "scene.prepare") {
        // The compiled scene travels as JSON (the frontend's compiler
        // produced it); validate it into the session's scene type before
        // storing. Nothing renders here — prepare only grows the store.
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "scene.prepare requires a scene object"),
                               outbound);
            return;
        }
        openshow::scene::Scene scene;
        std::string sceneError;
        if (!openshow::scene::fromJson(given, &scene, &sceneError)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 sceneError.empty() ? "scene.prepare received an invalid scene"
                                                    : sceneError),
                outbound);
            return;
        }
        // Task 6.9a (review R10): the store answer is immediate, the file
        // work is not. The revision is registered now (the pins too); the
        // ack itself travels when the executor finishes this prepare's
        // decodes and probe, so the loop goes on answering status, clear,
        // releases and shutdown while a cold file is being read. A refusal
        // to schedule (closing, or kMaxQueued prepares already waiting) is
        // answered here, explicitly.
        std::uint64_t revision = 0;
        const bool scheduled = fSession.prepareScene(
            scene, static_cast<std::uint64_t>(id),
            [defer = fDefer, id](std::uint64_t readyRevision, bool cancelled) {
                boost::json::value frame =
                    cancelled
                        ? proto::makeError(id, proto::kInternalError,
                                           "scene.prepare was retired before its file work ran")
                        : proto::makeResult(id, boost::json::object{
                                                    {"revision", readyRevision}});
                // The connection owns the bag and the self-wake; handing the
                // frame over is all a completion needs, from whatever thread
                // the executor runs it on.
                defer(std::move(frame));
            },
            &revision);
        if (!scheduled) {
            proto::appendFrame(
                proto::makeError(id, proto::kInternalError,
                                 "scene.prepare could not schedule its file work"),
                outbound);
            return;
        }
        // The ack answers from the executor thread (or, with no background
        // media, fired inline before this returned): nothing more to write
        // for this request on this thread.
    } else if (name == "scene.present") {
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "scene.present requires params"),
                               outbound);
            return;
        }
        std::uint64_t prepared = 0;
        if (!jsonUint64(given.as_object(), "revision", &prepared)) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "scene.present requires a \"revision\""),
                               outbound);
            return;
        }
        // Optional transition (task 3.2): "fade" (the default) or "cut".
        SceneTransitionKind kind = SceneTransitionKind::kFade;
        const boost::json::object& args = given.as_object();
        if (const auto it = args.find("transition"); it != args.end()) {
            if (!it->value().is_string()) {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "scene.present \"transition\" must be "
                                                    "a string"),
                                   outbound);
                return;
            }
            const std::string_view transition = it->value().as_string();
            if (transition == "fade") {
                kind = SceneTransitionKind::kFade;
            } else if (transition == "cut") {
                kind = SceneTransitionKind::kCut;
            } else {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "scene.present \"transition\" must be "
                                                    "\"fade\" or \"cut\""),
                                   outbound);
                return;
            }
        }
        std::uint64_t revision = 0;
        if (!fSession.presentScene(prepared, &revision, kind)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "no prepared scene with revision " +
                                     std::to_string(prepared)),
                outbound);
            return;
        }
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"revision", revision},
                                                     {"pending", true}}),
                           outbound);
    } else if (name == "preview.start") {
        // Each argument is optional, but a PRESENT argument must be a real
        // value: silently defaulting a "width" of "wide" (or letting 2^32+640
        // truncate back to 640) makes the client's request succeed with
        // geometry nobody asked for. The session then decides the sane band.
        uint32_t width = 640;
        uint32_t height = 360;
        uint32_t fps = 15;
        if (given.is_object()) {
            const boost::json::object& args = given.as_object();
            const struct {
                const char* key;
                uint32_t* slot;
            } numbers[] = {{"width", &width}, {"height", &height}, {"fps", &fps}};
            for (const auto& argument : numbers) {
                if (args.find(argument.key) == args.end()) {
                    continue;  // absent: the default above stands
                }
                uint32_t value = 0;
                if (!jsonUint32(args, argument.key, &value)) {
                    proto::appendFrame(
                        proto::makeError(id, proto::kInvalidParams,
                                         std::string("preview.start \"") + argument.key +
                                             "\" wants a whole number between 0 and " +
                                             std::to_string(4294967295u)),
                        outbound);
                    return;
                }
                *argument.slot = value;
            }
        }
        OutputSession::PreviewInfo info;
        std::string error;
        if (!fSession.startPreview(width, height, fps, &info, &error)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams, error), outbound);
            return;
        }
        // Pixels never ride the socket: this names the shm segment, the
        // consumer maps it and waits for preview.ready notifications.
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"name", info.name},
                                                     {"width", info.width},
                                                     {"height", info.height},
                                                     {"slotCount", info.slotCount},
                                                     {"bytesPerSlot", info.bytesPerSlot},
                                                     {"format", "RGBA8888"}}),
                           outbound);
    } else if (name == "preview.stop") {
        fSession.stopPreview();
        proto::appendFrame(proto::makeResult(id, boost::json::object{{"ok", true}}),
                           outbound);
    } else if (name == proto::kMethodPreviewSelect) {
        // The editor's off-air render (task 6.16a, review R16): "draw THIS
        // prepared scene into the preview stream without presenting it".
        // Both arguments are required, in their own widths, for the usual
        // reason — a revision that silently truncated to 0 would read as a
        // stand-down, and a request that wrapped into a smaller number would
        // be refused as stale by a client that believes it advanced.
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "preview.select wants {revision, request}"),
                               outbound);
            return;
        }
        const boost::json::object& args = given.as_object();
        std::uint64_t revision = 0;
        std::uint32_t request = 0;
        if (!jsonUint64(args, "revision", &revision) || !jsonUint32(args, "request", &request)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.select wants a whole-number revision and a "
                                 "request between 0 and 4294967295"),
                outbound);
            return;
        }
        std::string error;
        if (!fSession.selectPreviewScene(revision, request, &error)) {
            // The refusal names what the client can act on: no stream, a
            // generation that does not follow the accepted one (both numbers
            // in the text), or a revision the bounded store no longer holds.
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams, error), outbound);
            return;
        }
        // The answer is the accept, not the picture: whether a frame landed
        // is status.preview.selection.published's report, because the render
        // thread owns when it draws and this socket must not wait on it.
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"accepted", true},
                                                     {"revision", revision},
                                                     {"request", request}}),
                           outbound);
    } else if (name == proto::kMethodPreviewThumbnails) {
        // The slide list (task 6.16b, review R16): "picture these scenes at
        // this size, off the record". These scenes are NOT prepared — they
        // enter no store, pin no full-size decode and hold no revision anyone
        // can present — so this handler is the only place their cost is paid,
        // and the bounds below are what keep that cost proportionate to a
        // list of rows rather than to whatever a buggy client can send.
        if (!given.is_object()) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.thumbnails wants {scenes, width, height, request}"),
                outbound);
            return;
        }
        const boost::json::object& args = given.as_object();
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t request = 0;
        if (!jsonUint32(args, "width", &width) || !jsonUint32(args, "height", &height) ||
            !jsonUint32(args, "request", &request)) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.thumbnails wants width, height and request as "
                                 "whole numbers between 0 and 4294967295"),
                outbound);
            return;
        }
        const auto scenesEntry = args.find("scenes");
        if (scenesEntry == args.end() || !scenesEntry->value().is_array()) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.thumbnails wants a \"scenes\" array (an empty "
                                 "one stands the list down)"),
                outbound);
            return;
        }
        const boost::json::array& scenes = scenesEntry->value().as_array();
        // THE CAP, BEFORE ANYTHING IS PARSED. Three hundred scenes would be
        // three hundred scene objects compiled before the worker said no;
        // judging the count first makes the refusal cost one comparison,
        // which is the only way a cap bounds the work a request can cause
        // rather than the work it succeeds at.
        if (scenes.size() > proto::kMaxThumbnailBatch) {
            proto::appendFrame(
                proto::makeError(id, proto::kInvalidParams,
                                 "preview.thumbnails takes at most " +
                                     std::to_string(proto::kMaxThumbnailBatch) +
                                     " scenes, asked for " + std::to_string(scenes.size()) +
                                     " (request the visible band)"),
                outbound);
            return;
        }
        // Per-entry parsing, per-entry refusal: a slide whose JSON does not
        // compile loses its own picture, and the rest of the band still gets
        // its. A key over the limit is refused rather than clipped INTO the
        // identity — an accepted entry always carries the exact key it was
        // sent — and any key that is echoed back on the wire is clipped
        // first, R11's rule about putting a client's text in a reply.
        std::vector<OutputSession::ThumbnailEntry> batch;
        batch.reserve(scenes.size());
        std::vector<std::pair<std::string, std::string>> refused;
        for (const boost::json::value& item : scenes) {
            if (!item.is_object()) {
                refused.emplace_back(std::string(), "entry is not an object");
                continue;
            }
            const boost::json::object& entry = item.as_object();
            const auto key = entry.find("key");
            if (key == entry.end() || !key->value().is_string() ||
                key->value().as_string().empty()) {
                refused.emplace_back(std::string(), "entry has no \"key\"");
                continue;
            }
            const std::string_view rawKey = key->value().as_string();
            if (rawKey.size() > proto::kMaxThumbnailKeyLength) {
                refused.emplace_back(proto::clippedForEcho(rawKey),
                                     "key is over the " +
                                         std::to_string(proto::kMaxThumbnailKeyLength) +
                                         "-character limit");
                continue;
            }
            const auto sceneEntry = entry.find("scene");
            if (sceneEntry == entry.end() || !sceneEntry->value().is_object()) {
                refused.emplace_back(std::string(rawKey), "entry has no \"scene\" object");
                continue;
            }
            openshow::scene::Scene compiled;
            std::string sceneError;
            if (!openshow::scene::fromJson(sceneEntry->value(), &compiled, &sceneError)) {
                refused.emplace_back(std::string(rawKey),
                                     sceneError.empty() ? "not a compiled scene" : sceneError);
                continue;
            }
            OutputSession::ThumbnailEntry job;
            job.key = std::string(rawKey);
            job.scene = std::move(compiled);
            batch.push_back(std::move(job));
        }
        OutputSession::ThumbnailAck ack;
        std::string error;
        // Everything the session judges — no stream, an extent that could not
        // fit a slot, a replayed generation — refuses the WHOLE batch, and it
        // refuses it without having touched the pending list, so a refusal
        // leaves the previous band exactly as it stood.
        if (!fSession.requestThumbnails(batch, width, height, request, &ack, &error)) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams, error), outbound);
            return;
        }
        // The accept with the id each slide's frames will carry. Pixels are
        // not here and never will be: they arrive as preview.ready frames of
        // target "thumbnail" whose `revision` is this id.
        boost::json::array frames;
        for (const std::pair<std::string, std::uint32_t>& accepted : ack.accepted) {
            frames.push_back(
                boost::json::object{{"key", accepted.first}, {"id", accepted.second}});
        }
        boost::json::array refusals;
        for (const std::pair<std::string, std::string>& why : ack.refused) {
            refused.push_back(why);
        }
        for (const std::pair<std::string, std::string>& why : refused) {
            refusals.push_back(boost::json::object{{"key", why.first}, {"why", why.second}});
        }
        proto::appendFrame(proto::makeResult(id, boost::json::object{
                                                     {"accepted", true},
                                                     {"request", ack.request},
                                                     {"width", ack.width},
                                                     {"height", ack.height},
                                                     {"frames", std::move(frames)},
                                                     {"refused", std::move(refusals)}}),
                           outbound);
    } else if (name == proto::kMethodPreviewReleased) {
        // A notification from the consumer: it finished copying that slot
        // frame, so the producer may claim it again. Answered only if the
        // client bothered to identify it. The stream name is part of the
        // proof of ownership (task 6.5, review R5): a release that does
        // not name the live stream frees nothing.
        if (given.is_object()) {
            const boost::json::object& args = given.as_object();
            std::uint64_t seq = 0;
            std::uint32_t slot = 0;
            const auto stream = args.find("stream");
            // The slot is a 32-bit index and the sequence is not, so each is
            // read in its own width — a slot of 2^32+1 must not arrive as
            // slot 1 and release a frame the consumer never copied. The name
            // is bounded for the same reason R5 made it part of the proof:
            // it is compared, not stored, and an over-long one is not a
            // segment name by any construction the producer uses.
            const bool named = stream != args.end() && stream->value().is_string() &&
                               stream->value().as_string().size() <= 256;
            if (jsonUint32(args, "slot", &slot) && jsonUint64(args, "seq", &seq) && named) {
                fSession.releasePreviewSlot(slot, seq,
                                            std::string(stream->value().as_string()));
            }
        }
        if (envelope.hasId) {
            proto::appendFrame(proto::makeResult(id, boost::json::object{{"ok", true}}),
                               outbound);
        }
    } else if (name == proto::kMethodMediaPlay || name == proto::kMethodMediaPause) {
        // The media transport commands (task 6.15a, review R15): what the
        // benchmark's AvPlayer could already do, now answerable over the
        // socket. The shape is the session's existing one — the command
        // hands an intent to the transport's thread and answers
        // {"pending":true}; output.status.media is the acknowledgement
        // (playing flips, a held position holds), exactly how scenes and
        // layers confirm. Commands with no transport are refused NOW,
        // naming the reason, rather than dying unheard on a render thread
        // that was never asked.
        if (!fSession.mediaTransportLive()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "no background media is playing to " +
                                                    std::string(name)),
                               outbound);
        } else {
            if (name == proto::kMethodMediaPlay) {
                fSession.requestMediaPlay();
            } else {
                fSession.requestMediaPause();
            }
            proto::appendFrame(
                proto::makeResult(id, boost::json::object{{"pending", true}}), outbound);
        }
    } else if (name == proto::kMethodMediaSeek) {
        // The position is validated before it is believed: NaN fails the
        // range legs, a string or bool is not a position, and 24 hours of
        // media is a ceiling no real clip tests (the number is a bound on
        // nonsense, not a promise about durations).
        double target = 0.0;
        if (!given.is_object() ||
            !jsonDoubleInRange(given.as_object(), "position", 0.0, kMaxSeekSeconds, &target)) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "media.seek requires a numeric position "
                                                "within [0, 86400] seconds"),
                               outbound);
        } else if (!fSession.mediaTransportLive()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "no background media is playing to seek"),
                               outbound);
        } else {
            fSession.requestMediaSeek(target);
            proto::appendFrame(proto::makeResult(id, boost::json::object{{"pending", true},
                                                                         {"target", target}}),
                               outbound);
        }
    } else if (name == proto::kMethodMediaLoop) {
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "media.loop requires params"),
                               outbound);
        } else {
            const boost::json::object& args = given.as_object();
            const auto loop = args.find("loop");
            if (loop == args.end() || !loop->value().is_bool()) {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "media.loop requires a loop boolean"),
                                   outbound);
            } else if (!fSession.mediaTransportLive()) {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "no background media is playing to loop"),
                                   outbound);
            } else {
                const bool enabled = loop->value().as_bool();
                fSession.requestMediaLoop(enabled);
                // The EOF decision is the acknowledgement: with loop off
                // the clip ends once — status goes playing:false with no
                // fault and the position rests at the end while the
                // last-drawn frame holds on screen.
                proto::appendFrame(proto::makeResult(id, boost::json::object{{"pending", true},
                                                                             {"loop", enabled}}),
                                   outbound);
            }
        }
    } else if (name == proto::kMethodMediaGain || name == proto::kMethodMediaMute) {
        const bool isGain = name == proto::kMethodMediaGain;
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                std::string(name) + " requires params"),
                               outbound);
        } else {
            const boost::json::object& args = given.as_object();
            const auto source = args.find("source");
            const std::string sourceName =
                source != args.end() && source->value().is_string()
                    ? std::string(source->value().as_string())
                    : std::string();
            openshow::worker::OutputSession::AudioSource identity =
                openshow::worker::OutputSession::AudioSource::kVideo;
            if (sourceName == proto::kMediaSourceVideo) {
                identity = openshow::worker::OutputSession::AudioSource::kVideo;
            } else if (sourceName == proto::kMediaSourceStandalone) {
                identity = openshow::worker::OutputSession::AudioSource::kStandalone;
            }
            double gain = 0.0;
            bool muted = false;
            bool valueOk = false;
            if (isGain) {
                valueOk = jsonDoubleInRange(args, "gain", 0.0, 1.0, &gain);
            } else {
                const auto mute = args.find("mute");
                valueOk = mute != args.end() && mute->value().is_bool();
                if (valueOk) {
                    muted = mute->value().as_bool();
                }
            }
            std::string refusal;
            if (sourceName != proto::kMediaSourceVideo &&
                sourceName != proto::kMediaSourceStandalone) {
                refusal = "unknown audio source '" + proto::clippedForEcho(sourceName) +
                          "'; expected 'video' or 'standalone'";
            } else if (!valueOk) {
                refusal = isGain ? "media.gain requires a gain within [0, 1]"
                                 : "media.mute requires a mute boolean";
            } else {
                const bool accepted =
                    isGain ? fSession.requestMediaGain(identity, gain, &refusal)
                           : fSession.requestMediaMute(identity, muted, &refusal);
                if (accepted) {
                    refusal.clear();
                }
            }
            if (!refusal.empty()) {
                // A source this transport never registered is a refusal
                // the caller hears — the no-op AvPlayer passthrough for
                // unregistered identities stays below, but the socket
                // never lets a command evaporate.
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams, refusal),
                                   outbound);
            } else {
                boost::json::object ack{{"pending", true}, {"source", sourceName}};
                // Not one ternary: `isGain ? gain : muted` would quietly
                // promote bool to double and send 0/1 where the contract
                // says true/false.
                if (isGain) {
                    ack["gain"] = gain;
                } else {
                    ack["muted"] = muted;
                }
                proto::appendFrame(proto::makeResult(id, ack), outbound);
            }
        }
    } else if (name == proto::kMethodMediaStandalone) {
        // The independent production track (task 6.15b, review R15). The
        // path is a string, "" is the removal; the request is PROBED
        // before it is accepted, so "nothing readable at that path" is
        // answered with the reason and the old track keeps playing. Since
        // task 6.22e2 (review A5) that probe is no longer paid on this
        // thread: the answer travels as a deferred reply from the prepare
        // executor — the same route scene.prepare's ack took in 6.9a — and
        // a slow or stalled file leaves the RPC loop answering status,
        // releases and shutdown the whole while. An accepted path swaps on
        // the transport's thread, and status's standalone block is the
        // acknowledgement: live, path, playing, position, the lane, and
        // the track's own sticky error.
        if (!given.is_object()) {
            proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                "media.standalone requires params"),
                               outbound);
        } else {
            const boost::json::object& args = given.as_object();
            const auto path = args.find("path");
            if (path == args.end() || !path->value().is_string()) {
                proto::appendFrame(proto::makeError(id, proto::kInvalidParams,
                                                    "media.standalone requires a path string "
                                                    "(\"\" removes the track)"),
                                   outbound);
            } else {
                const std::string wanted(path->value().as_string());
                const bool scheduled = fSession.requestStandaloneAudioDeferred(
                    wanted, static_cast<std::uint64_t>(id),
                    [defer = fDefer, id, wanted](bool accepted, const std::string& refusal) {
                        // A removal (and a session with no media) answers
                        // this inline, before schedule returned; a probed
                        // request answers it from the executor thread, and
                        // the connection's bag plus self-wake make either
                        // thread safe.
                        boost::json::value frame =
                            accepted ? proto::makeResult(id, boost::json::object{
                                                                    {"pending", true},
                                                                    {"path", wanted}})
                                     : proto::makeError(id, proto::kInvalidParams,
                                                        refusal.empty() ? "standalone track refused"
                                                                        : refusal);
                        defer(std::move(frame));
                    });
                if (!scheduled) {
                    // Closing, or the prepare queue is at capacity: the
                    // honest answer is a refusal the caller can retry,
                    // here, because nothing was scheduled to answer later.
                    proto::appendFrame(
                        proto::makeError(id, proto::kInternalError,
                                         "media.standalone could not schedule its probe"),
                        outbound);
                }
            }
        }
    } else if (name == "shutdown") {
        proto::appendFrame(proto::makeResult(id, boost::json::object{{"ok", true}}),
                           outbound);
        fSession.requestStop();
    } else {
        proto::appendFrame(
            proto::makeError(id, proto::kMethodNotFound,
                             "no such method: " + std::string(name)),
            outbound);
    }
}

}  // namespace openshow::worker
