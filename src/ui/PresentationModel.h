/*
 * PresentationModel: the frontend's half of "what is on air, and what is owed
 * the air" — the acknowledged output model behind the scene properties.
 *
 * The worker's store hands out revisions; this class is the session's record of
 * what it handed out and what each one was. It mirrors the payload of every
 * prepare the session itself sent, keeps the newest go's payload as the scene
 * the output is owed, derives each clear from that pending presentation rather
 * than from the last confirmed status, retains the scene the first clear of a
 * sequence cut from as CONTENT so a restore survives the store pruning it, and
 * retires that snapshot only when a status poll confirms the restored revision
 * actually on air.
 *
 * It is the single owner of those truths, which is the point (task 6.19a3): a
 * QML property reads one place, so an indicator cannot light from a request that
 * was refused, and a clear cannot compose against a payload the operator has
 * already replaced. Like PreviewConsumer it owns no transport — it asks through
 * callbacks its host injects, and interprets only the prepare ack and the `scene`
 * block of a status answer.
 */

#pragma once

#include <boost/json.hpp>

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <memory>

#include "SceneAsk.h"
#include "SceneEdit.h"

namespace openshow::ui {

class PresentationModel : public QObject {
    Q_OBJECT
public:
    // What the model needs from whoever owns the socket. It sends, and it can
    // reach back into a request it has already put on the wire — arming a
    // prepare is done after the send, because the payload the ack has to mirror
    // is this record's, not a side table keyed by an id that another request
    // could claim (task 6.3a).
    struct Outbound {
        // False until the handshake is done and while the worker is gone: an
        // operation on a session that cannot carry it is not attempted, and it
        // leaves no state behind that a later connection could act on.
        std::function<bool()> live;
        // The authoring budgets (task 6.8a, review R11), checked over a scene
        // before any byte of it goes on the wire — the same check a preview
        // selection is held to, so the go button and the list cannot disagree
        // about what the worker will take.
        std::function<bool(const boost::json::value& candidate, const char* verb)> budget;
        // Send a request; 0 means the transport refused it locally and had
        // already said why, so the caller's state must not move.
        std::function<std::int64_t(const char* method, boost::json::object params)> request;
        // The intent record of the request this id names, created if it has none
        // yet; nullptr when no request exists (a refused send).
        std::function<std::shared_ptr<ScenePrepareAsk>(std::int64_t requestId)> prepareHandle;
        // Visit every prepare still awaiting its ack, so a payload the worker
        // has but this side now believes different can be corrected.
        std::function<void(const std::function<void(ScenePrepareAsk&)>& visit)> visitPrepares;
        // The operator-facing line, for the failures this class names itself.
        std::function<void(const QString& text)> statusLine;
    };

    PresentationModel(Outbound outbound, QObject* parent = nullptr);

    // ---- the operations the UI calls --------------------------------------
    // Stage a compiled scene in the worker's store without presenting it.
    void prepareScene(const QString& sceneJson);
    // Present a revision the store has already handed out.
    void presentScene(qint64 revision);
    // Prepare this JSON and present it on the acknowledgement.
    void presentSceneJson(const QString& sceneJson);
    // The same one-step go for a scene this build compiled itself (the demo
    // fixture), which is therefore not re-read against the authoring budgets.
    void presentCompiledScene(const boost::json::object& scene);
    // The derived presentations (task 3.4, hardened by 6.3b).
    void clearSlide();
    void clearBackground();
    void clearAll();
    void restoreOutput();
    // A save moved an asset's bytes: rewrite every payload this model holds
    // (and every prepare still awaiting its ack) and re-prepare what the output
    // is owed. Never re-presented: that would restart a fade.
    void adoptAssetPaths(const QVariantMap& pathMap);

    // ---- the transport's events -------------------------------------------
    // The store named this revision for the prepare carrying `intent`.
    void handlePrepareAck(std::int64_t requestId, const ScenePrepareAsk& intent,
                          qint64 revision);
    // A request was refused. A restore keeps its content and loses only its
    // in-flight marker, so the retry stays available.
    void handleRefusal(std::int64_t requestId);
    // The `scene` block of output.status. A worker that does not send one is
    // not an answer: nothing here moves.
    void updateFromStatus(const boost::json::object& payload);
    // The session is gone: no revision, no pending go, no restore point.
    void reset();

    // ---- the truth QML reads ----------------------------------------------
    qint64 sceneActive() const { return fSceneActive; }
    qint64 scenePending() const { return fScenePending; }
    qint64 scenePrepared() const { return fScenePrepared; }
    bool wordsOnAir() const { return fSceneContent.words; }
    bool backgroundOnAir() const { return fSceneContent.background; }
    bool canRestore() const { return !fRestoreJson.isEmpty(); }

signals:
    // Any change to the acknowledged state or to what is owed it; the host
    // forwards this as the signal its properties name.
    void changed();

private:
    // The scene a clear derives from: the pending logical presentation (latest
    // go) when one exists, else the last confirmed on-air payload (task 6.3b:
    // clears issued before an earlier clear's confirmation must compose with it,
    // not resurrect what it removed).
    QString clearSourceScene() const;
    // Attach the per-request prepare context (payload, and the go intent when
    // this prepare should present its ack).
    void armPrepare(std::int64_t requestId, const boost::json::object& scene,
                    bool autoPresent);
    // Prepare then present-on-ack; the restore slot is NOT touched here, so the
    // clear controls can stage a derived scene without forgetting what they
    // cleared. Returns the request id, or 0 when the JSON was unreadable or the
    // session not live.
    std::int64_t presentSceneInternal(const QString& sceneJson);
    // Parse, check against the worker's own budgets, and refuse locally with the
    // reason on the status line; false leaves every truth untouched.
    bool readableScene(const QString& sceneJson, const char* verb, boost::json::value& parsed);

    Outbound fOut;

    // Output controls (task 3.4): the JSON each revision was prepared with, so
    // a clear can derive from what is actually on air and the live indicators
    // can describe it. The worker may prune its store; this mirror is kept to
    // the same recent window and is only a mirror, never the source of truth
    // for what output shows.
    QHash<qint64, QString> fSceneJson;
    qint64 fMirrorMax = 0;  // highest revision mirrored (prune bound)
    // The worker's reported scene state, and the indicators derived from the
    // payload the ACTIVE revision was prepared with.
    qint64 fSceneActive = 0;
    qint64 fScenePending = 0;
    qint64 fScenePrepared = 0;
    SceneContent fSceneContent;
    // Restore state (task 6.3b): the payload the first clear of a sequence cut
    // from, retained as content so the worker's store window cannot retire it;
    // the id of a restore whose prepare is still in flight; and the revision
    // that must reach "active" before the snapshot retires.
    QString fRestoreJson;
    std::int64_t fRestoreArmedGo = 0;
    qint64 fRestoreDoneRevision = 0;
    // The payload of the newest go: what the output will show once that go is
    // acknowledged. Clears derive from this, not from the last confirmed status,
    // so rapid successive clears compose (task 6.3b).
    QString fDesiredSceneJson;
    // Id of the most recent go-intent still awaiting its presentation. A
    // prepare ack presents only while its request is still the newest go (the
    // latest-go rule): an older go acknowledged late must not regress the
    // audience off a newer one.
    std::int64_t fLatestGoId = 0;
};

}  // namespace openshow::ui
