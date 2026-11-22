#include "openshow/scene/stage.h"

#include <algorithm>
#include <cmath>

namespace openshow::scene {

namespace {

bool hasWords(const SceneItem& item) {
    if (item.isImage) {
        return false;
    }
    for (const std::string& line : item.lines) {
        if (!line.empty()) {
            return true;
        }
    }
    return false;
}

bool boxesOverlap(const SceneItem& item) {
    const double left = std::max(item.x, kStageNextBoxX);
    const double top = std::max(item.y, kStageNextBoxY);
    const double right = std::min(item.x + item.width, kStageNextBoxX + kStageNextBoxWidth);
    const double bottom = std::min(item.y + item.height, kStageNextBoxY + kStageNextBoxHeight);
    return right > left && bottom > top;
}

}  // namespace

Scene stageNextOverlay(const Scene& nextCue) {
    Scene overlay = clearedScene();
    overlay.showId = nextCue.showId;
    overlay.slideId = nextCue.slideId;
    // The overlay owns no background: the stage clears opaque black and the
    // current cue's routed background stays where it is.
    overlay.background.transparent = true;
    overlay.background.hasImage = false;
    overlay.background.isVideo = false;
    overlay.background.isCamera = false;

    const SceneItem* primary = nullptr;
    for (const SceneItem& item : nextCue.items) {
        if (hasWords(item)) {
            primary = &item;
            break;
        }
    }
    if (primary == nullptr) {
        return overlay;
    }
    SceneItem item = *primary;
    item.x = kStageNextBoxX;
    item.y = kStageNextBoxY;
    item.width = kStageNextBoxWidth;
    item.height = kStageNextBoxHeight;
    // The resolved size is concrete for authored content; the renderer's
    // default stands in when the chain left the size at "renderer default"
    // (0), so 55% is always a real number rather than a no-op.
    const double base = item.fontSize > 0.0 ? item.fontSize : 96.0;
    item.fontSize = base * kStageNextFontScale;
    item.minFontSize = std::max(1.0, item.minFontSize * kStageNextFontScale);
    overlay.items.push_back(std::move(item));
    return overlay;
}

bool stageNextSlotOverlaps(const Scene& current) {
    for (const SceneItem& item : current.items) {
        if (!item.isImage && boxesOverlap(item)) {
            return true;
        }
    }
    return false;
}

}  // namespace openshow::scene