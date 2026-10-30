/*
 * WorkerController implementation. Notes:
 *
 * - The worker path comes from the build ($<TARGET_FILE>); packaging can
 *   point it next to the UI later (task 6.3).
 * - The socket, the framing and the reply ledger are RpcClient's (task 6.19a1).
 *   The preview stream and the pictures copied out of it are PreviewConsumer's
 *   (task 6.19a2). What is left here is the session: the process, the handshake
 *   and poll rhythm, and the acknowledged output model.
 */

#include "WorkerController.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTimer>

#include <chrono>

#include "openshow/protocol/preview_wire.h"

#include "PreviewConsumer.h"
#include "SceneAsk.h"

namespace openshow::ui {

namespace proto = openshow::protocol;

namespace {

constexpr int kStatusPollMs = 500;
// Revisions of prepared-scene JSON kept as a mirror for the clear
// controls (task 3.4). Bounded; the worker prunes its own store too.
constexpr int kSceneMirrorSize = 32;

}  // namespace

WorkerController::WorkerController(QObject* parent) : QObject(parent), fRpc(this) {
    // The transport's events, read as session news. A connection that came up is
    // a handshake to send; one that went away is a session to forget, with the
    // reason the transport kept for whichever of the two paths reports it last.
    connect(&fRpc, &RpcClient::socketConnected, this, &WorkerController::onRpcConnected);
    connect(&fRpc, &RpcClient::socketDisconnected, this, &WorkerController::onRpcDisconnected);
    connect(&fRpc, &RpcClient::replyReceived, this, &WorkerController::onRpcReply);
    connect(&fRpc, &RpcClient::notificationReceived, this, &WorkerController::handleNotification);
    connect(&fRpc, &RpcClient::statusMessage, this, &WorkerController::setStatus);
    // Reaching for the worker stops when this side asked for a shutdown or the
    // process is gone: the transport asks, because only the supervisor knows
    // which of those two the answer to "keep trying?" depends on.
    fRpc.setStopCondition([this] {
        return fShutdownRequested || fProcess.state() == QProcess::NotRunning;
    });
    connect(&fProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setStatus(QStringLiteral("worker failed to start"));
        }
    });
    connect(&fProcess, &QProcess::finished, this, &WorkerController::onProcessFinished);
    // The pictures (task 6.19a2). The consumer asks through this session and
    // never reaches past it: a preview ask is a request like any other, and the
    // scene budget it is checked against is the same one a present is checked
    // against, so the list, the canvas and the go button cannot disagree about
    // what the worker will take. What it has to say back is a picture (the views
    // bind to this class, not to it) or a status line.
    fPreview = new PreviewConsumer(
        PreviewConsumer::Outbound{
            [this](const char* method, boost::json::object params, std::shared_ptr<void> intent) {
                return sendRequest(method, std::move(params), std::move(intent));
            },
            [this](const char* method, boost::json::object params) {
                sendNotification(method, std::move(params));
            },
            [this](const boost::json::value& candidate, const char* verb) {
                return sceneIsSendable(candidate, verb);
            }},
        this);
    connect(fPreview, &PreviewConsumer::changed, this, &WorkerController::previewChanged);
    connect(fPreview, &PreviewConsumer::thumbnailReady, this, &WorkerController::thumbnailReady);
    connect(fPreview, &PreviewConsumer::statusMessage, this, &WorkerController::setStatus);
}

WorkerController::~WorkerController() {
    if (fProcess.state() != QProcess::NotRunning) {
        fProcess.kill();
        fProcess.waitForFinished(1000);
    }
    QFile::remove(fRpc.socketPath());
}

// The QML surface for the pictures (task 6.19a2): the answers are the
// consumer's, and these exist so that nothing on the view side has to know the
// session was split. previewChanged is the consumer's own signal forwarded, so
// a counter, a capability or a picture arriving wakes the same bindings.
const QImage& WorkerController::previewImage() const {
    return fPreview->previewImage();
}
bool WorkerController::previewActive() const {
    return fPreview->previewActive();
}
qint64 WorkerController::previewFrames() const {
    return fPreview->previewFrames();
}
qint64 WorkerController::previewDropped() const {
    return fPreview->previewDropped();
}
QString WorkerController::previewError() const {
    return fPreview->previewError();
}
const QImage& WorkerController::selectionImage() const {
    return fPreview->selectionImage();
}
bool WorkerController::selectionReady() const {
    return fPreview->selectionReady();
}
qint64 WorkerController::selectionFrames() const {
    return fPreview->selectionFrames();
}
qint64 WorkerController::selectionStale() const {
    return fPreview->selectionStale();
}
bool WorkerController::selectionSupported() const {
    return fPreview->selectionSupported();
}
bool WorkerController::thumbnailSupported() const {
    return fPreview->thumbnailSupported();
}
qint64 WorkerController::thumbnailFrames() const {
    return fPreview->thumbnailFrames();
}
qint64 WorkerController::thumbnailStale() const {
    return fPreview->thumbnailStale();
}
qint64 WorkerController::thumbnailCachedBytes() const {
    return fPreview->thumbnailCachedBytes();
}
QImage WorkerController::thumbnail(const QString& key) const {
    return fPreview->thumbnail(key);
}

