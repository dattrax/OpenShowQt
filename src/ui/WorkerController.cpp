/*
 * WorkerController implementation. Notes:
 *
 * - The worker path comes from the build ($<TARGET_FILE>); packaging can
 *   point it next to the UI later (task 6.3).
 * - The socket, the framing and the reply ledger are RpcClient's (task 6.19a1).
 *   What is left here is the session: the process, the handshake and poll
 *   rhythm, the acknowledged output model, and the preview pictures.
 */

#include "WorkerController.h"

#include <QCoreApplication>
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
}

WorkerController::~WorkerController() {
    closePreviewSegment();
    if (fProcess.state() != QProcess::NotRunning) {
        fProcess.kill();
        fProcess.waitForFinished(1000);
    }
    QFile::remove(fRpc.socketPath());
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
    closePreviewSegment();
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
    closePreviewSegment();
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
    const std::shared_ptr<Ask> armed = pending.intentAs<Ask>();
    const Ask nothing;  // a request whose sender needed nothing remembered
    const Ask& intent = armed ? *armed : nothing;
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
        if (intent.thumbnailRequest != 0) {
            // A refused batch — no stream yet, an extent that could not fit a
            // slot, a generation the worker will not take — means the ask was
            // never MADE, so "asked" goes back to empty. The status poll's
            // stand-down rule then re-issues what the UI still wants once the
            // reason is gone, instead of the list believing it had asked for
            // pictures forever.
            fThumbnailAwaitAck = false;
            fThumbnailAskedPayload.clear();
        }
        if (intent.selectionRequest != 0) {
            // Task 6.9a's refusal is the same shape: an off-air prepare the
            // worker would not schedule was never MADE, so the dedupe that
            // remembers the asked payload must forget it — otherwise
            // re-selecting this very slide would silently do nothing, and
            // the canvas would wait forever on a frame nobody is coming.
            fSelectionAskedJson.clear();
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
        fSelectionCapable = false;
        fThumbnailCapable = false;
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
                    fSelectionCapable = true;
                } else if (capability.as_string() == "preview.thumbnail.v1") {
                    // The worker will render the slide list (task 6.16b).
                    // This capability is the whole negotiation: without it
                    // the list keeps the text line it already has, and with
                    // it the list gets the production compositor's own pixels
                    // at the size the rows are drawn. A worker too old for
                    // the method answers "method not found", which is why the
                    // list asks only when it is named.
                    fThumbnailCapable = true;
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
        // A new stream is a new generation domain (task 6.16a): the worker
        // has no selection registered on it, so whatever the editor was
        // looking at has to be asked for again — and the frame that answers
        // the old stream's ask is gone with it.
        fSelectionAskedJson.clear();
        fSelectionImage = QImage();
        fSelectionImageRequest = 0;
        fSelectionFrames = 0;
        if (!fSelectionWantedJson.isEmpty()) {
            const QString wanted = fSelectionWantedJson;
            requestSelectionPreview(wanted);
        }
        // The same reset for the list's target (task 6.16b): a new segment is
        // empty, the worker has no batch registered on it, and every id the
        // old ack named describes a frame that can never arrive. What the list
        // wants is re-asked as this stream's own first batch.
        fThumbnailAskedPayload.clear();
        fThumbnailAcked = 0;
        fThumbnailAwaitAck = false;
        fThumbnailIds.clear();
        fThumbnailKeys.clear();
        fThumbnailFrames = 0;
        clearThumbnailPictures();
        if (!fThumbnailWantedPayload.isEmpty()) {
            sendThumbnailBatch(fThumbnailWantedPayload, fThumbnailWantedWidth,
                               fThumbnailWantedHeight);
        }
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
        // The off-air ask (task 6.16a): this revision IS the picture the
        // editor asked for, so name it back with the generation the ask
        // carried. The worker refuses a generation that does not follow the
        // one it accepted — which is the rule that keeps a retried or
        // reordered prepare from putting an edit the operator abandoned back
        // on the canvas.
        if (intent.selectionRequest != 0) {
            sendRequest(proto::kMethodPreviewSelect,
                        boost::json::object{{"revision",
                                            static_cast<std::uint64_t>(prepared)},
                                           {"request", intent.selectionRequest}});
        }
        emit sceneChanged();
        return;
    }

    if (pending.method == proto::kMethodPreviewThumbnails) {
        // The ack for a band (task 6.16b, review R16): the per-slide ids its
        // frames will carry. They are installed from THIS request's reply and
        // only while it is still the newest ask this client made — an ack for
        // a band the operator has since scrolled past would otherwise map a
        // frame of nothing onto a row that is on screen.
        if (intent.thumbnailRequest != fThumbnailRequest) {
            return;  // superseded: the ids it names are already nobody's
        }
        fThumbnailAwaitAck = false;
        if (!payloadValue.is_object()) {
            return;
        }
        const boost::json::object& ack = payloadValue.as_object();
        const auto generation = ack.find("request");
        if (generation != ack.end()) {
            fThumbnailAcked = static_cast<std::uint32_t>(
                generation->value().to_number<std::uint64_t>());
        }
        fThumbnailIds.clear();
        fThumbnailKeys.clear();
        const auto frames = ack.find("frames");
        if (frames != ack.end() && frames->value().is_array()) {
            for (const auto& item : frames->value().as_array()) {
                if (!item.is_object()) {
                    continue;
                }
                const boost::json::object& slide = item.as_object();
                const auto key = slide.find("key");
                const auto id = slide.find("id");
                if (key == slide.end() || !key->value().is_string() || id == slide.end()) {
                    continue;
                }
                const QString slideKey =
                    QString::fromStdString(std::string(key->value().as_string()));
                const auto numericId =
                    static_cast<std::uint32_t>(id->value().to_number<std::uint64_t>());
                if (slideKey.isEmpty() || numericId == 0) {
                    continue;  // 0 is "no id", on every identity in this protocol
                }
                fThumbnailIds.insert(slideKey, numericId);
                fThumbnailKeys.insert(numericId, slideKey);
            }
        }
        // A refused entry is reported by the caller-facing status line rather
        // than silently: a slide whose scene does not compile is a document
        // problem the operator can act on, and the band's other pictures
        // arrive anyway.
        const auto refused = ack.find("refused");
        if (refused != ack.end() && refused->value().is_array() &&
            !refused->value().as_array().empty()) {
            setStatus(QStringLiteral("preview.thumbnails: %1 of %2 slides refused")
                          .arg(static_cast<qint64>(refused->value().as_array().size()))
                          .arg(static_cast<qint64>(fThumbnailIds.size() +
                                                   refused->value().as_array().size())));
        }
        emit previewChanged();
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
            // The off-air target's counters (task 6.16a). Optional, like
            // every newer field: a worker that predates the block keeps
            // whatever the last poll said rather than reading as zero.
            const auto selection = preview.find("selection");
            if (selection != preview.end() && selection->value().is_object()) {
                const boost::json::object& offAir = selection->value().as_object();
                const auto stale = offAir.find("stale");
                if (stale != offAir.end()) {
                    fSelectionStale =
                        static_cast<qint64>(stale->value().to_number<std::uint64_t>());
                }
                const auto active = offAir.find("active");
                if (active != offAir.end() && active->value().is_bool() &&
                    !active->value().as_bool() && !fSelectionAskedJson.isEmpty() &&
                    !fPendingPreviewName.empty()) {
                    // The worker has no target registered while this client
                    // believes it asked for one: a stand-down it made, or a
                    // stream that came up without the ask it was owed. The
                    // ask is re-issued rather than argued about, because the
                    // canvas showing yesterday's slide is the failure this
                    // whole task exists to prevent.
                    const QString wanted = fSelectionWantedJson;
                    fSelectionAskedJson.clear();
                    if (!wanted.isEmpty()) {
                        requestSelectionPreview(wanted);
                    }
                }
            }
            if (!preview.at("running").as_bool() && fMap) {
                closePreviewSegment();  // the worker retired the stream
                fPendingPreviewName.clear();
            }
            // The list's counters (task 6.16b), optional like every newer
            // field: a worker that predates the block keeps whatever the last
            // poll said rather than reading as zero.
            const auto thumbnail = preview.find("thumbnail");
            if (thumbnail != preview.end() && thumbnail->value().is_object()) {
                const boost::json::object& list = thumbnail->value().as_object();
                const auto stale = list.find("stale");
                if (stale != list.end()) {
                    fThumbnailStale =
                        static_cast<qint64>(stale->value().to_number<std::uint64_t>());
                }
                const auto active = list.find("active");
                // The re-issue rule for a batch, which is the selection's with
                // one difference: a list is a SET, so "the worker is not
                // showing what I last asked for" is the whole test. It waits
                // for no ack that is already on its way, and it never fires for
                // a list the operator is not looking at — an empty wanted
                // payload means exactly that. A worker that dropped the batch
                // (a stream that came up without it, a stand-down it made
                // itself, a refusal whose reply went missing) is answered by
                // asking again rather than argued about, because a slide list of
                // text lines is the thing this task exists to replace.
                const bool workerActive =
                    active != list.end() && active->value().is_bool() &&
                    active->value().as_bool();
                if (!fThumbnailWantedPayload.isEmpty() && !fThumbnailAwaitAck &&
                    !workerActive && !fPendingPreviewName.empty()) {
                    sendThumbnailBatch(fThumbnailWantedPayload, fThumbnailWantedWidth,
                                       fThumbnailWantedHeight);
                }
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

// --- the editor's off-air picture (task 6.16a, review R16) ---------------
//
// The edit canvas used to render its own text with QML, which meant the
// operator composed one picture and the audience saw another: Qt's wrapping,
// Qt's fit, Qt's fallbacks against Skia's. The fix is not a better QML
// approximation — it is the worker drawing the slide the operator picked,
// into the preview stream it already owns, without the audience being shown
// it. So this is a three-step conversation over the transport that exists:
// prepare (the store hands out a revision), preview.select (the render thread
// draws THAT revision off the air), preview.ready tagged with the generation
// (the picture arrives). Nothing here presents, restores or clears.
void WorkerController::requestSelectionPreview(const QString& sceneJson) {
    if (sceneJson.isEmpty()) {
        return;  // no slide: the caller stands the target down instead
    }
    // What the UI wants to see, remembered FIRST and even when this call
    // cannot ask for it: a worker that is still starting, a stream
    // mid-restart, and a capability that only appears at the handshake all
    // re-issue from here, which is why opening the edit page on a cold start
    // still ends with a picture rather than a placeholder forever.
    fSelectionWantedJson = sceneJson;
    if (!fHandshakeDone || !fSelectionCapable) {
        return;  // the canvas keeps the frame it last had, awaiting its host
    }
    if (sceneJson == fSelectionAskedJson) {
        return;  // the newest ask already carries this exact picture
    }
    boost::system::error_code parseError;
    const boost::json::value parsed =
        boost::json::parse(sceneJson.toStdString(), parseError);
    if (parseError || !parsed.is_object()) {
        setStatus(QStringLiteral("preview.select: the scene JSON is not readable"));
        return;
    }
    if (!sceneIsSendable(parsed, "preview.select")) {
        return;  // the reason is already in the status line
    }
    // Prepare first: an off-air scene is named by a store revision, and the
    // only way to have one is to hand the scene over. No armPrepare — that
    // mirrors the payload as a presentable scene, and this one is not being
    // presented; the marker below is what makes the ack answer with a
    // preview.select instead. The ask travels WITH the request rather than being
    // noted after it: the generation only exists once the socket carries it, and
    // the reply has to find the one it belongs to.
    auto ask = std::make_shared<Ask>();
    const std::int64_t id = sendRequest("scene.prepare", parsed.as_object(), ask);
    if (id == 0) {
        return;  // refused locally (backlog or in-flight bound); a later change re-asks
    }
    ++fSelectionRequest;
    ask->selectionRequest = fSelectionRequest;
    fSelectionAskedJson = sceneJson;
}

void WorkerController::clearSelectionPreview() {
    const bool hadAsk = !fSelectionAskedJson.isEmpty() || !fSelectionWantedJson.isEmpty();
    fSelectionWantedJson.clear();
    fSelectionAskedJson.clear();
    fSelectionImage = QImage();
    fSelectionImageRequest = 0;
    if (hadAsk) {
        emit previewChanged();
    }
    if (!fHandshakeDone || !fSelectionCapable || fPendingPreviewName.empty()) {
        return;  // no stream: there is no registered target to stand down
    }
    ++fSelectionRequest;  // a stand-down is a change, and changes advance
    sendRequest(proto::kMethodPreviewSelect,
                boost::json::object{{"revision", 0},
                                    {"request", fSelectionRequest}});
}

// --- the slide list's pictures (task 6.16b, review R16) ------------------
//
// Twenty slides used to be twenty lines of text, which is the same compromise
// the edit canvas used to make: the operator composes one picture in Skia and
// the list shows somebody else's approximation of it. The worker now renders
// the visible band itself — its compositor, its fonts, its backgrounds — at
// the size the rows are drawn, into the stream it already owns, as a third
// target, off the air and out of the scene store. One step rather than three:
// preview.thumbnails with the band's compiled scene text, an ack carrying one
// id per slide, and preview.ready frames tagged target=2 whose `revision` is
// that id. Pixels never ride the socket in either direction.
//
// The client's half of "this is a preview, not a second show" is the shape of
// its memory: the newest band is the only thing it wants, the ids live exactly
// as long as the batch that named them, and the pictures it has copied are
// bounded by count AND by bytes, so the cache is a number rather than a habit
// no one has measured.
QImage WorkerController::thumbnail(const QString& key) const {
    const auto entry = fThumbnailCache.constFind(key);
    return entry == fThumbnailCache.constEnd() ? QImage() : entry->image;
}

void WorkerController::storeThumbnail(const QString& key, QImage image) {
    // Arrival is the recency: a band scrolls one way, so the least recently
    // received picture is the one whose row is furthest off screen.
    dropThumbnailPicture(key);
    ThumbnailPicture picture;
    picture.image = std::move(image);
    picture.sourceJson = fThumbnailAskedScenes.value(key);
    fThumbnailBytes += picture.image.sizeInBytes();
    fThumbnailCache.insert(key, std::move(picture));
    fThumbnailOrder.push_back(key);
    ++fThumbnailFrames;
    // Bounded twice, because either bound alone is a leak with a good excuse:
    // a document of two thousand slides would grow a per-slide cache forever,
    // and a cache of "just" sixty-four pictures is unbounded if a slide is
    // ever asked for at 512x512. Whichever ceiling is hit first evicts.
    while (fThumbnailOrder.size() > static_cast<int>(kThumbnailCacheEntries) ||
           fThumbnailBytes > kThumbnailCacheBytes) {
        if (fThumbnailOrder.isEmpty()) {
            break;
        }
        dropThumbnailPicture(fThumbnailOrder.front());
    }
    emit thumbnailReady(key);
}

void WorkerController::dropThumbnailPicture(const QString& key) {
    const auto entry = fThumbnailCache.find(key);
    if (entry != fThumbnailCache.end()) {
        fThumbnailBytes -= entry->image.sizeInBytes();
        fThumbnailCache.erase(entry);
    }
    fThumbnailOrder.removeAll(key);
    if (fThumbnailBytes < 0) {
        fThumbnailBytes = 0;  // the counter cannot go under; a stale bookkeeping
                              // bug must not make it able to never evict again
    }
}

void WorkerController::clearThumbnailPictures() {
    fThumbnailCache.clear();
    fThumbnailOrder.clear();
    fThumbnailBytes = 0;
}

void WorkerController::requestThumbnails(const QVariantList& slides, qint64 width,
                                        qint64 height) {
    // Build the batch the worker will see, and record what the UI wants FIRST
    // and even when this call cannot send it: a worker still starting, a stream
    // mid-restart and a capability that only appears at the handshake all
    // re-issue from here, which is why opening the slide list on a cold start
    // ends with pictures rather than text lines forever.
    boost::json::array scenes;
    QHash<QString, QString> askedScenes;
    for (const QVariant& entry : slides) {
        const QVariantMap slide = entry.toMap();
        const QString key = slide.value(QStringLiteral("key")).toString();
        const QString sceneJson = slide.value(QStringLiteral("scene")).toString();
        if (key.isEmpty() || sceneJson.isEmpty()) {
            continue;
        }
        boost::system::error_code parseError;
        const boost::json::value parsed =
            boost::json::parse(sceneJson.toStdString(), parseError);
        if (parseError || !parsed.is_object()) {
            continue;  // a slide the compiler has not answered for
        }
        // R11's rule, applied to a BATCH: a slide this build already knows the
        // worker will refuse (authored text over the limit, a data: URI as an
        // asset reference) is dropped here with the reason in the status line,
        // and the rest of the band is asked for anyway. Sending it would buy
        // nothing — the worker refuses that entry too — and a band whose thirty
        // scenes happen to exceed one message would take its twenty-nine good
        // pictures down with it.
        if (!sceneIsSendable(parsed, "preview.thumbnails")) {
            continue;
        }
        // An edited slide's old picture is a lie about the content it is
        // listed under, so it goes now and the row falls back to its text line
        // until the worker's new render lands. An unchanged slide keeps its
        // picture: the ask below is deduplicated, so re-listing a band nobody
        // edited costs nobody anything.
        const auto cached = fThumbnailCache.constFind(key);
        if (cached != fThumbnailCache.constEnd() && cached->sourceJson != sceneJson) {
            dropThumbnailPicture(key);
        }
        boost::json::object item;
        item.emplace("key", key.toStdString());
        item.emplace("scene", parsed);
        scenes.push_back(std::move(item));
        askedScenes.insert(key, sceneJson);
    }
    if (scenes.size() > proto::kMaxThumbnailBatch) {
        // The cap is the bound on what one ask can cost the worker, so it is
        // judged here as well as there. Refusing with the reason beats drawing
        // the first thirty of sixty and letting the list believe it had
        // pictures for all sixty.
        setStatus(QStringLiteral("preview.thumbnails takes %1 slides, asked for %2 - ask "
                                 "for the visible band")
                      .arg(static_cast<qint64>(proto::kMaxThumbnailBatch))
                      .arg(static_cast<qint64>(scenes.size())));
        return;
    }
    const QString payload = QString::fromStdString(boost::json::serialize(scenes));
    fThumbnailWantedPayload = payload;
    fThumbnailWantedWidth = width;
    fThumbnailWantedHeight = height;
    if (!fHandshakeDone || !fThumbnailCapable) {
        return;  // the rows keep their text lines, awaiting the capability
    }
    if (payload == fThumbnailAskedPayload && width == fThumbnailAskedWidth &&
        height == fThumbnailAskedHeight && fThumbnailAcked == fThumbnailRequest &&
        !fThumbnailAwaitAck) {
        return;  // the newest ask already carries exactly this band
    }
    fThumbnailAskedScenes = askedScenes;
    sendThumbnailBatch(payload, width, height);
}

void WorkerController::sendThumbnailBatch(const QString& scenesPayload, qint64 width,
                                          qint64 height) {
    if (!fHandshakeDone || !fThumbnailCapable) {
        return;
    }
    if (fPendingPreviewName.empty()) {
        return;  // no stream: there is nowhere for these pixels to go
    }
    // The extent is judged against the segment this client mapped, not against
    // a box: a frame's pixels live in a slot reserved for the stream's own
    // picture, so a size legal on a 1024x576 stream is impossible on a
    // 320x180 one. The worker refuses the same way (the same helper decides
    // both), and asking first puts the reason on the operator's status line
    // rather than in a socket error.
    if (!proto::thumbnailExtentFits(static_cast<std::uint32_t>(std::max<qint64>(0, width)),
                                    static_cast<std::uint32_t>(std::max<qint64>(0, height)),
                                    static_cast<std::uint32_t>(fPreviewSlotStride))) {
        // One predicate, and one sentence that names both of its halves: the
        // agreed box AND this stream's slot. A 16x16 ask and a 512x100 ask on a
        // small stream fail for different reasons, and a status line that
        // blamed the slot for the first would send an operator to change the
        // wrong number.
        setStatus(QStringLiteral("preview.thumbnails: %1x%2 is outside %3..%4 pixels, or does "
                                 "not fit the %5-byte preview slot this stream was mapped with")
                      .arg(width)
                      .arg(height)
                      .arg(static_cast<qint64>(proto::kMinThumbnailDimension))
                      .arg(static_cast<qint64>(proto::kMaxThumbnailDimension))
                      .arg(static_cast<qulonglong>(fPreviewSlotStride)));
        return;
    }
    boost::system::error_code parseError;
    const boost::json::value scenes =
        boost::json::parse(scenesPayload.toStdString(), parseError);
    if (parseError || !scenes.is_array()) {
        setStatus(QStringLiteral("preview.thumbnails: the band is not readable"));
        return;
    }
    // THE GENERATION, the batch's own counter: every ask the socket carries
    // advances it, and the worker takes only one strictly later than what it
    // accepted. That is what makes a duplicated scroll handler, a retried
    // request and a replayed frame all the same non-event.
    ++fThumbnailRequest;
    auto ask = std::make_shared<Ask>();
    boost::json::object params{{"scenes", scenes},
                               {"width", static_cast<std::int64_t>(width)},
                               {"height", static_cast<std::int64_t>(height)},
                               {"request", fThumbnailRequest}};
    const std::int64_t id =
        sendRequest(proto::kMethodPreviewThumbnails, std::move(params), ask);
    if (id == 0) {
        return;  // refused locally; the next change re-asks
    }
    ask->thumbnailRequest = fThumbnailRequest;
    fThumbnailAskedPayload = scenesPayload;
    fThumbnailAskedWidth = width;
    fThumbnailAskedHeight = height;
    fThumbnailAwaitAck = true;
}

void WorkerController::clearThumbnails() {
    const bool hadAsk = !fThumbnailAskedPayload.isEmpty() || !fThumbnailWantedPayload.isEmpty();
    const qint64 width = fThumbnailWantedWidth;
    const qint64 height = fThumbnailWantedHeight;
    // Nothing is wanted, nothing is owed, and nothing is kept: a stand-down
    // that left thirty pictures in the cache would be a list that never stops
    // costing memory the moment it stops being looked at.
    fThumbnailWantedPayload.clear();
    fThumbnailAskedPayload.clear();
    fThumbnailAskedScenes.clear();
    fThumbnailIds.clear();
    fThumbnailKeys.clear();
    fThumbnailAwaitAck = false;
    clearThumbnailPictures();
    if (hadAsk) {
        emit previewChanged();
    }
    if (!hadAsk || !fHandshakeDone || !fThumbnailCapable || fPendingPreviewName.empty()) {
        return;  // never asked: there is no registered batch to stand down
    }
    // A stand-down is a change, so it advances the generation like any other
    // ask, and it travels with the extent the batch had: the worker judges the
    // extent before the batch, and an empty list with an extent it cannot
    // describe would be refused rather than obeyed.
    sendThumbnailBatch(QStringLiteral("[]"), width == 0 ? 160 : width,
                       height == 0 ? 90 : height);
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
        const auto ask = request.intentAs<Ask>();
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
    auto ask = entry->intentAs<Ask>();
    if (!ask) {
        ask = std::make_shared<Ask>();
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
    // Whose picture this is, and which ask answers it (task 6.16a). A worker
    // too old to send the fields means the audience, which is what version 1
    // of this stream could only ever carry.
    const std::uint64_t target = number("target");
    const std::uint32_t request = static_cast<std::uint32_t>(number("request"));
    const bool forSelection = target == proto::kPreviewTargetSelection;
    // The list's target (task 6.16b). A thumbnail's `revision` is not a scene
    // revision at all — it is the id the ack gave this slide, because a scene
    // the store has never held has no other name.
    const bool forThumbnail = target == proto::kPreviewTargetThumbnail;
    const std::uint32_t frameId = static_cast<std::uint32_t>(number("revision"));
    // THE CLIENT'S GENERATION GATE, the consumer's half of the rule. A frame
    // answering an ask this client has already superseded is yesterday's edit
    // or last scroll's band: it is released like any other slot and never
    // painted, because putting it under the handles of the slide now being
    // edited — or under a row the operator has since scrolled past — is
    // precisely the failure R16 reported.
    const bool superseded = (forSelection && request != fSelectionRequest) ||
                            (forThumbnail && request != fThumbnailAcked);
    // A thumbnail whose id no accepted batch named describes a slide this
    // client does not have on its list: an id from a band that has since been
    // replaced, or from a batch whose ack has not arrived yet. Either way
    // there is no row to put it in, and guessing from the order frames
    // arrived in is how a picture ends up on the wrong slide.
    const bool unmapped = forThumbnail && !fThumbnailKeys.contains(frameId);

    // Copy the payload under the slot's seqlock: same sequence before and
    // after means the producer never touched it mid-copy. A stale or torn
    // frame is simply skipped; the next notification brings a newer one.
    bool fresh = false;
    if (!superseded && !unmapped && fMap && slot < fPreviewSlotCount &&
        rowBytes == width * 4 && width > 0 && height > 0 && width <= 4096 &&
        height <= 4096 &&
        sizeof(proto::PreviewSlotHeader) + rowBytes * height <= fPreviewSlotStride) {
        const std::uint8_t* slotBase =
            fMap + proto::previewSlotOffset(static_cast<std::uint32_t>(slot),
                                            fPreviewSlotStride);
        const auto* header =
            reinterpret_cast<const proto::PreviewSlotHeader*>(slotBase);
        const std::uint64_t seqBefore =
            std::atomic_ref<const std::uint64_t>(header->seq).load();
        // The slot's own geometry, cross-checked against the notification
        // (task 6.16b): one stream now carries frames of DIFFERENT sizes —
        // the mirror at the stream's extent, a thumbnail at the band's — and
        // the copy below is bounded by the notification's numbers. If the
        // header the same sequence stamps disagrees, the two are not the same
        // frame, and the safe answer is to copy nothing.
        const bool geometryAgrees = header->width == width && header->height == height &&
                                    header->rowBytes == rowBytes;
        if (seqBefore == seq && (seqBefore & 1) == 0 && geometryAgrees) {
            QImage image(static_cast<int>(width), static_cast<int>(height),
                         QImage::Format_RGBA8888);
            if (!image.isNull()) {
                std::memcpy(image.bits(), slotBase + sizeof(proto::PreviewSlotHeader),
                            static_cast<std::size_t>(rowBytes) * height);
                const std::uint64_t seqAfter =
                    std::atomic_ref<const std::uint64_t>(header->seq).load();
                if (seqAfter == seqBefore) {
                    if (forThumbnail) {
                        const QString key = fThumbnailKeys.value(frameId);
                        // The key can only have disappeared between the two
                        // checks above and here if a batch was accepted
                        // mid-copy, on this same thread — which it cannot be.
                        // Belt: a frame with nowhere to go is dropped.
                        if (!key.isEmpty()) {
                            storeThumbnail(key, std::move(image));
                            fresh = true;
                        }
                    } else if (forSelection) {
                        fSelectionImage = std::move(image);
                        fSelectionImageRequest = request;
                        ++fSelectionFrames;
                    } else {
                        fPreviewImage = std::move(image);
                        ++fPreviewFrames;
                    }
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
    // The off-air frame lived in this mapping (task 6.16a). Dropping it is
    // not housekeeping: a picture from a retired stream, still on the canvas
    // while the operator edits something else, is exactly the stale imagery
    // the generation rule is designed to make impossible — so it goes now,
    // and the ask goes with it. What the UI WANTS is kept: the next stream
    // re-issues it as its own first ask.
    fSelectionImage = QImage();
    fSelectionImageRequest = 0;
    fSelectionAskedJson.clear();
    // The list's pictures came out of this mapping too, and so does their
    // right to be shown (task 6.16b). The ids go with them: a frame of the
    // retired stream can arrive with a sequence that still looks good, and the
    // only thing that says which slide it belongs to is the batch that named
    // it. What the UI WANTS is kept, as above — the next stream re-issues.
    clearThumbnailPictures();
    fThumbnailIds.clear();
    fThumbnailKeys.clear();
    fThumbnailAskedPayload.clear();
    fThumbnailAwaitAck = false;
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
