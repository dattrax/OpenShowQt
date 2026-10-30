/*
 * WorkerController: the frontend's session half of the worker conversation —
 * the QML facade that owns the process (task 1.2 supervision) and routes the
 * acknowledged output model behind the status properties.
 *
 * It spawns openshow-worker --socket PATH and runs the JSON-RPC handshake
 * through RpcClient, which owns the socket, the framing and the rule that a
 * reply belongs to the request its id names. This class decides which event is
 * whose: what the store acknowledged goes to PresentationModel — the task
 * 6.19a3 owner of what is on air, what the output is owed, and the restore
 * point — and what the worker has to show goes to PreviewConsumer, the task
 * 6.19a2 owner of the stream the pictures travel in. The same session
 * operations the UI calls are what the RPC exposes: the operator plays frames,
 * toggles layers, goes to scenes, and the facade forwards each to whoever
 * owns its half.
 *
 * Lifecycle per PLAN.md: the worker lives only while connected. Closing
 * the UI sends "shutdown"; a worker that dies or hangs anyway is
 * terminated, and its state is reported honestly.
 */

#pragma once

#include <boost/json.hpp>

#include <QHash>
#include <QImage>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

#include <chrono>
#include <cstdint>
#include <memory>

#include "openshow/protocol/protocol.h"
#include "openshow/scene/demo.h"

#include "PresentationModel.h"
#include "PreviewConsumer.h"
#include "RpcClient.h"

namespace openshow::ui {

class PreviewConsumer;

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
    // The editor's off-air picture (task 6.16a, review R16): the worker's own
    // Skia render of the slide under edit, drawn into the same stream without
    // presenting it. selectionFrames counts the edit frames this client
    // copied; selectionStale is the worker's counter of edit draws a newer
    // ask overtook before they reached the wire (they are destroyed, never
    // shown, so a rising stale is the editor being faster than the GPU, not
    // the operator seeing yesterday's slide).
    Q_PROPERTY(qint64 selectionFrames READ selectionFrames NOTIFY previewChanged)
    Q_PROPERTY(qint64 selectionStale READ selectionStale NOTIFY previewChanged)
    Q_PROPERTY(bool selectionSupported READ selectionSupported NOTIFY previewChanged)
    // True once a picture answering the newest ask is in hand. The canvas
    // shows its placeholder while this is false: the alternative — painting
    // the slide's text with QML until the worker's version arrives — is the
    // second text layout this task removes.
    Q_PROPERTY(bool selectionReady READ selectionReady NOTIFY previewChanged)
    // The slide list's pictures (task 6.16b, review R16): the worker's own
    // render of the visible band, small and off the air, on the same stream.
    // `thumbnailSupported` is the whole negotiation — a worker without the
    // capability gets the list's text line and nothing else. The two counters
    // are the client's own (frames copied) and the worker's (stale: pictures a
    // newer band superseded before they reached the wire), and
    // thumbnailCachedBytes is this client's byte-bounded LRU, so a document of
    // any length has a picture memory that is a number rather than a habit.
    Q_PROPERTY(bool thumbnailSupported READ thumbnailSupported NOTIFY previewChanged)
    Q_PROPERTY(qint64 thumbnailFrames READ thumbnailFrames NOTIFY previewChanged)
    Q_PROPERTY(qint64 thumbnailStale READ thumbnailStale NOTIFY previewChanged)
    Q_PROPERTY(qint64 thumbnailCachedBytes READ thumbnailCachedBytes NOTIFY previewChanged)
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
    // Standalone production track (task 6.15b, review R15): the independent
    // transport, selected by path and confirmed by the status block of the
    // same name. standaloneOnAir is its commandability mirror
    // (media.standalone.live); path, position and error are the track's
    // own story — the media* properties above never describe it, and
    // scene changes move none of this except through the track itself.
    Q_PROPERTY(bool standaloneOnAir READ standaloneOnAir NOTIFY mediaChanged)
    Q_PROPERTY(QString standalonePath READ standalonePath NOTIFY mediaChanged)
    Q_PROPERTY(double standalonePosition READ standalonePosition NOTIFY mediaChanged)
    Q_PROPERTY(QString standaloneError READ standaloneError NOTIFY mediaChanged)
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
    // Foreground pictures the worker could not decode (task 6.21f, review
    // F6): the bounded [{path, error}, ...] list from the last status poll
    // and the worker's lifetime failure-event count beside it. Their own
    // lane: the transport's mediaError above says nothing about a stack
    // picture, and a stack picture says nothing about the transport.
    Q_PROPERTY(QVariantList foregroundErrors READ foregroundErrors NOTIFY sceneChanged)
    Q_PROPERTY(qint64 foregroundFailureEvents READ foregroundFailureEvents NOTIFY sceneChanged)

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