void WorkerController::start() {
    // Tests supervise a scripted stand-in (worker_controller_tests drives
    // this binary against itself as the "worker"); production never sets it.
    const QString programOverride = qEnvironmentVariable("OPENSHOW_WORKER_PROGRAM");
#ifdef OPENSHOW_WORKER_PATH
    const QString workerPath =
        programOverride.isEmpty() ? QStringLiteral(OPENSHOW_WORKER_PATH) : programOverride;
#else
    const QString workerPath =
        programOverride.isEmpty()
            ? QCoreApplication::applicationDirPath() + QStringLiteral("/openshow-worker")
            : programOverride;
#endif
    setStatus(QStringLiteral("starting worker..."));
    QStringList arguments{QStringLiteral("--socket"), fRpc.socketPath()};
    // Development hook, e.g. OPENSHOW_WORKER_EXTRA_ARGS="--monitor 1" to
    // send the audience output to the second display while debugging.
    const QString extra = qEnvironmentVariable("OPENSHOW_WORKER_EXTRA_ARGS");
    if (!extra.isEmpty()) {
        arguments.append(extra.split(u' ', Qt::SkipEmptyParts));
    }
    // Forward the worker's output into the UI's own log so supervision is
    // observable (M6.2 will turn this into proper diagnostics).
    fProcess.setProcessChannelMode(QProcess::ForwardedChannels);
    fProcess.start(workerPath, arguments);
    fRpc.startConnecting();
}

void WorkerController::onRpcConnected() {
    fConnected = true;
    fStatusRequestId = 0;  // the ledger is the transport's, and it started empty
    setStatus(QStringLiteral("handshaking..."));
    boost::json::object params = proto::helloParams("openshow-ui", proto::kProtocolMajor,
                                                    proto::kProtocolMinor);
    sendRequest("hello", std::move(params));
    emit stateChanged();
}

void WorkerController::onRpcDisconnected() {
    fConnected = false;
    fHandshakeDone = false;
    fStatusRequestId = 0;
    fPreview->setSessionLive(false);
    fPreview->closeStream();
    resetSceneTracking();
    fMediaPlaying = false;
    fMediaError.clear();  // the media state describes a worker we lost
    fMediaAudio = QStringLiteral("none");
    emit mediaChanged();
    emit stateChanged();
    if (!fShutdownRequested) {
        // A connection this side closed because the worker had stopped reading
        // says so: "connection lost" would leave the operator looking for a
        // crash in a worker that is still running (task 6.8b, review R11).
        setStatus(fRpc.endedByWriteStall() ? fRpc.writeStallMessage()
                                           : QStringLiteral("worker connection lost"));
    }
}

void WorkerController::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    fConnected = false;
    fOutputStopped = true;
    fStatusRequestId = 0;
    // The stream died with the process: its mapping, and every picture that
    // was a window into it, go now (task 6.19a2).
    fPreview->closeStream();
    resetSceneTracking();
    fMediaPlaying = false;
    fMediaError.clear();
    fMediaAudio = QStringLiteral("none");
    emit mediaChanged();
    emit stateChanged();
    if (fShutdownRequested) {
        setStatus(QStringLiteral("worker stopped (exit %1)").arg(exitCode));
    } else if (exitStatus == QProcess::CrashExit) {
        setStatus(QStringLiteral("worker crashed"));
    } else if (fRpc.endedByWriteStall()) {
        // Keep the reason this side hung up: "worker exited (0)" would read as
        // if nothing had gone wrong, when what went wrong was that the worker
        // stopped reading and the operator is the one who ended it.
        setStatus(fRpc.writeStallMessage().append(
            QStringLiteral(" (worker exited %1)").arg(exitCode)));
    } else {
        setStatus(QStringLiteral("worker exited (%1)").arg(exitCode));
    }
}

