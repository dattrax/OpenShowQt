/*
 * WorkerController implementation. Notes:
 *
 * - The worker path comes from the build ($<TARGET_FILE>); packaging can
 *   point it next to the UI later (task 6.3).
 * - The socket lives under XDG_RUNTIME_DIR with the UI pid, so two UIs
 *   never collide and bind has a fresh path (the worker also unlinks a
 *   stale one).
 * - QLocalSocket with a path containing '/' connects to exactly the file
 *   the worker bound, using the same framing both sides share.
 */

#include "WorkerController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace openshow::ui {

namespace proto = openshow::protocol;

namespace {

constexpr int kConnectRetryMs = 100;
constexpr int kStatusPollMs = 500;
// Revisions of prepared-scene JSON kept as a mirror for the clear
// controls (task 3.4). Bounded; the worker prunes its own store too.
constexpr int kSceneMirrorSize = 32;

}  // namespace

WorkerController::WorkerController(QObject* parent) : QObject(parent) {
    fSocketPath = QDir(qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath()))
                      .filePath(QStringLiteral("openshow-ui-%1.sock")
                                    .arg(QCoreApplication::applicationPid()));

    connect(&fSocket, &QLocalSocket::connected, this, &WorkerController::onSocketConnected);
    connect(&fSocket, &QLocalSocket::disconnected, this,
            &WorkerController::onSocketDisconnected);
    connect(&fSocket, &QLocalSocket::readyRead, this, &WorkerController::onSocketReady);
    // The worker binds its socket a moment after we spawn it, so the first
    // connect attempts fail; retry on a short timer until it answers, the
    // worker dies, or patience runs out.
    fConnectRetry.setInterval(kConnectRetryMs);
    connect(&fConnectRetry, &QTimer::timeout, this, &WorkerController::tryConnect);
    connect(&fProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setStatus(QStringLiteral("worker failed to start"));
        }
    });
    connect(&fProcess, &QProcess::finished, this, &WorkerController::onProcessFinished);
}

WorkerController::~WorkerController() {
    closePreviewSegment();
    if (fProcess.state() != QProcess::NotRunning) {
        fProcess.kill();
        fProcess.waitForFinished(1000);
    }
    QFile::remove(fSocketPath);
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
    QStringList arguments{QStringLiteral("--socket"), fSocketPath};
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
    fConnectAttempts = 0;
    fConnectRetry.start();
    tryConnect();
}

void WorkerController::tryConnect() {
    const QLocalSocket::LocalSocketState state = fSocket.state();
    if (state == QLocalSocket::ConnectedState || state == QLocalSocket::ConnectingState) {
        return;
    }
    if (fShutdownRequested || fProcess.state() == QProcess::NotRunning) {
        fConnectRetry.stop();
        return;
    }
    if (++fConnectAttempts > 100) {  // ten seconds of patience
        fConnectRetry.stop();
        setStatus(QStringLiteral("could not reach the worker on %1").arg(fSocketPath));
        return;
    }
    fSocket.connectToServer(fSocketPath);
}

void WorkerController::onSocketConnected() {
    fConnectRetry.stop();
    fConnected = true;
    // A fresh connection owns a fresh conversation: no request of the old
    // one may be answered, and no half-read frame of the old stream may
    // join the new one (task 6.3a).
    ++fConnectionGeneration;
    fDecoder.reset();
    fPendingRequests.clear();
    setStatus(QStringLiteral("handshaking..."));
    boost::json::object params = proto::helloParams("openshow-ui", proto::kProtocolMajor,
                                                    proto::kProtocolMinor);
    sendRequest("hello", std::move(params));
    emit stateChanged();
}

void WorkerController::onSocketDisconnected() {
    fConnected = false;
    fHandshakeDone = false;
    closePreviewSegment();
    resetSceneTracking();
    fMediaPlaying = false;
    fMediaError.clear();  // the media state describes a worker we lost
    emit mediaChanged();
    emit stateChanged();
    if (!fShutdownRequested) {
        setStatus(QStringLiteral("worker connection lost"));
    }
}

