/*
 * WorkerController: the frontend side of task 1.2 supervision.
 *
 * Spawns openshow-worker --socket PATH, connects with QLocalSocket, runs
 * the JSON-RPC handshake through the shared openshow::protocol framing,
 * and keeps the QML status properties updated with output.status polls.
 * The same session operations the UI calls are what the RPC exposes:
 * layer toggles now, scenes later.
 *
 * Lifecycle per PLAN.md: the worker lives only while connected. Closing
 * the UI sends "shutdown"; a worker that dies or hangs anyway is
 * terminated, and its state is reported honestly.
 */

#pragma once

#include <boost/json.hpp>

#include <QHash>
#include <QImage>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

#include <chrono>
#include <cstdint>

#include "openshow/protocol/preview_wire.h"
#include "openshow/protocol/protocol.h"
#include "openshow/scene/demo.h"

#include "SceneEdit.h"

namespace openshow::ui {

class WorkerController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY stateChanged)
    Q_PROPERTY(bool outputLive READ outputLive NOTIFY stateChanged)
    Q_PROPERTY(qint64 frames READ frames NOTIFY outputStatusChanged)
    Q_PROPERTY(qint64 revision READ revision NOTIFY outputStatusChanged)
    Q_PROPERTY(QString device READ device NOTIFY outputStatusChanged)
    Q_PROPERTY(QVariantMap layers READ layers NOTIFY outputStatusChanged)
    // Destination control (task 1.3): -1 windowed, >=0 display index, plus
    // the enumerated panels for the picker.
    Q_PROPERTY(qint64 currentMonitor READ currentMonitor NOTIFY outputStatusChanged)
    Q_PROPERTY(QVariantList displays READ displays NOTIFY displaysChanged)
    // Preview (task 1.4): frames arrive through shared memory the worker
    // names; previewDropped is the worker's own drop counter, so a stalled
    // consumer is visible honestly.
    Q_PROPERTY(bool previewActive READ previewActive NOTIFY previewChanged)
    Q_PROPERTY(qint64 previewFrames READ previewFrames NOTIFY previewChanged)
    Q_PROPERTY(qint64 previewDropped READ previewDropped NOTIFY previewChanged)
    Q_PROPERTY(QString previewError READ previewError NOTIFY previewChanged)
    // Background media (task 5.5d): the worker's video-background transport
    // state. mediaError is the last failure for the on-air background and
    // stays set until a successful open replaces it (the worker carries it
    // in status and pushes the same text as a media.error notification).
    // mediaAudio (task 6.7c) is the typed audible lane — "none", "silent",
    // "no-output", "degraded", "paused" or "live" — so a clip that is on
    // air with sound the audience cannot hear is visible as something other
    // than a green light.
    Q_PROPERTY(bool mediaPlaying READ mediaPlaying NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaError READ mediaError NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaAudio READ mediaAudio NOTIFY mediaChanged)
    // Media transport (task 6.15a, review R15): the operator's playback
    // surface. mediaOnAir is the honest enablement — the worker says a
    // transport is live, not the hopeful click (path would lie: it stays
    // set with a sticky error). Position and duration are seconds (the
    // worker reports -1 for unknown), loop is the intent the end of the
    // clip will honour, and mediaSources lists the registered audio
    // identities as [{id, gain, muted}] — a source the transport does
    // not have is absent, never falsely listed at default gain.
    Q_PROPERTY(bool mediaOnAir READ mediaOnAir NOTIFY mediaChanged)
    Q_PROPERTY(double mediaPosition READ mediaPosition NOTIFY mediaChanged)
    Q_PROPERTY(double mediaDuration READ mediaDuration NOTIFY mediaChanged)
    Q_PROPERTY(bool mediaLoop READ mediaLoop NOTIFY mediaChanged)
    Q_PROPERTY(QVariantList mediaSources READ mediaSources NOTIFY mediaChanged)
    // Scene store (task 2.4c): what the worker has applied and is about
    // to apply, and how many prepared scenes it keeps. demoSlideCount is
    // the fixture's size, fixed for the session.
    Q_PROPERTY(qint64 sceneActive READ sceneActive NOTIFY sceneChanged)
    Q_PROPERTY(qint64 scenePending READ scenePending NOTIFY sceneChanged)
    Q_PROPERTY(qint64 scenePrepared READ scenePrepared NOTIFY sceneChanged)
    Q_PROPERTY(qint64 demoSlideCount READ demoSlideCount CONSTANT)
    // Output controls (task 3.4): the clear buttons follow acknowledged
    // state. The flags describe the scene the worker reports active -
    // derived from the JSON that revision was prepared with, never from a
    // hopeful click - and canRestore holds the clear's snapshot until the
    // restore itself is on air, so the button cannot lie about output it
    // does not yet have.
    Q_PROPERTY(bool wordsOnAir READ wordsOnAir NOTIFY sceneChanged)
    Q_PROPERTY(bool backgroundOnAir READ backgroundOnAir NOTIFY sceneChanged)
    Q_PROPERTY(bool canRestore READ canRestore NOTIFY sceneChanged)