void WorkerController::onRpcReply(const RpcRequest& pending, const boost::json::object& message) {
    // The transport has already done the correlation: it found the request this
    // reply's id names, consumed its ledger entry, and dropped anything written
    // on a connection that is gone. What is left is the meaning, which is read
    // off the METHOD the request went out under — never off the shape of the
    // result, which is what let a burst of prepares misattribute payloads before
    // task 6.3a — plus the intent its sender left for exactly this answer.
    const std::int64_t id = pending.id;
    const std::shared_ptr<ScenePrepareAsk> armed = pending.intentAs<ScenePrepareAsk>();
    const ScenePrepareAsk nothing;  // a request whose sender needed nothing remembered
    const ScenePrepareAsk& intent = armed ? *armed : nothing;
    if (id == fStatusRequestId) {
        fStatusRequestId = 0;  // the poll is answered: the next tick may ask again
    }

    const auto error = message.find("error");
    if (error != message.end() && error->value().is_object()) {
        // An error answers its request's intent: drop that context so a
        // failed go cannot resurface on some later reply.
        if (id == fRestoreArmedGo) {
            // A rejected restore keeps its retained content (task 6.3b):
            // only the in-flight marker clears, so the retry is available.
            fRestoreArmedGo = 0;
        }
        // A refused ask that was carrying a picture was never MADE, so the
        // dedupe that remembers it has to forget it and the status poll can
        // re-issue what the UI still wants (task 6.16a, 6.16b).
        fPreview->handleRefusal(intent);
        const boost::json::object& reported = error->value().as_object();
        const auto text = reported.find("message");
        setStatus(QStringLiteral("worker error (%1): %2")
                      .arg(QString::fromStdString(pending.method),
                           text != reported.end() && text->value().is_string()
                               ? QString::fromStdString(
                                     std::string(text->value().as_string()))
                               : QStringLiteral("unspecified")));
        return;
    }
    const auto result = message.find("result");
    if (result == message.end()) {
        return;
    }
    const boost::json::value& payloadValue = result->value();

    if (pending.method == "output.displays") {  // answers with a bare array
        if (!payloadValue.is_array()) {
            return;
        }
        QVariantList displays;
        for (const auto& entry : payloadValue.as_array()) {
            const boost::json::object& panel = entry.as_object();
            QVariantMap item;
            item.insert(QStringLiteral("index"),
                        static_cast<qint64>(panel.at("index").to_number<std::int64_t>()));
            item.insert(QStringLiteral("name"),
                        QString::fromStdString(std::string(panel.at("name").as_string())));
            item.insert(QStringLiteral("size"),
                        QStringLiteral("%1x%2@%3Hz")
                            .arg(static_cast<qint64>(
                                panel.at("width").to_number<std::int64_t>()))
                            .arg(static_cast<qint64>(
                                panel.at("height").to_number<std::int64_t>()))
                            .arg(static_cast<qint64>(
                                panel.at("refreshHz").to_number<std::int64_t>())));
            displays.push_back(item);
        }
        fDisplays = displays;
        emit displaysChanged();
        return;
    }

    if (!payloadValue.is_object()) {
        return;
    }
    const boost::json::object& payload = payloadValue.as_object();

    if (pending.method == "hello") {  // the handshake answer
        if (!payload.contains("application")) {
            return;
        }
        fHandshakeDone = true;
        // The pictures ride this session, so they are online when it is: an
        // ask sent before the worker has identified itself is a request the
        // worker would answer with a method it has not agreed to speak.
        fPreview->setSessionLive(true);
        const boost::json::object& negotiated = payload.at("protocol").as_object();
        setStatus(QStringLiteral("connected to worker %1 (protocol %2.%3)")
                      .arg(QString::fromStdString(
                               std::string(payload.at("version").as_string())),
                           QString::number(static_cast<qlonglong>(
                               negotiated.at("major").to_number<std::uint64_t>())),
                           QString::number(static_cast<qlonglong>(
                               negotiated.at("minor").to_number<std::uint64_t>()))));
        // Ask for the first status immediately, then keep it fresh while
        // connected; the poll stops itself once the socket is gone.
        sendRequest("output.displays", {});
        // Preview (task 1.4): ask for the shm stream whenever the worker
        // speaks the capability; the start answer opens the mapping.
        bool previewCapable = false;
        bool selectionCapable = false;
        bool thumbnailCapable = false;
        if (payload.contains("capabilities") && payload.at("capabilities").is_array()) {
            for (const auto& capability : payload.at("capabilities").as_array()) {
                if (!capability.is_string()) {
                    continue;
                }
                if (capability.as_string() == "preview.shm.v1") {
                    previewCapable = true;
                } else if (capability.as_string() == "preview.select.v1") {
                    // The worker can draw a scene the audience is NOT shown
                    // (task 6.16a). Without this the edit canvas has no
                    // worker picture to show and says so; it never falls
                    // back to laying the text out itself, which is the two
                    // renderers R16 was about.
                    selectionCapable = true;
                } else if (capability.as_string() == "preview.thumbnail.v1") {
                    // The worker will render the slide list (task 6.16b).
                    // This capability is the whole negotiation: without it
                    // the list keeps the text line it already has, and with
                    // it the list gets the production compositor's own pixels
                    // at the size the rows are drawn. A worker too old for
                    // the method answers "method not found", which is why the
                    // list asks only when it is named.
                    thumbnailCapable = true;
                }
            }
        }
        // Both targets at once, before anything is asked of them: the hello
        // reply is the only place either answer changes, and the consumer's
        // gates are what stop a worker that dropped a capability from being
        // asked for a picture it can no longer draw.
        fPreview->setCapabilities(selectionCapable, thumbnailCapable);
        if (previewCapable) {
            sendRequest("preview.start", boost::json::object{{"width", 640},
                                                             {"height", 360},
                                                             {"fps", 15}});
        }
        QTimer::singleShot(0, this, &WorkerController::pollStatus);
        auto* poll = new QTimer(this);
        poll->setInterval(kStatusPollMs);
        connect(poll, &QTimer::timeout, this, [this, poll] {
            if (!fConnected) {
                poll->stop();
                poll->deleteLater();
                return;
            }
            pollStatus();
        });
        poll->start();
        return;
    }

    if (pending.method == "preview.start") {
        // A stream is the consumer's to own: the segment it names, the
        // generation domain that starts with it, and whatever the editor and
        // the list were waiting for this stream to be asked for (task 6.16a,
        // 6.16b).
        fPreview->handleStartReply(payload);
        return;
    }

    if (pending.method == "scene.prepare") {
        if (!payload.contains("revision")) {
            return;
        }
        // The scene now lives in the worker's store under this revision,
        // and THIS request's payload is what was stored under it — a
        // second prepare issued before the first was acknowledged cannot
        // move either mirror entry (task 6.3a).
        const qint64 prepared =
            static_cast<qint64>(payload.at("revision").to_number<std::uint64_t>());
        if (!intent.prepareJson.isEmpty()) {
            fSceneJson.insert(prepared, intent.prepareJson);
            if (prepared > fMirrorMax) {
                fMirrorMax = prepared;
            }
            if (fSceneJson.size() > kSceneMirrorSize) {
                const qint64 cutoff = fMirrorMax - kSceneMirrorSize;
                for (auto entry = fSceneJson.begin(); entry != fSceneJson.end();) {
                    if (entry.key() <= cutoff) {
                        entry = fSceneJson.erase(entry);
                    } else {
                        ++entry;
                    }
                }
            }
        }
        // Latest-go rule: the audience moves only for the newest go. A
        // superseded prepare still mirrors its payload (the store has it;
        // clears and indicators may still reference that revision) but
        // must not present it, so a slow ack for an older go can never
        // regress the audience off the newer slide already intended.
        if (intent.autoPresent && id == fLatestGoId) {
            fLatestGoId = 0;
            presentScene(prepared);
            if (id == fRestoreArmedGo) {
                // The restore's own go moved the audience; its snapshot
                // retires once a status poll confirms this revision on
                // air — until then canRestore honestly says the restored
                // scene is not yet confirmed (task 6.3b).
                fRestoreArmedGo = 0;
                fRestoreDoneRevision = prepared;
            }
        }
        // The off-air ask (task 6.16a): when this prepare was one, this
        // revision IS the picture the editor asked for, and the consumer names
        // it back with the generation the ask carried. The worker refuses a
        // generation that does not follow the one it accepted — which is the
        // rule that keeps a retried or reordered prepare from putting an edit
        // the operator abandoned back on the canvas.
        fPreview->handlePrepareAck(intent, prepared);
        emit sceneChanged();
        return;
    }

    if (pending.method == proto::kMethodPreviewThumbnails) {
        // The ack for a band (task 6.16b): the per-slide ids its frames will
        // carry, which only the consumer's own generation gate can decide are
        // still somebody's.
        fPreview->handleThumbnailsAck(intent, payload);
        return;
    }

    if (pending.method == "output.status") {  // the poll answer
        if (!payload.contains("frames")) {
            return;
        }
        fFrames = static_cast<qint64>(payload.at("frames").to_number<std::uint64_t>());
        fRevision = static_cast<qint64>(payload.at("revision").to_number<std::uint64_t>());
        if (payload.contains("device")) {
            fDevice = QString::fromStdString(std::string(payload.at("device").as_string()));
        }
        if (payload.contains("monitor")) {
            fCurrentMonitor =
                static_cast<qint64>(payload.at("monitor").to_number<std::int64_t>());
        }
        if (payload.contains("scene") && payload.at("scene").is_object()) {
            const boost::json::object& scene = payload.at("scene").as_object();
            fSceneActive =
                static_cast<qint64>(scene.at("active").to_number<std::uint64_t>());
            fScenePending =
                static_cast<qint64>(scene.at("pending").to_number<std::uint64_t>());
            fScenePrepared =
                static_cast<qint64>(scene.at("prepared").to_number<std::uint64_t>());
            // A restore is confirmed when the revision it re-prepared is
            // the one on air: only now does the retained snapshot retire
            // and canRestore go quiet (task 6.3b).
            if (fRestoreDoneRevision != 0 && fSceneActive == fRestoreDoneRevision) {
                fRestoreDoneRevision = 0;
                fRestoreJson.clear();
            }
            // The live indicators describe the acknowledged active revision
            // only (task 3.4): words/background flags come from the JSON that
            // revision was prepared with, so a clear or present lights the
            // indicator when the output has it, not when it was requested.
            fSceneContent = sceneContent(fSceneJson.value(fSceneActive));
            emit sceneChanged();
        }
        // The preview block (task 6.19a2): the worker's own counters, and the
        // one place a target the worker stood down on its own is noticed and
        // asked for again. Owned end to end by the consumer.
        fPreview->updateFromStatus(payload);
        // Background media (task 5.5d). Optional: a worker predating the
        // media object keeps whatever the last notification said rather
        // than falsely clearing it.
        if (payload.contains("media") && payload.at("media").is_object()) {
            const boost::json::object& media = payload.at("media").as_object();
            fMediaPlaying = media.at("playing").as_bool();
            fMediaError =
                QString::fromStdString(std::string(media.at("error").as_string()));
            // Optional even within minor 5's spirit: a worker that does not
            // send the lane leaves the last known answer rather than
            // pretending the audio went away.
            const auto audio = media.find("audio");
            if (audio != media.end() && audio->value().is_string()) {
                fMediaAudio = QString::fromStdString(std::string(audio->value().as_string()));
            }
            // Transport state (task 6.15a, review R15), each field optional:
            // a worker older than minor 6 keeps the last known answer rather
            // than being read as "stopped" for fields it never sends.
            const auto live = media.find("live");
            if (live != media.end() && live->value().is_bool()) {
                fMediaLive = live->value().as_bool();
            }
            const auto position = media.find("position");
            if (position != media.end() && position->value().is_number()) {
                fMediaPosition = position->value().to_number<double>();
            }
            const auto duration = media.find("duration");
            if (duration != media.end() && duration->value().is_number()) {
                fMediaDuration = duration->value().to_number<double>();
            }
            const auto loop = media.find("loop");
            if (loop != media.end() && loop->value().is_bool()) {
                fMediaLoop = loop->value().as_bool();
            }
            const auto sources = media.find("sources");
            if (sources != media.end() && sources->value().is_array()) {
                QVariantList listed;
                for (const boost::json::value& entry : sources->value().as_array()) {
                    if (!entry.is_object()) {
                        continue;
                    }
                    const boost::json::object& source = entry.as_object();
                    const auto name = source.find("name");
                    const auto gain = source.find("gain");
                    const auto muted = source.find("muted");
                    if (name == source.end() || !name->value().is_string() ||
                        gain == source.end() || !gain->value().is_number() ||
                        muted == source.end() || !muted->value().is_bool()) {
                        continue;  // a half-described source is not listed
                    }
                    QVariantMap row;
                    row.insert(QStringLiteral("id"),
                               QString::fromStdString(std::string(name->value().as_string())));
                    row.insert(QStringLiteral("gain"), gain->value().to_number<double>());
                    row.insert(QStringLiteral("muted"), muted->value().as_bool());
                    listed.append(row);
                }
                fMediaSources = listed;
            }
            // The standalone track's own block (task 6.15b, review R15),
            // optional field by field like the rest: the track reports
            // itself, never a hopeful selection.
            const auto standalone = media.find("standalone");
            if (standalone != media.end() && standalone->value().is_object()) {
                const boost::json::object& track = standalone->value().as_object();
                const auto live = track.find("live");
                if (live != track.end() && live->value().is_bool()) {
                    fStandaloneLive = live->value().as_bool();
                }
                const auto path = track.find("path");
                if (path != track.end() && path->value().is_string()) {
                    fStandalonePath =
                        QString::fromStdString(std::string(path->value().as_string()));
                }
                const auto position = track.find("position");
                if (position != track.end() && position->value().is_number()) {
                    fStandalonePosition = position->value().to_number<double>();
                }
                const auto error = track.find("error");
                if (error != track.end() && error->value().is_string()) {
                    fStandaloneError =
                        QString::fromStdString(std::string(error->value().as_string()));
                }
            }
            emit mediaChanged();
        }
        QVariantMap layers;
        for (const auto& entry : payload.at("layers").as_object()) {
            layers.insert(QString::fromStdString(std::string(entry.key())),
                          entry.value().as_bool());
        }
        fLayers = layers;
        emit outputStatusChanged();
    }
    // Acks of layer.setEnabled, scene.present, output.select and shutdown
    // carry no display state of their own; the next status poll confirms
    // them. Their pending entries were already consumed above.
}