    // The editor's off-air picture (task 6.16a, review R16): prepare this
    // scene and ask the worker to draw it into the preview stream WITHOUT
    // presenting it, so the edit canvas can show the worker's own Skia
    // composition — the production text layout, the background media — under
    // the drag handles instead of a QML guess at it. Idempotent per payload
    // (an unchanged slide is not re-asked) and every real change carries a
    // new request generation, which is what makes yesterday's imagery
    // impossible rather than unlikely: a replay cannot be accepted, and a
    // frame answering an older ask is dropped here rather than painted.
    Q_INVOKABLE void requestSelectionPreview(const QString& sceneJson);
    // Stand the target down (no slide selected, the edit page closed): the
    // stream goes back to mirroring the audience alone, and the worker stops
    // holding a picture of a slide nobody is looking at.
    Q_INVOKABLE void clearSelectionPreview();

    // The slide list's band (task 6.16b, review R16): ask the worker to render
    // THESE scenes at THIS size as thumbnails, off the air and out of the
    // store. `slides` is [{key, scene}], the scene being the same compiled JSON
    // scene.prepare takes, and the batch REPLACES whatever was asked before —
    // which is what makes coalescing and cancelling one act: scroll past a
    // slide and its picture stops being owed. An empty band stands the target
    // down, exactly as clearSelectionPreview does for one edit.
    Q_INVOKABLE void requestThumbnails(const QVariantList& slides, qint64 width, qint64 height);
    Q_INVOKABLE void clearThumbnails();

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

    // Standalone production track (task 6.15b, review R15): select the
    // independent audio file ("" removes it). The worker probes before
    // acknowledging, so a bad path is a refused request whose reason
    // reaches the status line like any other worker error, and the
    // properties stay on the last worker truth — the track on air (if
    // any) was never touched. When the track is up, its own story is the
    // standalone* properties, mirrored from status and nothing else.
    Q_INVOKABLE void selectStandaloneTrack(const QString& path);

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
    // The output model (task 6.19a3): the truth is PresentationModel's; these
    // are the reads the QML properties point at.
    qint64 sceneActive() const { return fPresent->sceneActive(); }
    qint64 scenePending() const { return fPresent->scenePending(); }
    qint64 scenePrepared() const { return fPresent->scenePrepared(); }
    bool wordsOnAir() const { return fPresent->wordsOnAir(); }
    bool backgroundOnAir() const { return fPresent->backgroundOnAir(); }
    bool canRestore() const { return fPresent->canRestore(); }
    QVariantList foregroundErrors() const { return fPresent->foregroundErrors(); }
    qint64 foregroundFailureEvents() const { return fPresent->foregroundFailureEvents(); }

    qint64 demoSlideCount() const;