void WorkerController::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    fConnected = false;
    fOutputStopped = true;
    closePreviewSegment();
    resetSceneTracking();
    fMediaPlaying = false;
    fMediaError.clear();
    emit mediaChanged();
    emit stateChanged();
    if (fShutdownRequested) {
        setStatus(QStringLiteral("worker stopped (exit %1)").arg(exitCode));
    } else if (exitStatus == QProcess::CrashExit) {
        setStatus(QStringLiteral("worker crashed"));
    } else {
        setStatus(QStringLiteral("worker exited (%1)").arg(exitCode));
    }
}

void WorkerController::onSocketReady() {
    const QByteArray incoming = fSocket.readAll();
    fDecoder.feed(std::string_view(incoming.constData(),
                                   static_cast<std::size_t>(incoming.size())));
    drainMessages();
}

void WorkerController::drainMessages() {
    boost::json::value message;
    std::string error;
    while (fDecoder.next(&message, &error)) {
        if (!message.is_object()) {
            continue;
        }
        if (message.as_object().contains("method")) {  // worker-originated
            handleNotification(message.as_object());
        } else {
            handleReply(message.as_object());
        }
    }
    if (fDecoder.failed()) {
        setStatus(QStringLiteral("worker broke the protocol: %1")
                      .arg(QString::fromStdString(error)));
        fSocket.disconnectFromServer();
    }
}