void WorkerController::pollStatus() {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    // One status request on the wire at a time (task 6.8b, review R11). The
    // poll asks "what does the output look like", and a second identical
    // question behind an unanswered first one cannot be answered any better —
    // it only lengthens the queue in front of the commands that need the
    // socket. The next tick asks again, and by then the answer is fresher.
    if (fStatusRequestId != 0) {
        return;
    }
    fStatusRequestId = sendRequest("output.status", {});
}

void WorkerController::setLayerEnabled(const QString& name, bool enabled) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest("layer.setEnabled",
                boost::json::object{{"layer", name.toStdString()},
                                    {"enabled", enabled}});
}

// The authored scene is checked HERE, before it is sent (task 6.8a, review
// R11): the budgets that bound a slide — items, lines, characters, what an
// asset reference may name — live in openshow::scene, and this is the reader
// the worker runs too, so the two sides cannot disagree about what fits.
// Checking it here is what "rejected locally" means for the operator who pasted
// a novel into a lyric field: no payload crosses the socket, the worker never
// hangs up on them, and the status line names the rule the slide broke.
bool WorkerController::sceneIsSendable(const boost::json::value& candidate,
                                       const char* verb) {
    openshow::scene::Scene parsed;
    std::string error;
    if (openshow::scene::fromJson(candidate, &parsed, &error)) {
        return true;
    }
    setStatus(QStringLiteral("%1: the scene was not sent - %2")
                  .arg(QString::fromLatin1(verb), QString::fromStdString(error)));
    return false;
}

