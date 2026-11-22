/*
 * Stage delivery policy (FO.5a.2/FO.5b.1): the pure rules behind the stage
 * destination, pinned without a window, GPU or worker.
 *
 *  1. Display identity is connection-scoped and survives re-enumeration of
 *     the same attached set. Duplicate names/positions get an ordinal; a
 *     replacement panel at the same index is a different identity; a saved
 *     index is never the proof.
 *  2. The next-cue overlay carries only the next cue's primary TEXT item,
 *     moved into the v1 next slot at 55% of its resolved size. Pictures and
 *     the background do not travel; an image-only cue has no invented next
 *     words.
 *  3. A current text box that intersects the next slot is reported, never
 *     moved by the product.
 */

#include <cstdio>
#include <string>
#include <vector>

#include "openshow/scene/scene.h"
#include "openshow/scene/stage.h"

#include "output/DisplayInventory.h"

namespace {

int gFailures = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++gFailures;
    }
    return condition;
}

using openshow::scene::Scene;
using openshow::scene::SceneItem;
using openshow::worker::DisplayInfo;

SceneItem textItem(double x, double y, double width, double height, double fontSize) {
    SceneItem item;
    item.lines = {"A line of words"};
    item.x = x;
    item.y = y;
    item.width = width;
    item.height = height;
    item.fontSize = fontSize;
    item.minFontSize = 24.0;
    item.fitMode = "shrink";
    return item;
}

SceneItem imageItem() {
    SceneItem item;
    item.isImage = true;
    item.assetId = "picture";
    item.assetPath = "/tmp/picture.png";
    item.width = 400.0;
    item.height = 300.0;
    return item;
}

DisplayInfo panel(int index, const std::string& name, int x, int y, int w, int h) {
    DisplayInfo info;
    info.index = index;
    info.name = name;
    info.x = x;
    info.y = y;
    info.width = w;
    info.height = h;
    info.refreshHz = 60;
    return info;
}

void exerciseDisplayIdentity() {
    std::vector<DisplayInfo> attached{
        panel(0, "eDP-1", 0, 0, 2256, 1504),
        panel(1, "DP-9", 1672, 0, 1024, 768),
        panel(2, "DP-10", 2696, 0, 1920, 1080),
    };
    std::vector<DisplayInfo> previous;
    openshow::worker::stampDisplayIdentities(&attached, previous, 1);
    check(attached[2].identity == "DP-10|2696,0|1920x1080",
          "identity names the panel by name, position and mode");
    check(attached[2].connectionGeneration == 1, "first appearance stamps the generation");
    check(openshow::worker::displayIndexForIdentity(attached, attached[1].identity) == 1,
          "identity resolves to the current index");
    check(!openshow::worker::displayIdentityPresent(attached, ""),
          "an unchosen stage identity is never present");

    // Re-enumerating the same attached set does not bump the inventory.
    std::vector<DisplayInfo> refreshed{
        panel(0, "eDP-1", 0, 0, 2256, 1504),
        panel(1, "DP-9", 1672, 0, 1024, 768),
        panel(2, "DP-10", 2696, 0, 1920, 1080),
    };
    check(openshow::worker::sameDisplayInventory(attached, refreshed),
          "an identical re-enumeration is not a display change");
    openshow::worker::stampDisplayIdentities(&refreshed, attached, 2);
    check(refreshed[2].connectionGeneration == 1,
          "the same panel keeps its first-appearance generation");

    // A REPLACEMENT panel at the saved index reuses the name but not the
    // mode: a stale saved index must not silently use it.
    std::vector<DisplayInfo> replaced{
        panel(0, "eDP-1", 0, 0, 2256, 1504),
        panel(1, "DP-9", 1672, 0, 1024, 768),
        panel(2, "DP-10", 2696, 0, 2560, 1440),
    };
    check(!openshow::worker::sameDisplayInventory(attached, replaced),
          "a replacement panel is an inventory change");
    openshow::worker::stampDisplayIdentities(&replaced, attached, 2);
    check(replaced[2].identity != attached[2].identity,
          "the replacement panel has a different identity");
    check(replaced[2].connectionGeneration == 2,
          "the replacement panel is a new connection");

    // Two indistinguishable panels still get distinct identities.
    std::vector<DisplayInfo> identical{
        panel(0, "HDMI-1", 0, 0, 1920, 1080),
        panel(1, "HDMI-1", 0, 0, 1920, 1080),
    };
    openshow::worker::stampDisplayIdentities(&identical, {}, 3);
    check(identical[0].identity == "HDMI-1|0,0|1920x1080" &&
              identical[1].identity == "HDMI-1|0,0|1920x1080#1",
          "identical panels get ordinals rather than sharing an identity");
}

void exerciseNextOverlay() {
    Scene next;
    next.showId = "show-next";
    next.slideId = "slide-next";
    next.items.push_back(imageItem());
    next.items.push_back(textItem(200.0, 200.0, 800.0, 300.0, 100.0));

    const Scene overlay = openshow::scene::stageNextOverlay(next);
    check(overlay.items.size() == 1, "next overlay carries one text item, not the picture");
    if (overlay.items.size() == 1) {
        const SceneItem& item = overlay.items.front();
        check(!item.isImage, "the next overlay is text, never an image");
        check(item.x == openshow::scene::kStageNextBoxX &&
                  item.y == openshow::scene::kStageNextBoxY &&
                  item.width == openshow::scene::kStageNextBoxWidth &&
                  item.height == openshow::scene::kStageNextBoxHeight,
              "the next cue sits in the v1 next slot");
        check(item.fontSize > 54.0 && item.fontSize < 56.0,
              "the next cue starts at 55% of its resolved size");
        check(item.minFontSize > 13.0 && item.minFontSize < 14.0,
              "the next cue's minimum scales by the same 55%");
        check(item.lines.size() == 1 && item.lines.front() == "A line of words",
              "every selected next-cue word travels unchanged");
    }
    check(overlay.background.transparent, "the next overlay owns no background");

    Scene picturesOnly;
    picturesOnly.items.push_back(imageItem());
    check(openshow::scene::stageNextOverlay(picturesOnly).items.empty(),
          "an image-only next cue invents no words");

    Scene emptyText;
    emptyText.items.push_back(textItem(200.0, 200.0, 800.0, 300.0, 100.0));
    emptyText.items.front().lines = {"", ""};
    check(openshow::scene::stageNextOverlay(emptyText).items.empty(),
          "an empty next cue has an empty next region");
}

void exerciseOverlap() {
    Scene stageLayout;
    // The stage profile's default current box: 160,120,1600,560.
    stageLayout.items.push_back(textItem(160.0, 120.0, 1600.0, 560.0, 96.0));
    check(!openshow::scene::stageNextSlotOverlaps(stageLayout),
          "the default current box does not overlap the next slot");

    Scene customTall;
    customTall.items.push_back(textItem(160.0, 120.0, 1600.0, 800.0, 96.0));
    check(openshow::scene::stageNextSlotOverlaps(customTall),
          "a custom current box reaching into the next slot is reported");

    Scene pictureOverlap;
    SceneItem picture = imageItem();
    picture.x = 160.0;
    picture.y = 800.0;
    picture.width = 400.0;
    picture.height = 200.0;
    pictureOverlap.items.push_back(picture);
    check(!openshow::scene::stageNextSlotOverlaps(pictureOverlap),
          "a picture over the next slot is not a TEXT overlap");
}

}  // namespace

int main() {
    exerciseDisplayIdentity();
    exerciseNextOverlay();
    exerciseOverlap();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d stage policy check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("stage policy checks passed\n");
    return 0;
}