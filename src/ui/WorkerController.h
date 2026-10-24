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

    // Output controls (task 3.4). Each clear is a derived presentation:
    // the scene on air with one layer's content removed (the reference's
    // independent per-layer clears), remembered so a single restore can
    // re-present it. They do nothing until a scene is on air.
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
    bool canRestore() const { return fRestoreRevision != 0; }

    qint64 demoSlideCount() const;

    // The copied frame for PreviewView (UI thread only).
    const QImage& previewImage() const { return fPreviewImage; }
    bool previewActive() const { return fMap != nullptr; }
    qint64 previewFrames() const { return fPreviewFrames; }
    qint64 previewDropped() const { return fPreviewDropped; }
    QString previewError() const { return fPreviewError; }

signals:
    void stateChanged();
    void outputStatusChanged();
    void displaysChanged();
    void previewChanged();
    void sceneChanged();

private slots:
    void tryConnect();
    void onSocketReady();
    void onSocketConnected();
    void onSocketDisconnected();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void pollStatus();

private:
    void sendRequest(const char* method, boost::json::object params);
    void sendNotification(const char* method, boost::json::object params);
    void drainMessages();
    void handleReply(const boost::json::object& message);
    void handleNotification(const boost::json::object& message);
    void openPreviewSegment(const std::string& name, std::uint64_t bytesPerSlot);
    void closePreviewSegment();
    void setStatus(const QString& text);
    // The prepare/present chain the public calls share; unlike
    // presentSceneJson it does not touch the restore slot, so the clear
    // operations can stage a derived scene without forgetting what they
    // cleared.
    void presentSceneInternal(const QString& sceneJson);
    void notePrepare(const boost::json::object& scene);
    void resetSceneTracking();

    QProcess fProcess;
    QLocalSocket fSocket;
    openshow::protocol::Decoder fDecoder;
    QString fSocketPath;
    QTimer fConnectRetry;
    int fConnectAttempts = 0;

    std::int64_t fNextId = 1;
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
    bool fAutoPresentScene = false;
    openshow::domain::Documents fDemoDocuments = openshow::scene::demoDocuments();

    // Output controls (task 3.4): the JSON each revision was prepared with,
    // so a clear can derive from what is actually on air and the live
    // indicators can describe it. The worker may prune its store; this mirror
    // is kept to the same recent window and is only a mirror, never the
    // source of truth for what output shows.
    QHash<qint64, QString> fSceneJson;
    QString fPendingPrepareJson;  // JSON of the prepare awaiting its revision
    SceneContent fSceneContent;   // derived from fSceneJson[fSceneActive]
    qint64 fRestoreRevision = 0;  // full revision the current clear sequence cut from

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
};

}  // namespace openshow::ui