void WorkerController::prepareScene(const QString& sceneJson) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    boost::system::error_code parseError;
    const boost::json::value parsed =
        boost::json::parse(sceneJson.toStdString(), parseError);
    if (parseError || !parsed.is_object()) {
        setStatus(QStringLiteral("scene.prepare: the scene JSON is not readable"));
        return;
    }
    if (!sceneIsSendable(parsed, "scene.prepare")) {
        return;
    }
    // The scene object itself is the params (the worker validates it too).
    const std::int64_t id = sendRequest("scene.prepare", parsed.as_object());
    armPrepare(id, parsed.as_object(), /*autoPresent=*/false);
}

// --- the pictures (task 6.16a, 6.16b; the consumer is task 6.19a2) ---------
//
// Both targets are choreographed by PreviewConsumer: the prepare-then-select
// dance for the slide under edit, the batch and its ack for the visible band,
// the byte-bounded cache, and the generation gates that decide whether a frame
// that arrived is still anybody's picture. What the session adds is nothing:
// the UI asks through this class because the operations the RPC exposes are the
// operations the QML names, and the answers the views bind to are the
// consumer's, forwarded.
void WorkerController::requestSelectionPreview(const QString& sceneJson) {
    fPreview->requestSelectionPreview(sceneJson);
}