public:
    explicit WorkerController(QObject* parent = nullptr);
    ~WorkerController() override;

    // Spawns the worker and starts connecting. Called once at startup.
    void start();

    // Send "shutdown" and reap; escalating to terminate/kill if the
    // worker ignores its frontend. Connected to the UI window closing.
    Q_INVOKABLE void shutdownNow();

    // One output.status round trip; also drives the periodic poll.
    // Move the audience output (-1 windowed, >=0 display index).
    Q_INVOKABLE void selectDisplay(qint64 index);

    Q_INVOKABLE void setLayerEnabled(const QString& name, bool enabled);

    // Scenes (task 2.4c): prepare a scene object (the JSON the frontend's
    // compiler emitted), present a prepared revision, or compile one of
    // the demo fixture's slides and present it in one step — the same
    // compileSlide path real content will take, so the demo exercises
    // exactly the production code path.
    Q_INVOKABLE void prepareScene(const QString& sceneJson);
    Q_INVOKABLE void presentScene(qint64 revision);
    Q_INVOKABLE void presentDemoSlide(qint64 index);
    // Prepare compiled scene JSON and present it the moment the worker
    // acknowledges (task 3.3: the lyric panel presents real slides with
    // one call; the revision, as ever, is the worker's to hand out).
    Q_INVOKABLE void presentSceneJson(const QString& sceneJson);

    // Media transport (task 6.15a, review R15): play/pause/seek/loop/
    // gain/mute as operator operations over the socket. Each call is a
    // fire-and-confirm request — the reply says the worker took it, the
    // media* properties then say what the audience has (the 3.4 rule:
    // controls bind to worker-reported state, never to the click).
    Q_INVOKABLE void mediaPlay();
    Q_INVOKABLE void mediaPause();
    Q_INVOKABLE void mediaSeek(double seconds);
    Q_INVOKABLE void setMediaLoop(bool enabled);
    Q_INVOKABLE void setMediaGain(const QString& source, double gain);
    Q_INVOKABLE void setMediaMute(const QString& source, bool muted);

    // Adoption after a save moves an asset's bytes (task 6.12, review R7).
    // pathMap names each move old-path -> new-path. Every scene payload this
    // controller still holds — the revision mirror, the retained restore
    // snapshot, the pending logical presentation, and a prepare still
    // awaiting its ack — has background.assetPath rewritten where it names
    // an old path, and the scene owed to the output is RE-PREPARED so the
    // worker opens the new file before the next go. It is deliberately
    // never re-presented: presenting again would restart a fade the
    // audience is watching, and the worker keeps the on-air old path pinned
    // (path-keyed) until the next present takes the new one.
    Q_INVOKABLE void adoptAssetPaths(const QVariantMap& pathMap);

    // Output controls (task 3.4, hardened by task 6.3b). Each clear is a
    // derived presentation: the pending logical scene (the newest go's
    // payload — not merely the last confirmed status, so back-to-back
    // clears compose) with one layer's content removed. The scene the
    // first clear of a sequence cut from is retained as CONTENT, not just
    // as a store revision, so a restore survives the worker pruning its
    // store; the intent lives until the restored scene is confirmed on
    // air, and an error leaves it in place so the retry stays available.
    // They do nothing until a scene is on air.
    Q_INVOKABLE void clearSlide();
    Q_INVOKABLE void clearBackground();
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void restoreOutput();

    QString statusText() const { return fStatusText; }
    bool isConnected() const { return fConnected; }
    bool outputLive() const { return fConnected && !fOutputStopped; }
    qint64 frames() const { return fFrames; }
    qint64 revision() const { return fRevision; }
    QString device() const { return fDevice; }
    QVariantMap layers() const { return fLayers; }
    qint64 currentMonitor() const { return fCurrentMonitor; }
    QVariantList displays() const { return fDisplays; }
    qint64 sceneActive() const { return fSceneActive; }
    qint64 scenePending() const { return fScenePending; }
    qint64 scenePrepared() const { return fScenePrepared; }
    bool wordsOnAir() const { return fSceneContent.words; }
    bool backgroundOnAir() const { return fSceneContent.background; }
    bool canRestore() const { return !fRestoreJson.isEmpty(); }

    qint64 demoSlideCount() const;

    // The copied frame for PreviewView (UI thread only).
    const QImage& previewImage() const { return fPreviewImage; }
    bool previewActive() const { return fMap != nullptr; }
    qint64 previewFrames() const { return fPreviewFrames; }
    qint64 previewDropped() const { return fPreviewDropped; }
    QString previewError() const { return fPreviewError; }
    bool mediaPlaying() const { return fMediaPlaying; }
    QString mediaError() const { return fMediaError; }
    QString mediaAudio() const { return fMediaAudio; }
    bool mediaOnAir() const { return fMediaLive; }
    double mediaPosition() const { return fMediaPosition; }
    double mediaDuration() const { return fMediaDuration; }
    bool mediaLoop() const { return fMediaLoop; }
    QVariantList mediaSources() const { return fMediaSources; }