    // The pictures (task 6.19a2): every one of these is PreviewConsumer's
    // answer about its own copies and gates, re-exported for the views, which
    // bind to the session rather than to the parts of it.
    //
    // The copied frame for PreviewView (UI thread only).
    const QImage& previewImage() const;
    bool previewActive() const;
    qint64 previewFrames() const;
    qint64 previewDropped() const;
    QString previewError() const;
    // The off-air edit frame (task 6.16a): the same transport, the same
    // seqlock copy, a different target. Empty until the worker has drawn the
    // slide under edit — callers paint the document's own placeholder then.
    const QImage& selectionImage() const;
    bool selectionReady() const;
    qint64 selectionFrames() const;
    qint64 selectionStale() const;
    bool selectionSupported() const;
    // The list's pictures (task 6.16b). Keyed lookup, no assembly: the bytes
    // are the worker's, copied under the same seqlock as every other target,
    // and a key the cache has not received yet simply has no image — the
    // caller shows its text line until the worker's version arrives.
    bool thumbnailSupported() const;
    qint64 thumbnailFrames() const;
    qint64 thumbnailStale() const;
    qint64 thumbnailCachedBytes() const;
    QImage thumbnail(const QString& key) const;
    bool mediaPlaying() const { return fMediaPlaying; }
    QString mediaError() const { return fMediaError; }
    QString mediaAudio() const { return fMediaAudio; }
    bool mediaOnAir() const { return fMediaLive; }
    double mediaPosition() const { return fMediaPosition; }
    double mediaDuration() const { return fMediaDuration; }
    bool mediaLoop() const { return fMediaLoop; }
    QVariantList mediaSources() const { return fMediaSources; }
    bool standaloneOnAir() const { return fStandaloneLive; }
    QString standalonePath() const { return fStandalonePath; }
    double standalonePosition() const { return fStandalonePosition; }
    QString standaloneError() const { return fStandaloneError; }

signals:
    void stateChanged();
    void outputStatusChanged();
    void displaysChanged();
    void previewChanged();
    // One slide's picture arrived (task 6.16b). Keyed, so a list delegate
    // repaints its own row and not the thirty around it; previewChanged still
    // carries the counters and the capability.
    void thumbnailReady(const QString& key);
    void sceneChanged();
    void mediaChanged();

private slots:
    // The transport's events, as the session reads them. What the socket doing
    // something means here is a policy question (handshake, resets, what to
    // tell the operator), which is exactly the half RpcClient does not own.
    void onRpcConnected();
    void onRpcDisconnected();
    void onRpcReply(const RpcRequest& request, const boost::json::object& message);
    // A message that names a method is the worker originating, not answering:
    // the media failure that needs the operator now, and the preview frame that
    // says the segment the render thread made is ready to read.
    void handleNotification(const boost::json::object& message);
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void pollStatus();

private:
    // Both forwards to the transport, kept so the session speaks the protocol in
    // the same terms everywhere: a request owes an answer and gets an id, a
    // notification owes nothing. A refused send is 0 (or simply absent) with the
    // reason already in the status line, which is RpcClient's answer, not a
    // failure of the call site. The optional intent is what the sender wants its
    // reply to find (SceneAsk.h is the one this session sends); a request that
    // was refused carries nothing, because it never became one.
    std::int64_t sendRequest(const char* method, boost::json::object params,
                             std::shared_ptr<void> intent = nullptr);
    void sendNotification(const char* method, boost::json::object params);
    void setStatus(const QString& text);
    // Runs the worker's own reader over a scene BEFORE anything is written to
    // the socket (task 6.8a, review R11): returns true when the payload may go
    // on the wire, and otherwise sets the status line to the reason the
    // operator has to fix.
    bool sceneIsSendable(const boost::json::value& candidate, const char* verb);

    QProcess fProcess;
    // The transport (task 6.19a1): the socket, the shared framing, the ledger of
    // requests the worker owes, the bounds on what this side will put on the
    // wire, and the deadline for a peer that has stopped reading. This class
    // tells it when to reach for the worker and what a reply means; it does not
    // own any of that machinery.
    RpcClient fRpc;
    // The output.status request currently awaiting its answer, 0 for none. The
    // poll is a refresh, so while one is outstanding the next tick asks for
    // something already on its way (task 6.8b).
    std::int64_t fStatusRequestId = 0;
    bool fHandshakeDone = false;
    // The SESSION's view of the link, not the socket's: it also goes false when
    // the process is gone, which is a fact the transport is not told about. The
    // QML `connected` property reads this.
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

    openshow::domain::Documents fDemoDocuments = openshow::scene::demoDocuments();

    // The acknowledged output model (task 6.19a3): what the worker's store
    // confirmed and what it is owed — the revision mirror, the pending logical
    // presentation the clears derive from, the restore point — with its own
    // answers behind the scene properties above. The QML operations forward
    // here; the prepare ack, the `scene` block of a status answer and every
    // refused request arrive through the transport in onRpcReply.
    PresentationModel* fPresent = nullptr;

    // The pictures (task 6.19a2): the shm stream, the frames copied out of it,
    // and the rules about which of them this client is still owed. Created with
    // the session and its child, so it outlives nothing the session can be
    // asked about; the QML getters above are its answers.
    PreviewConsumer* fPreview = nullptr;
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
    // The standalone track's status echo (task 6.15b, review R15): the
    // same optional-field rule as the transport's — a worker that does not
    // send the block keeps the last known answer; the defaults mirror a
    // session that never selected a track.
    bool fStandaloneLive = false;
    QString fStandalonePath;
    double fStandalonePosition = -1.0;
    QString fStandaloneError;
};

}  // namespace openshow::ui