void WorkerController::clearSelectionPreview() {
    fPreview->clearSelectionPreview();
}

void WorkerController::requestThumbnails(const QVariantList& slides, qint64 width,
                                         qint64 height) {
    fPreview->requestThumbnails(slides, width, height);
}

void WorkerController::clearThumbnails() {
    fPreview->clearThumbnails();
}

void WorkerController::presentScene(qint64 revision) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    // An explicit present re-targets the logical presentation: later
    // clears derive from what this revision was prepared with (task 6.3b).
    fDesiredSceneJson = fSceneJson.value(revision);
    sendRequest("scene.present",
                boost::json::object{{"revision", static_cast<std::uint64_t>(revision)}});
}

void WorkerController::presentSceneJson(const QString& sceneJson) {
    // New content is a fresh go: whatever a clear sequence kept for restore is
    // history now (the operator moved on). That only becomes true once the new
    // content is actually on its way (task 6.8a): a scene this client refuses
    // to send leaves the audience exactly where it was, and the snapshot that
    // could put it back is still worth having.
    const std::int64_t id = presentSceneInternal(sceneJson);
    if (id == 0) {
        return;
    }
    fRestoreJson.clear();
    fRestoreArmedGo = 0;
    fRestoreDoneRevision = 0;
}

void WorkerController::adoptAssetPaths(const QVariantMap& pathMap) {
    if (pathMap.isEmpty()) {
        return;
    }
    // One payload's rewrite: the new JSON when background.assetPath named a
    // path this save moved, empty for anything else — an unreadable payload
    // or one that does not name the file is left exactly as it was, because
    // there is nothing here that was made stale by the move.
    const auto rewrite = [&pathMap](const QString& sceneJson) {
        if (sceneJson.isEmpty()) {
            return QString();
        }
        boost::system::error_code parseError;
        boost::json::value parsed = boost::json::parse(sceneJson.toStdString(), parseError);
        if (parseError || !parsed.is_object()) {
            return QString();
        }
        const auto background = parsed.as_object().find("background");
        if (background == parsed.as_object().end() || !background->value().is_object()) {
            return QString();
        }
        const auto path = background->value().as_object().find("assetPath");
        if (path == background->value().as_object().end() || !path->value().is_string()) {
            return QString();
        }
        const QString oldPath = QString::fromStdString(std::string(path->value().as_string()));
        if (!pathMap.contains(oldPath)) {
            return QString();
        }
        path->value() = pathMap.value(oldPath).toString().toStdString();
        return QString::fromStdString(boost::json::serialize(parsed));
    };

    // The mirror: each retained revision's payload. A later explicit
    // presentScene(revision) must not put a deleted path back on air.
    for (auto entry = fSceneJson.begin(); entry != fSceneJson.end(); ++entry) {
        const QString adopted = rewrite(entry.value());
        if (!adopted.isEmpty()) {
            entry.value() = adopted;
        }
    }
    // A prepare whose ack has not landed yet: the worker has the old payload
    // (it decoded it at prepare time, and the on-air path stays pinned), but
    // when the ack arrives the mirror must record what the session now
    // believes owns those bytes. The transport holds the request; the payload
    // the session attached to it is what gets rewritten.
    fRpc.eachPending([&rewrite](RpcRequest& request) {
        const auto ask = request.intentAs<ScenePrepareAsk>();
        if (!ask || ask->prepareJson.isEmpty()) {
            return;
        }
        const QString adopted = rewrite(ask->prepareJson);
        if (!adopted.isEmpty()) {
            ask->prepareJson = adopted;
        }
    });
    // The restore snapshot re-prepares through presentSceneInternal when the
    // operator clicks it, so rewriting it here is enough: it names the new
    // file at the moment it is used.
    const QString adoptedRestore = rewrite(fRestoreJson);
    if (!adoptedRestore.isEmpty()) {
        fRestoreJson = adoptedRestore;
    }
    const QString adoptedDesired = rewrite(fDesiredSceneJson);
    if (adoptedDesired.isEmpty()) {
        return;  // nothing the output is owed named a moved file
    }
    fDesiredSceneJson = adoptedDesired;
    // Stage the corrected payload for the next go (a clear deriving from it,
    // an explicit present). Prepare only: presenting would restart a fade.
    prepareScene(fDesiredSceneJson);
}

