#include "SceneEdit.h"

#include <functional>

#include <boost/json.hpp>

#include "openshow/scene/scene.h"

namespace openshow::ui {

namespace {

// Parse the wire object, or nullptr-ish through an optional.
boost::json::value parseObject(const QString& sceneJson, bool* ok) {
    boost::system::error_code error;
    boost::json::value parsed = boost::json::parse(sceneJson.toStdString(), error);
    if (error || !parsed.is_object()) {
        *ok = false;
        return {};
    }
    *ok = true;
    return parsed;
}

QString serializeJson(boost::json::value value) {
    return QString::fromStdString(boost::json::serialize(value));
}

// The typed reading of a scene payload; false for anything the worker's own
// reader would refuse.
bool parseScene(const QString& sceneJson, openshow::scene::Scene* scene) {
    boost::system::error_code error;
    const boost::json::value parsed = boost::json::parse(sceneJson.toStdString(), error);
    return !error && openshow::scene::fromJson(parsed, scene);
}

// Edit one object in place; returns "" when the input was not an object.
QString edited(const QString& sceneJson, void (*mutate)(boost::json::object&)) {
    bool ok = false;
    boost::json::value parsed = parseObject(sceneJson, &ok);
    if (!ok) {
        return {};
    }
    boost::json::object& root = parsed.as_object();
    mutate(root);
    return serializeJson(std::move(parsed));
}

void dropWords(boost::json::object& root) { root["items"] = boost::json::array{}; }

// Every asset reference a scene carries, in one place (review D2). The walk
// visits the background, the show's audio layer and each image item; a
// reference it does not visit is a path a save leaves stale in the running
// session.
void visitAssetPaths(boost::json::object& root,
                     const std::function<void(boost::json::value&)>& visit) {
    const auto visitHolder = [&visit](boost::json::value* holder) {
        if (holder == nullptr || !holder->is_object()) {
            return;
        }
        boost::json::value* path = holder->as_object().if_contains("assetPath");
        if (path != nullptr && path->is_string()) {
            visit(*path);
        }
    };
    visitHolder(root.if_contains("background"));
    visitHolder(root.if_contains("audio"));
    boost::json::value* items = root.if_contains("items");
    if (items != nullptr && items->is_array()) {
        for (boost::json::value& element : items->as_array()) {
            visitHolder(&element);
        }
    }
}

}  // namespace

SceneContent sceneContent(const QString& sceneJson) {
    SceneContent content;
    // The flags are the WORKER's reading of the payload, not this client's
    // guess at its keys (review D1). The typed reader normalises every
    // relationship the codec defines — a camera reinstates its picture, a
    // kind flag with no media is refused, transparency suppresses the
    // backdrop — so the indicator and the audience cannot disagree about
    // what is on air. A payload the reader refuses reports nothing, loudly.
    openshow::scene::Scene scene;
    if (!parseScene(sceneJson, &scene)) {
        return content;
    }
    content.words = !scene.items.empty();
    content.background = !scene.background.transparent &&
                         (scene.background.hasImage || scene.background.hasGradient ||
                          scene.background.color != openshow::scene::kBlack);
    // The show's audio layer is its OWN layer: audible activity, never a
    // composition layer (DR.5). It rides the acknowledged payload, so the
    // answer is the cue's, not a hopeful click's.
    content.audio = scene.audio.present;
    return content;
}

QString withRetainedBackground(const QString& sceneJson, const QString& previousJson) {
    bool ok = false;
    auto current = parseObject(sceneJson, &ok);
    if (!ok) return sceneJson;
    auto* background = current.as_object().if_contains("background");
    if (!background || !background->is_object()) return sceneJson;
    auto* alpha = background->as_object().if_contains("transparent");
    if (!alpha || !alpha->is_bool() || !alpha->as_bool()) return sceneJson;
    auto previous = parseObject(previousJson, &ok);
    if (!ok || !sceneContent(previousJson).background) return sceneJson;
    current.as_object()["background"] = previous.as_object().at("background");
    current.as_object()["backgroundColor"] = previous.as_object().at("backgroundColor");
    return serializeJson(std::move(current));
}

QString withWordsCleared(const QString& sceneJson) {
    return edited(sceneJson, &dropWords);
}

QString withBackgroundCleared(const QString& sceneJson) {
    // Review D1: a clear is a COMPLETE scene transformation, built through the
    // typed contract rather than by deleting a few keys. The old edit left
    // `video`, `camera`, `cameraSource` and `cameraMirror` behind, so a video
    // clear produced a payload the codec refused ("background 'video' requires
    // 'image'") and a camera clear came back on air as the same camera while
    // the indicator said the background was gone.
    openshow::scene::Scene scene;
    if (!parseScene(sceneJson, &scene)) {
        return QString();  // not a scene this build can present: nothing to clear
    }
    const openshow::scene::Background cleared;
    // The fit is the SLIDE's standing answer for the next picture, not part of
    // the cleared backdrop, so it rides through the clear untouched.
    const auto fit = scene.background.fit;
    scene.background = cleared;
    scene.background.fit = fit;
    return QString::fromStdString(boost::json::serialize(openshow::scene::toJson(scene)));
}

QString clearedSceneJson() {
    return serializeJson(openshow::scene::toJson(openshow::scene::clearedScene()));
}

QStringList sceneAssetPaths(const QString& sceneJson) {
    bool ok = false;
    boost::json::value parsed = parseObject(sceneJson, &ok);
    if (!ok) {
        return {};
    }
    QStringList paths;
    visitAssetPaths(parsed.as_object(), [&paths](boost::json::value& path) {
        paths.push_back(QString::fromStdString(std::string(path.as_string())));
    });
    return paths;
}

QString withAdoptedAssetPaths(const QString& sceneJson, const QVariantMap& pathMap) {
    bool ok = false;
    boost::json::value parsed = parseObject(sceneJson, &ok);
    if (!ok) {
        return QString();
    }
    bool changed = false;
    visitAssetPaths(parsed.as_object(), [&pathMap, &changed](boost::json::value& path) {
        const QString oldPath = QString::fromStdString(std::string(path.as_string()));
        if (!pathMap.contains(oldPath)) {
            return;
        }
        path = pathMap.value(oldPath).toString().toStdString();
        changed = true;
    });    if (!changed) {
        return sceneJson;  // nothing here was made stale by the move
    }
    return serializeJson(std::move(parsed));
}

}  // namespace openshow::ui