void WorkerController::handleReply(const boost::json::object& message) {
    // Reply correlation (task 6.3a, review R3): a reply is only what the
    // request its id names said it would be. Shape-matching replies let a
    // burst of prepares misattribute payloads and fire stale auto-presents.
    const auto idEntry = message.find("id");
    if (idEntry == message.end() ||
        !(idEntry->value().is_int64() || idEntry->value().is_uint64())) {
        return;  // a reply with no usable id answers nothing
    }
    const std::int64_t id = idEntry->value().to_number<std::int64_t>();
    const auto pendingEntry = fPendingRequests.find(id);
    if (pendingEntry == fPendingRequests.end()) {
        return;  // duplicate, superseded, or from a previous connection
    }
    const PendingRequest pending = pendingEntry.value();
    fPendingRequests.erase(pendingEntry);
    if (pending.connection != fConnectionGeneration) {
        return;  // written on a connection that is gone
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
        if (payload.contains("capabilities") && payload.at("capabilities").is_array()) {
            for (const auto& capability : payload.at("capabilities").as_array()) {
                if (capability.is_string() &&
                    capability.as_string() == "preview.shm.v1") {
                    previewCapable = true;
                    break;
                }
            }
        }
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
        if (!payload.contains("bytesPerSlot")) {
            return;
        }
        // Do not map yet: the segment exists once the render thread has
        // acted, and the first preview.ready proves it did.
        closePreviewSegment();
        fPreviewSlotCount = payload.at("slotCount").to_number<std::uint64_t>();
        fPreviewSlotStride = payload.at("bytesPerSlot").to_number<std::uint64_t>();
        fPendingPreviewName = std::string(payload.at("name").as_string());
        fPreviewFrames = 0;
        fPreviewError.clear();
        emit previewChanged();
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
        if (!pending.prepareJson.isEmpty()) {
            fSceneJson.insert(prepared, pending.prepareJson);
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
        if (pending.autoPresent && id == fLatestGoId) {
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
        emit sceneChanged();
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
        if (payload.contains("preview") && payload.at("preview").is_object()) {
            const boost::json::object& preview = payload.at("preview").as_object();
            fPreviewDropped =
                static_cast<qint64>(preview.at("dropped").to_number<std::uint64_t>());
            fPreviewError = QString::fromStdString(
                std::string(preview.at("error").as_string()));
            if (!preview.at("running").as_bool() && fMap) {
                closePreviewSegment();  // the worker retired the stream
                fPendingPreviewName.clear();
            }
            emit previewChanged();
        }
        // Background media (task 5.5d). Optional: a worker predating the
        // media object keeps whatever the last notification said rather
        // than falsely clearing it.
        if (payload.contains("media") && payload.at("media").is_object()) {
            const boost::json::object& media = payload.at("media").as_object();
            fMediaPlaying = media.at("playing").as_bool();
            fMediaError =
                QString::fromStdString(std::string(media.at("error").as_string()));
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
    if (fConnected && fHandshakeDone) {
        sendRequest("output.status", {});
    }
}

void WorkerController::setLayerEnabled(const QString& name, bool enabled) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest("layer.setEnabled",
                boost::json::object{{"layer", name.toStdString()},
                                    {"enabled", enabled}});
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
    // The scene object itself is the params (the worker validates it).
    const std::int64_t id = sendRequest("scene.prepare", parsed.as_object());
    armPrepare(id, parsed.as_object(), /*autoPresent=*/false);
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
    // New content is a fresh go: whatever a clear sequence kept for
    // restore is history now (the operator moved on).
    fRestoreJson.clear();
    fRestoreArmedGo = 0;
    fRestoreDoneRevision = 0;
    presentSceneInternal(sceneJson);
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
    const auto entry = fPendingRequests.find(requestId);
    if (entry == fPendingRequests.end()) {
        return;  // sendRequest always records the entry; belt and braces
    }
    entry->prepareJson = QString::fromStdString(boost::json::serialize(scene));
    entry->autoPresent = autoPresent;
    if (autoPresent) {
        // This go supersedes any older one still awaiting its ack; only
        // the newest go's acknowledgement moves the audience. Its payload
        // is the pending logical presentation the clears compose against
        // (task 6.3b).
        fDesiredSceneJson = entry->prepareJson;
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
    // arrive for them, and none must survive to act on a later one.
    fPendingRequests.clear();
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

std::int64_t WorkerController::sendRequest(const char* method, boost::json::object params) {
    // Record before writing: the reply to this id must find its request,
    // tagged with the connection it belongs to (task 6.3a).
    const std::int64_t id = fNextId++;
    PendingRequest pending;
    pending.method = method;
    pending.connection = fConnectionGeneration;
    fPendingRequests.insert(id, std::move(pending));
    std::string wire;
    proto::appendFrame(proto::makeRequest(id, method, std::move(params)), &wire);
    fSocket.write(wire.data(), static_cast<qint64>(wire.size()));
    return id;
}

void WorkerController::sendNotification(const char* method, boost::json::object params) {
    std::string wire;
    proto::appendFrame(proto::makeNotification(method, std::move(params)), &wire);
    fSocket.write(wire.data(), static_cast<qint64>(wire.size()));
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
    const boost::json::object& params = paramsIt->value().as_object();

    // Stream identity (task 6.5, review R5): a ready naming anything
    // other than the stream this client currently owns describes slots
    // of a segment it does not have — ignore it entirely, copying nothing
    // and releasing nothing (a delayed release of an old stream must
    // never free a live stream's slot).
    const auto streamIt = params.find("stream");
    if (streamIt == params.end() || !streamIt->value().is_string() ||
        std::string_view(streamIt->value().as_string()) != fPendingPreviewName) {
        return;
    }

    // First ready proves the segment exists: this is where a pending
    // (not-yet-mapped) stream gets opened, retrying if we beat the worker.
    if (fMap == nullptr && !fPendingPreviewName.empty()) {
        openPreviewSegment(fPendingPreviewName, fPreviewSlotStride);
    }

    auto number = [&params](const char* key) -> std::uint64_t {
        const auto entry = params.find(key);
        return entry == params.end() ? 0 : entry->value().to_number<std::uint64_t>();
    };
    const std::uint64_t slot = number("slot");
    const std::uint64_t seq = number("seq");
    const std::uint64_t width = number("width");
    const std::uint64_t height = number("height");
    const std::uint64_t rowBytes = number("rowBytes");

    // Copy the payload under the slot's seqlock: same sequence before and
    // after means the producer never touched it mid-copy. A stale or torn
    // frame is simply skipped; the next notification brings a newer one.
    bool fresh = false;
    if (fMap && slot < fPreviewSlotCount && rowBytes == width * 4 && width > 0 &&
        height > 0 && width <= 4096 && height <= 4096 &&
        sizeof(proto::PreviewSlotHeader) + rowBytes * height <= fPreviewSlotStride) {
        const std::uint8_t* slotBase =
            fMap + proto::previewSlotOffset(static_cast<std::uint32_t>(slot),
                                            fPreviewSlotStride);
        const auto* header =
            reinterpret_cast<const proto::PreviewSlotHeader*>(slotBase);
        const std::uint64_t seqBefore =
            std::atomic_ref<const std::uint64_t>(header->seq).load();
        if (seqBefore == seq && (seqBefore & 1) == 0) {
            QImage image(static_cast<int>(width), static_cast<int>(height),
                         QImage::Format_RGBA8888);
            if (!image.isNull()) {
                std::memcpy(image.bits(), slotBase + sizeof(proto::PreviewSlotHeader),
                            static_cast<std::size_t>(rowBytes) * height);
                const std::uint64_t seqAfter =
                    std::atomic_ref<const std::uint64_t>(header->seq).load();
                if (seqAfter == seqBefore) {
                    fPreviewImage = image;
                    ++fPreviewFrames;
                    fresh = true;
                }
            }
        }
    }

    // Ownership of the slot ends whether or not this copy worked out; the
    // producer drops rather than waits, but there is no reason to make it.
    // The echo carries the stream identity the ready came with (task 6.5).
    sendNotification(proto::kMethodPreviewReleased,
                     boost::json::object{{"slot", static_cast<std::int64_t>(slot)},
                                         {"seq", static_cast<std::int64_t>(seq)},
                                         {"stream", fPendingPreviewName}});
    if (fresh) {
        emit previewChanged();
    }
}

void WorkerController::openPreviewSegment(const std::string& name,
                                          std::uint64_t bytesPerSlot) {
    closePreviewSegment();
    fPreviewSlotStride = bytesPerSlot;

    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        fPreviewError = QStringLiteral("shm_open %1 failed").arg(
            QString::fromStdString(name));
        emit previewChanged();
        return;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 ||
        info.st_size <
            static_cast<off_t>(proto::previewSegmentBytes(0, 0))) {
        ::close(fd);
        fPreviewError = QStringLiteral("preview segment has a bad size");
        emit previewChanged();
        return;
    }
    const std::uint64_t total = fPreviewSlotStride * fPreviewSlotCount + 64;
    if (info.st_size < static_cast<off_t>(total)) {
        ::close(fd);
        fPreviewError = QStringLiteral("preview segment is smaller than promised");
        emit previewChanged();
        return;
    }
    void* map = ::mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        fPreviewError = QStringLiteral("mmap %1 failed").arg(
            QString::fromStdString(name));
        emit previewChanged();
        return;
    }

    const auto* segment = static_cast<const proto::PreviewSegmentHeader*>(map);
    if (segment->magic != proto::kPreviewMagic ||
        segment->layoutVersion != proto::kPreviewLayoutVersion ||
        segment->slotCount != fPreviewSlotCount ||
        segment->bytesPerSlot != bytesPerSlot) {
        ::munmap(map, total);
        ::close(fd);
        fPreviewError = QStringLiteral("preview segment header mismatch");
        emit previewChanged();
        return;
    }

    fMap = static_cast<std::uint8_t*>(map);
    fMapFd = fd;
    fMapBytes = total;
    fPreviewError.clear();
    emit previewChanged();
}

void WorkerController::closePreviewSegment() {
    if (fMap) {
        ::munmap(fMap, fMapBytes);
        fMap = nullptr;
        fMapBytes = 0;
    }
    if (fMapFd >= 0) {
        ::close(fMapFd);
        fMapFd = -1;
    }
}

void WorkerController::shutdownNow() {
    if (fShutdownRequested) {
        return;
    }
    fShutdownRequested = true;
    if (fConnected && fHandshakeDone) {
        sendRequest("shutdown", {});
        fSocket.flush();
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