std::int64_t WorkerController::presentSceneInternal(const QString& sceneJson) {
    if (!fConnected || !fHandshakeDone) {
        return 0;
    }
    boost::system::error_code parseError;
    const boost::json::value parsed =
        boost::json::parse(sceneJson.toStdString(), parseError);
    if (parseError || !parsed.is_object()) {
        setStatus(QStringLiteral("scene.present: the scene JSON is not readable"));
        return 0;
    }
    if (!sceneIsSendable(parsed, "scene.present")) {
        return 0;
    }
    // Same hand-off as presentDemoSlide: present on the prepare ack, so
    // the audience switch lands as soon as the store names the revision.
    const std::int64_t id = sendRequest("scene.prepare", parsed.as_object());
    armPrepare(id, parsed.as_object(), /*autoPresent=*/true);
    return id;
}

// --- output controls (task 3.4, hardened by task 6.3b) --------------------
//
// Each clear presents a scene derived from the PENDING logical
// presentation — the newest go's payload, not merely the last confirmed
// status — so two clears clicked back to back compose: the second removes
// its layer from what the first already staged instead of resurrecting it
// from a stale snapshot. The layer promises hold: clears are independent,
// the untouched layer rides through unchanged, and the latest-go rule
// (task 6.3a) means the audience only ever moves to the composed result.
// The scene the first clear of a sequence cut from is retained as content,
// so a restore survives the worker pruning its store; the retained intent
// lives until the restore is confirmed on air, and a failed restore leaves
// it in place so the retry stays available.

QString WorkerController::clearSourceScene() const {
    if (!fDesiredSceneJson.isEmpty()) {
        return fDesiredSceneJson;
    }
    return fSceneJson.value(fSceneActive);
}

void WorkerController::clearSlide() {
    const QString source = clearSourceScene();
    if (source.isEmpty()) {
        return;  // nothing on air with a known payload: nothing to clear
    }
    const QString cleared = withWordsCleared(source);
    if (cleared.isEmpty() || cleared == source) {
        return;  // nothing to clear, or already gone: coalesce to a no-op
    }
    if (fRestoreJson.isEmpty()) {
        fRestoreJson = source;
    }
    presentSceneInternal(cleared);
}

void WorkerController::clearBackground() {
    const QString source = clearSourceScene();
    if (source.isEmpty()) {
        return;
    }
    const QString cleared = withBackgroundCleared(source);
    if (cleared.isEmpty() || cleared == source) {
        return;  // already gone: coalesce to a no-op
    }
    if (fRestoreJson.isEmpty()) {
        fRestoreJson = source;
    }
    presentSceneInternal(cleared);
}

void WorkerController::clearAll() {
    const QString source = clearSourceScene();
    if (source.isEmpty()) {
        return;
    }
    const QString cleared = clearedSceneJson();
    if (cleared.isEmpty() || cleared == source) {
        return;
    }
    if (fRestoreJson.isEmpty()) {
        fRestoreJson = source;
    }
    presentSceneInternal(cleared);
}

void WorkerController::restoreOutput() {
    if (fRestoreJson.isEmpty() || fRestoreArmedGo != 0) {
        return;  // nothing retained, or a restore is already in flight
    }
    // Restore re-prepares the RETAINED content rather than presenting the
    // pre-clear revision: the worker may have pruned its store since the
    // clear, and the audience is owed the scene the clear cut from
    // regardless (task 6.3b). Until a status poll confirms the restored
    // revision on air the snapshot stays, so canRestore never goes quiet
    // on a restore the output has not taken — and an error reply leaves it
    // here, keeping the retry available.
    const std::int64_t id = presentSceneInternal(fRestoreJson);
    if (id != 0) {
        fRestoreArmedGo = id;
    }
}

void WorkerController::armPrepare(std::int64_t requestId, const boost::json::object& scene,
                                  bool autoPresent) {
    // The prepare's payload is recorded on the request itself, not on a side
    // table keyed by id: when the ack lands it brings this record with it, so a
    // mirror entry can only ever be written from the payload THAT prepare sent
    // (task 6.3a). A refused send has no entry, so it arms nothing.
    RpcRequest* entry = fRpc.pending(requestId);
    if (!entry) {
        return;  // sendRequest always records the entry; belt and braces
    }
    auto ask = entry->intentAs<ScenePrepareAsk>();
    if (!ask) {
        ask = std::make_shared<ScenePrepareAsk>();
        entry->intent = ask;
    }
    ask->prepareJson = QString::fromStdString(boost::json::serialize(scene));
    ask->autoPresent = autoPresent;
    if (autoPresent) {
        // This go supersedes any older one still awaiting its ack; only
        // the newest go's acknowledgement moves the audience. Its payload
        // is the pending logical presentation the clears compose against
        // (task 6.3b).
        fDesiredSceneJson = ask->prepareJson;
        fLatestGoId = requestId;
    }
}

void WorkerController::resetSceneTracking() {
    fSceneJson.clear();
    fMirrorMax = 0;
    fSceneContent = SceneContent{};
    fRestoreJson.clear();
    fRestoreArmedGo = 0;
    fRestoreDoneRevision = 0;
    fDesiredSceneJson.clear();
    fLatestGoId = 0;
    // In-flight intents die with the connection; a reply can no longer
    // arrive for them, and none must survive to act on a later one. The
    // transport holds the requests, so this is where they are given up on.
    fRpc.clearPending();
    emit sceneChanged();
}

