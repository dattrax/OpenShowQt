#include "SceneEdit.h"

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

void dropBackground(boost::json::object& root) {
    root["backgroundColor"] = 0u;
    if (root.contains("background") && root.at("background").is_object()) {
        boost::json::object& background = root.at("background").as_object();
        background["image"] = false;
        background["assetId"] = "";
        background["assetPath"] = "";
    }
}

}  // namespace

SceneContent sceneContent(const QString& sceneJson) {
    SceneContent content;
    bool ok = false;
    const boost::json::value parsed = parseObject(sceneJson, &ok);
    if (!ok) {
        return content;
    }
    const boost::json::object& root = parsed.as_object();
    const auto items = root.find("items");
    content.words = items != root.end() && items->value().is_array() &&
                    !items->value().as_array().empty();
    const auto color = root.find("backgroundColor");
    content.background =
        color != root.end() && color->value().is_number() &&
        color->value().to_number<std::uint64_t>() != 0;
    const auto background = root.find("background");
    if (background != root.end() && background->value().is_object()) {
        const auto image = background->value().as_object().find("image");
        if (image != background->value().as_object().end() && image->value().is_bool() &&
            image->value().as_bool()) {
            content.background = true;
        }
    }
    return content;
}

QString withWordsCleared(const QString& sceneJson) {
    return edited(sceneJson, &dropWords);
}

QString withBackgroundCleared(const QString& sceneJson) {
    return edited(sceneJson, &dropBackground);
}

QString clearedSceneJson() {
    return serializeJson(openshow::scene::toJson(openshow::scene::clearedScene()));
}

}  // namespace openshow::ui