signals:
    void stateChanged();
    void outputStatusChanged();
    void displaysChanged();
    void previewChanged();
    void sceneChanged();
    void mediaChanged();

private slots:
    void tryConnect();
    void onSocketReady();
    void onSocketConnected();
    void onSocketDisconnected();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void pollStatus();

private:
    // Reply correlation (task 6.3a, review R3): every request is recorded
    // with the connection generation it was written on, and every reply
    // belongs to the request its id names — never to "whatever result
    // shape arrived". A prepare carries its own payload and, when it began
    // as a go, the intent to present the revision the worker hands back.
    struct PendingRequest {
        std::string method;
        QString prepareJson;       // scene.prepare: the payload to mirror
        bool autoPresent = false;  // present the acked revision (a "go")
        std::uint64_t connection = 0;
    };

    std::int64_t sendRequest(const char* method, boost::json::object params);
    void sendNotification(const char* method, boost::json::object params);
    // Arms the stalled-write deadline when the socket's backlog is over budget
    // (task 6.8b, review R11). When the window closes, a backlog that has shrunk
    // at all means the worker is slow and stays connected; one that has not lost
    // a single byte means it is gone, and the connection is closed rather than
    // buffered indefinitely.
    void watchWriteBacklog();
    void onWriteStallExpired();
    // The sentence both the disconnect path and the process-exit path use for a
    // worker this side gave up on, so the reason survives whichever of the two
    // arrives last.
    QString writeStallMessage() const;
    void drainMessages();
    void handleReply(const boost::json::object& message);
    void handleNotification(const boost::json::object& message);
    void openPreviewSegment(const std::string& name, std::uint64_t bytesPerSlot);
    void closePreviewSegment();
    void setStatus(const QString& text);
    // The prepare/present chain the public calls share; unlike
    // presentSceneJson it does not touch the restore slot, so the clear
    // operations can stage a derived scene without forgetting what they
    // cleared. Returns the request id of the prepare, or 0 when the JSON
    // was unreadable or the session not live.
    std::int64_t presentSceneInternal(const QString& sceneJson);
    // Runs the worker's own reader over a scene BEFORE anything is written to
    // the socket (task 6.8a, review R11): returns true when the payload may go
    // on the wire, and otherwise sets the status line to the reason the
    // operator has to fix.
    bool sceneIsSendable(const boost::json::value& candidate, const char* verb);
    // The scene a clear derives from: the pending logical presentation
    // (latest go) when one exists, else the last confirmed on-air payload
    // (task 6.3b: clears issued before an earlier clear's confirmation
    // must compose with it, not resurrect what it removed).
    QString clearSourceScene() const;
    // Attach the per-request prepare context (payload, and the go intent
    // when this prepare should present its ack).
    void armPrepare(std::int64_t requestId, const boost::json::object& scene,
                    bool autoPresent);
    void resetSceneTracking();

    QProcess fProcess;
    QLocalSocket fSocket;
    openshow::protocol::Decoder fDecoder;
    QString fSocketPath;
    QTimer fConnectRetry;
    int fConnectAttempts = 0;
    // Armed by a refusal to queue more and then read on a short tick: what ends
    // the connection is the worker taking no byte for kWriteStallMs, not the
    // depth of the queue. fWriteStalled carries the reason into the disconnect
    // message, and fWriteStalledBytes is the backlog as it stood when the deadline
    // fired — abort() discards the buffer the number came from.
    QTimer fWriteStallDeadline;
    bool fWriteStalled = false;
    qint64 fWriteStalledBytes = 0;
    // When the worker last took a byte, as Qt reports it flushing the backlog.
    // The deadline is measured from this and not from the depth of the queue: a
    // worker that is slow keeps its session, one that has taken nothing for the
    // whole window does not (task 6.8b, review R11).
    std::chrono::steady_clock::time_point fLastByteOutAt{};

    std::int64_t fNextId = 1;
    // The output.status request currently awaiting its answer, 0 for none. The
    // poll is a refresh, so while one is outstanding the next tick asks for
    // something already on its way (task 6.8b).
    std::int64_t fStatusRequestId = 0;
    // Bumped on every successful connect. Replies are only honoured for
    // requests of the current connection; the decoder is reset with it,
    // so bytes of a dead stream can never answer a new one.
    std::uint64_t fConnectionGeneration = 0;
    QHash<std::int64_t, PendingRequest> fPendingRequests;
    bool fHandshakeDone = false;
    bool fConnected = false;
    bool fOutputStopped = false;
    bool fShutdownRequested = false;

    QString fStatusText = QStringLiteral("worker not started");
    qint64 fFrames = 0;
    qint64 fRevision = 0;
    QString fDevice;
    QVariantMap fLayers;
    qint64 fCurrentMonitor = -1;
    QVariantList fDisplays;

    // Scene adapter state (task 2.4c): the worker's store revisions as
    // last reported by output.status, plus the prepare-then-present
    // chain presentDemoSlide uses (the present waits for prepare's ack).
    qint64 fSceneActive = 0;
    qint64 fScenePending = 0;
    qint64 fScenePrepared = 0;
    openshow::domain::Documents fDemoDocuments = openshow::scene::demoDocuments();

    // Output controls (task 3.4): the JSON each revision was prepared with,
    // so a clear can derive from what is actually on air and the live
    // indicators can describe it. The worker may prune its store; this mirror
    // is kept to the same recent window and is only a mirror, never the
    // source of truth for what output shows.
    QHash<qint64, QString> fSceneJson;
    qint64 fMirrorMax = 0;        // highest revision mirrored (prune bound)
    SceneContent fSceneContent;   // derived from fSceneJson[fSceneActive]
    // Restore state (task 6.3b): the payload the first clear of a sequence
    // cut from, retained as content so the worker's store window cannot
    // retire it; the id of a restore whose prepare is still in flight; and
    // the revision that must reach "active" before the snapshot retires.
    QString fRestoreJson;
    std::int64_t fRestoreArmedGo = 0;
    qint64 fRestoreDoneRevision = 0;
    // The payload of the newest go: what the output will show once that
    // go is acknowledged. Clears derive from this, not from the last
    // confirmed status, so rapid successive clears compose (task 6.3b).
    QString fDesiredSceneJson;
    // Id of the most recent go-intent still awaiting its presentation.
    // A prepare ack presents only while its request is still the newest
    // go (the latest-go rule): an older go acknowledged late must not
    // regress the audience off a newer one.
    std::int64_t fLatestGoId = 0;

    // Preview consumer state (UI thread only): the read-only mapping of
    // the worker's shm segment and the frame last copied out of it.
    std::uint8_t* fMap = nullptr;
    int fMapFd = -1;
    std::uint64_t fMapBytes = 0;
    std::uint64_t fPreviewSlotStride = 0;
    std::uint64_t fPreviewSlotCount = 0;
    // The start answer names a segment the render thread has not
    // necessarily created yet; it is mapped on the first ready.
    std::string fPendingPreviewName;
    QImage fPreviewImage;
    qint64 fPreviewFrames = 0;
    qint64 fPreviewDropped = 0;
    QString fPreviewError;
    bool fMediaPlaying = false;
    QString fMediaError;
    // The lane as the worker last reported it (task 6.7c); "none" is also
    // the answer for a worker too old to send the field.
    QString fMediaAudio = QStringLiteral("none");
    // The transport echo of status (task 6.15a, review R15). Every field
    // is optional on the wire: a worker older than minor 6 keeps the last
    // known answer rather than being read as a stopped transport. The
    // defaults mirror a session that has never seen media at all.
    bool fMediaLive = false;
    double fMediaPosition = -1.0;
    double fMediaDuration = -1.0;
    bool fMediaLoop = false;
    QVariantList fMediaSources;
};

}  // namespace openshow::ui