void WorkerController::presentDemoSlide(qint64 index) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    openshow::scene::Scene scene;
    std::string error;
    if (!openshow::scene::compileDemoSlide(fDemoDocuments,
                                           static_cast<std::size_t>(index), &scene, &error)) {
        setStatus(QStringLiteral("demo slide %1: %2")
                      .arg(index)
                      .arg(QString::fromStdString(error)));
        return;
    }
    boost::json::value jsonScene = openshow::scene::toJson(scene);
    // Present on the prepare acknowledgement: the revision is the
    // worker's to hand out, and the store only names it once stored.
    fRestoreJson.clear();  // a fresh go retires any clear's restore point
    fRestoreArmedGo = 0;
    fRestoreDoneRevision = 0;
    const std::int64_t id = sendRequest("scene.prepare", jsonScene.as_object());
    armPrepare(id, jsonScene.as_object(), /*autoPresent=*/true);
}

qint64 WorkerController::demoSlideCount() const {
    return static_cast<qint64>(openshow::scene::demoSlideCount(fDemoDocuments));
}

void WorkerController::selectDisplay(qint64 index) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest("output.select",
                boost::json::object{{"monitor", static_cast<std::int64_t>(index)}});
}

// --- media transport (task 6.15a, review R15) ---
//
// Every command follows the layer/toggle shape: send it, and let the
// status poll confirm it. The client adds no local optimism — a refused
// seek leaves mediaPosition on the real position and the slider, bound
// to it, snaps back with the next poll rather than hiding where the
// media actually is.

void WorkerController::mediaPlay() {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest(proto::kMethodMediaPlay, {});
}

void WorkerController::mediaPause() {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest(proto::kMethodMediaPause, {});
}

void WorkerController::mediaSeek(double seconds) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest(proto::kMethodMediaSeek,
                boost::json::object{{"position", seconds}});
}

void WorkerController::setMediaLoop(bool enabled) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest(proto::kMethodMediaLoop, boost::json::object{{"loop", enabled}});
}

void WorkerController::setMediaGain(const QString& source, double gain) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest(proto::kMethodMediaGain,
                boost::json::object{{"source", source.toStdString()}, {"gain", gain}});
}

void WorkerController::setMediaMute(const QString& source, bool muted) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest(proto::kMethodMediaMute,
                boost::json::object{{"source", source.toStdString()}, {"mute", muted}});
}

void WorkerController::selectStandaloneTrack(const QString& path) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest(proto::kMethodMediaStandalone,
                boost::json::object{{"path", path.toStdString()}});
}

std::int64_t WorkerController::sendRequest(const char* method, boost::json::object params,
                                          std::shared_ptr<void> intent) {
    // The transport decides whether this message can go on the wire at all: the
    // frame's size, the backlog the worker has not read, and how many replies it
    // already owes (task 6.8a/6.8b, review R11). A 0 here means it refused and
    // said why through statusMessage, and no request exists for the caller to
    // undo — which is why an intent handed to a refused send costs nothing.
    return fRpc.sendRequest(method, std::move(params), std::move(intent));
}

void WorkerController::sendNotification(const char* method, boost::json::object params) {
    fRpc.sendNotification(method, std::move(params));
}

void WorkerController::handleNotification(const boost::json::object& message) {
    const auto method = message.find("method");
    if (method == message.end() || !method->value().is_string()) {
        return;
    }
    const std::string_view name = method->value().as_string();
    if (name == proto::kMethodMediaError) {
        // Background media failure (task 5.5d): show it now rather than
        // at the next status poll; the poll then re-confirms (the worker's
        // status carries the same text until a success replaces it).
        if (message.contains("params") && message.at("params").is_object()) {
            const boost::json::object& params = message.at("params").as_object();
            const auto text = params.find("message");
            if (text != params.end() && text->value().is_string()) {
                fMediaError = QString::fromStdString(std::string(text->value().as_string()));
                emit mediaChanged();
            }
        }
        return;
    }
    if (name != proto::kMethodPreviewReady) {
        return;  // unknown notification: ignore, the protocol tolerates extensions
    }
    const auto paramsIt = message.find("params");
    if (paramsIt == message.end() || !paramsIt->value().is_object()) {
        return;
    }
    // A frame of the stream is the consumer's to judge: which target it is,
    // whether the ask it answers is still the newest one, what gets copied out
    // of the mapping and what is dropped. The slot it borrowed is released
    // there too — task 6.5's stream-identity rule travels with the copy, since
    // it is the reason a late release can never free a live stream's slot.
    fPreview->handleReady(paramsIt->value().as_object());
}

void WorkerController::shutdownNow() {
    if (fShutdownRequested) {
        return;
    }
    fShutdownRequested = true;
    if (fConnected && fHandshakeDone) {
        sendRequest("shutdown", {});
        fRpc.flush();
    }
    // Grace first: a healthy worker exits on its own the moment the
    // request lands. Anything left after two seconds is ours to kill.
    if (!fProcess.waitForFinished(2000)) {
        fProcess.terminate();
        if (!fProcess.waitForFinished(1000)) {
            fProcess.kill();
            fProcess.waitForFinished(1000);
        }
    }
    setStatus(QStringLiteral("worker stopped"));
}

void WorkerController::setStatus(const QString& text) {
    fStatusText = text;
    emit stateChanged();
}

}  // namespace openshow::ui
