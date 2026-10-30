/*
 * PresentationModel implementation. The logic is the session's acknowledged
 * output model exactly as the go and clear controls earned it through tasks
 * 2.4c, 3.4, 6.3a, 6.3b and 6.8a — extracted unchanged (task 6.19a3) so the
 * truths live in one class with the reads that expose them.
 */

#include "PresentationModel.h"

#include <boost/json.hpp>

namespace openshow::ui {

namespace {

// Revisions of prepared-scene JSON kept as a mirror for the clear
// controls (task 3.4). Bounded; the worker prunes its own store too.
constexpr int kSceneMirrorSize = 32;

// The foreground-fault list the worker reports per poll is capped at its
// own bound (OutputSession's kMaxReportedForegroundFailures = 8); this is
// the receiving side's identical ceiling, so a peer — real or scripted —
// that sent more than the contract allows cannot grow a UI list without
// limit (task 6.21f, review F6).
constexpr int kMaxForegroundFaults = 8;

}  // namespace

PresentationModel::PresentationModel(Outbound outbound, QObject* parent)
    : QObject(parent), fOut(std::move(outbound)) {}

// --- the operations the UI calls ---------------------------------------------

// The authored scene is checked HERE, before it is sent (task 6.8a, review
// R11): the budgets that bound a slide — items, lines, characters, image
// bytes — are the worker's, and a scene that would come back refused is
// refused locally instead of costing a round trip and confusing an operator
// who watches a button do nothing.
void PresentationModel::prepareScene(const QString& sceneJson) {
    if (!fOut.live()) {
        return;
    }
    boost::json::value parsed;
    if (!readableScene(sceneJson, "scene.prepare", parsed)) {
        return;
    }
    // The scene object itself is the params (the worker validates it too).
    const std::int64_t id = fOut.request("scene.prepare", parsed.as_object());
    armPrepare(id, parsed.as_object(), /*autoPresent=*/false);
}

void PresentationModel::presentScene(qint64 revision) {
    if (!fOut.live()) {
        return;
    }
    // An explicit present re-targets the logical presentation: later
    // clears derive from what this revision was prepared with (task 6.3b).
    fDesiredSceneJson = fSceneJson.value(revision);
    fOut.request("scene.present",
                 boost::json::object{{"revision", static_cast<std::uint64_t>(revision)}});
}

void PresentationModel::presentSceneJson(const QString& sceneJson) {
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

// The demo fixture's go: the payload is this build's own compiler's output,
// so it is not re-read against the budgets (there is no authoring mistake to
// catch — only the presentation promise matters), and a fresh go retires any
// clear's restore point just as any new content does.
void PresentationModel::presentCompiledScene(const boost::json::object& scene) {
    if (!fOut.live()) {
        return;
    }
    fRestoreJson.clear();  // a fresh go retires any clear's restore point
    fRestoreArmedGo = 0;
    fRestoreDoneRevision = 0;
    // Present on the prepare acknowledgement: the revision is the
    // worker's to hand out, and the store only names it once stored.
    const std::int64_t id = fOut.request("scene.prepare", scene);
    armPrepare(id, scene, /*autoPresent=*/true);
}

// --- output controls (task 3.4, hardened by task 6.3b) -----------------------
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

QString PresentationModel::clearSourceScene() const {
    if (!fDesiredSceneJson.isEmpty()) {
        return fDesiredSceneJson;
    }
    return fSceneJson.value(fSceneActive);
}

void PresentationModel::clearSlide() {
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

void PresentationModel::clearBackground() {
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

void PresentationModel::clearAll() {
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

void PresentationModel::restoreOutput() {
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

void PresentationModel::adoptAssetPaths(const QVariantMap& pathMap) {
    if (pathMap.isEmpty()) {
        return;
    }
    // One payload's rewrite: the new JSON when ANY assetPath this save moved
    // appears in it — the background's pair since 6.12, and since 6.21b
    // (review F2) the `assetPath` of every image ITEM too, because a
    // foreground picture is just as much the operator's file as a background
    // one. Empty for anything else — an unreadable payload or one that does
    // not name the file is left exactly as it was, because there is nothing
    // here that was made stale by the move.
    const auto rewrite = [&pathMap](const QString& sceneJson) {
        if (sceneJson.isEmpty()) {
            return QString();
        }
        boost::system::error_code parseError;
        boost::json::value parsed = boost::json::parse(sceneJson.toStdString(), parseError);
        if (parseError || !parsed.is_object()) {
            return QString();
        }
        bool changed = false;
        const auto repoint = [&pathMap, &changed](boost::json::object& holder) {
            const auto path = holder.find("assetPath");
            if (path == holder.end() || !path->value().is_string()) {
                return;
            }
            const QString oldPath = QString::fromStdString(std::string(path->value().as_string()));
            if (!pathMap.contains(oldPath)) {
                return;
            }
            path->value() = pathMap.value(oldPath).toString().toStdString();
            changed = true;
        };
        const auto background = parsed.as_object().find("background");
        if (background != parsed.as_object().end() && background->value().is_object()) {
            repoint(background->value().as_object());
        }
        const auto items = parsed.as_object().find("items");
        if (items != parsed.as_object().end() && items->value().is_array()) {
            for (auto& element : items->value().as_array()) {
                if (element.is_object()) {
                    repoint(element.as_object());
                }
            }
        }
        if (!changed) {
            return QString();
        }
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
    // this model attached to it is what gets rewritten.
    fOut.visitPrepares([&rewrite](ScenePrepareAsk& ask) {
        if (ask.prepareJson.isEmpty()) {
            return;
        }
        const QString adopted = rewrite(ask.prepareJson);
        if (!adopted.isEmpty()) {
            ask.prepareJson = adopted;
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

// --- the transport's events ----------------------------------------------------

void PresentationModel::handlePrepareAck(std::int64_t requestId, const ScenePrepareAsk& intent,
                                         qint64 revision) {
    // The scene now lives in the worker's store under this revision,
    // and THIS request's payload is what was stored under it — a
    // second prepare issued before the first was acknowledged cannot
    // move either mirror entry (task 6.3a).
    if (!intent.prepareJson.isEmpty()) {
        fSceneJson.insert(revision, intent.prepareJson);
        if (revision > fMirrorMax) {
            fMirrorMax = revision;
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
    if (intent.autoPresent && requestId == fLatestGoId) {
        fLatestGoId = 0;
        presentScene(revision);
        if (requestId == fRestoreArmedGo) {
            // The restore's own go moved the audience; its snapshot
            // retires once a status poll confirms this revision on
            // air — until then canRestore honestly says the restored
            // scene is not yet confirmed (task 6.3b).
            fRestoreArmedGo = 0;
            fRestoreDoneRevision = revision;
        }
    }
    emit changed();
}

void PresentationModel::handleRefusal(std::int64_t requestId) {
    // A rejected restore keeps its retained content (task 6.3b): only the
    // in-flight marker clears, so the retry is available.
    if (requestId == fRestoreArmedGo) {
        fRestoreArmedGo = 0;
    }
}

void PresentationModel::updateFromStatus(const boost::json::object& payload) {
    const auto scene = payload.find("scene");
    if (scene == payload.end() || !scene->value().is_object()) {
        return;  // a worker that sends no scene block answers nothing here
    }
    const boost::json::object& block = scene->value().as_object();
    fSceneActive = static_cast<qint64>(block.at("active").to_number<std::uint64_t>());
    fScenePending = static_cast<qint64>(block.at("pending").to_number<std::uint64_t>());
    fScenePrepared = static_cast<qint64>(block.at("prepared").to_number<std::uint64_t>());
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
    // The per-item foreground faults (task 6.21f, review F6). Replaced
    // wholesale from this poll, exactly as the worker replaced them: an
    // entry whose path a later prepare decoded successfully is simply not
    // in the next list, so a repair that the worker has acknowledged makes
    // the notice go — and a status block that carries no such array (an
    // older worker) leaves the lane empty rather than stale. The shape is
    // checked entry by entry: a malformed element costs that entry only.
    QVariantList foregroundErrors;
    const auto failures = block.find("foregroundErrors");
    if (failures != block.end() && failures->value().is_array()) {
        for (const boost::json::value& element : failures->value().as_array()) {
            if (!element.is_object() || foregroundErrors.size() >= kMaxForegroundFaults) {
                continue;
            }
            const boost::json::object& fault = element.as_object();
            const auto path = fault.find("path");
            const auto error = fault.find("error");
            if (path == fault.end() || !path->value().is_string() || error == fault.end() ||
                !error->value().is_string()) {
                continue;
            }
            foregroundErrors.push_back(QVariantMap{{QStringLiteral("path"),
                                                   QString::fromStdString(
                                                       std::string(path->value().as_string()))},
                                                  {QStringLiteral("error"),
                                                   QString::fromStdString(
                                                       std::string(error->value().as_string()))}});
        }
    }
    const auto events = block.find("foregroundFailureEvents");
    fForegroundErrors = foregroundErrors;
    fForegroundFailureEvents =
        events != block.end() && events->value().is_number()
            ? static_cast<qint64>(events->value().to_number<std::uint64_t>())
            : 0;
    emit changed();
}

void PresentationModel::reset() {
    fSceneJson.clear();
    fMirrorMax = 0;
    fSceneContent = SceneContent{};
    // The worker's fault list died with the connection: a reconnect's first
    // poll reports the truth again, and a stale amber line over a fresh
    // session is the lie R18 was about (task 6.21f).
    fForegroundErrors.clear();
    fForegroundFailureEvents = 0;
    fRestoreJson.clear();
    fRestoreArmedGo = 0;
    fRestoreDoneRevision = 0;
    fDesiredSceneJson.clear();
    fLatestGoId = 0;
    // The three revisions are the worker's own counters, "as last reported".
    // They survive here exactly as they survived the connection before this
    // class existed: a reconnect re-confirms them with the first status poll,
    // and nothing between the loss and that poll reads them for a decision.
    emit changed();
}

// --- shared plumbing ------------------------------------------------------------

std::int64_t PresentationModel::presentSceneInternal(const QString& sceneJson) {
    if (!fOut.live()) {
        return 0;
    }
    boost::json::value parsed;
    if (!readableScene(sceneJson, "scene.present", parsed)) {
        return 0;
    }
    // Same hand-off as presentCompiledScene: present on the prepare ack, so
    // the audience switch lands as soon as the store names the revision.
    const std::int64_t id = fOut.request("scene.prepare", parsed.as_object());
    armPrepare(id, parsed.as_object(), /*autoPresent=*/true);
    return id;
}

bool PresentationModel::readableScene(const QString& sceneJson, const char* verb,
                                      boost::json::value& parsed) {
    boost::system::error_code parseError;
    parsed = boost::json::parse(sceneJson.toStdString(), parseError);
    if (parseError || !parsed.is_object()) {
        fOut.statusLine(
            QStringLiteral("%1: the scene JSON is not readable").arg(QString::fromLatin1(verb)));
        return false;
    }
    return fOut.budget(parsed, verb);
}

void PresentationModel::armPrepare(std::int64_t requestId, const boost::json::object& scene,
                                   bool autoPresent) {
    // The prepare's payload is recorded on the request itself, not on a side
    // table keyed by id: when the ack lands it brings this record with it, so a
    // mirror entry can only ever be written from the payload THAT prepare sent
    // (task 6.3a). A refused send has no entry, so it arms nothing.
    const std::shared_ptr<ScenePrepareAsk> ask = fOut.prepareHandle(requestId);
    if (!ask) {
        return;  // sendRequest always records the entry; belt and braces
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

}  // namespace openshow::ui
