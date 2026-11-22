/*
 * Task 4.1/4.2/4.5 fixtures: WorkspacePanels — the reference's panel
 * resize and drawer height, and AppSettings' persisted toggles, pinned
 * without a window (Qt Core only).
 *
 * The numbers are the measured ones (docs/reference.md §2/§4): default
 * 290, max 638, the 100.05/200.1 snap thresholds and the 270-310 home
 * snap; the drawer opens at 300 and snaps shut under 80. The boundaries
 * are the product — an operator dragging through the snap zone feels
 * exactly those jumps — so the exact endpoints get checked.
 */

#include <cstdio>
#include <filesystem>

#include <QCoreApplication>
#include <QSettings>

#include "WorkspacePanels.h"
#include "AppSettings.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::ui::WorkspacePanels;
using openshow::ui::AppSettings;

void exerciseSnap() {
    // Dragging to the window edge closes the panel to the handle.
    check(WorkspacePanels::widthFor(0) == WorkspacePanels::kHandleWidth &&
              WorkspacePanels::widthFor(100.04) == WorkspacePanels::kHandleWidth,
          "candidates under 100.05 collapse to the handle width");
    check(WorkspacePanels::widthFor(100.05) == 200.1 && WorkspacePanels::widthFor(150) == 200.1,
          "100.05-200.1 floors at 200.1");
    check(WorkspacePanels::widthFor(200.11) > 200.1, "just above the floor stays where dragged");

    // The home snap: 270-310 (exclusive ends) pulls back to 290.
    check(WorkspacePanels::widthFor(269.9) == 269.9, "just below the home zone is free");
    check(WorkspacePanels::widthFor(271) == 290 && WorkspacePanels::widthFor(290) == 290 &&
              WorkspacePanels::widthFor(309.9) == 290,
          "270-310 snaps home to 290");
    check(WorkspacePanels::widthFor(311) == 311, "just above the home zone is free");

    // And the clamp.
    check(WorkspacePanels::widthFor(638) == 638 && WorkspacePanels::widthFor(5000) == 638,
          "638 is the widest panel, come what may");
}

void exerciseSession(const std::filesystem::path& settingsPath) {
    {
        WorkspacePanels panels(nullptr, settingsPath);
        check(panels.leftWidth() == 290 && panels.rightWidth() == 290,
              "a first session opens both panels at the default");
        panels.resizeLeft(150);
        check(panels.leftWidth() == 200.1, "dragging left floors at 200.1");
        panels.resizeLeft(60);
        check(panels.leftWidth() == WorkspacePanels::kHandleWidth, "dragging on closes the panel");
        panels.clickLeft();
        check(panels.leftWidth() == 290, "a click on the closed strip restores the default");
        panels.clickLeft();
        check(panels.leftWidth() == 290, "a click on an open panel does nothing (no click-close)");
        panels.resizeRight(700);
        check(panels.rightWidth() == 638, "the right panel clamps at the max");
    }
    {   // The widths went to disk with the first object...
        WorkspacePanels panels(nullptr, settingsPath);
        check(panels.rightWidth() == 638, "an open panel's width survives the session");
    }
    {   // ...but a panel left closed reopens at the default (startup rule).
        WorkspacePanels panels(nullptr, settingsPath);
        panels.resizeLeft(0);
        check(panels.leftWidth() == WorkspacePanels::kHandleWidth, "closed on exit");
    }
    {
        WorkspacePanels panels(nullptr, settingsPath);
        check(panels.leftWidth() == 290, "a panel left closed reopens at the default");
    }
}

void exerciseDrawer(const std::filesystem::path& settingsPath) {
    check(WorkspacePanels::heightFor(0, 5000) == WorkspacePanels::kDrawerClosed &&
              WorkspacePanels::heightFor(79.9, 5000) == WorkspacePanels::kDrawerClosed,
          "a drawer dragged under twice its closed height snaps shut");
    check(WorkspacePanels::heightFor(81, 5000) == 81 &&
              WorkspacePanels::heightFor(5000, 900) == 900,
          "between the snap and the viewport the drawer follows the mouse");

    {
        WorkspacePanels panels(nullptr, settingsPath);
        check(panels.drawerHeight() == 300, "the drawer starts open at the default 300");
        panels.toggleDrawer();
        check(panels.drawerHeight() == 40, "Ctrl+D (or the strip click) shuts it to the bar");
        panels.toggleDrawer();
        check(panels.drawerHeight() == 300, "and back to 300");
        panels.resizeDrawer(500, 900);
        check(panels.drawerHeight() == 500, "a taller drag stays taller");
        panels.toggleDrawer();
        panels.toggleDrawer();
        check(panels.drawerHeight() == 500, "the reopen remembers the remembered height");
    }
    {   // An open height persists across restarts; the close-remembered
        // value does not (the reference store is per-session).
        WorkspacePanels panels(nullptr, settingsPath);
        check(panels.drawerHeight() == 500, "a session that saved 500 restarts at 500");
        panels.toggleDrawer();  // shut it, leaving 500 in the file
        check(panels.drawerHeight() == 40, "closed at exit");
    }
    {
        WorkspacePanels panels(nullptr, settingsPath);
        check(panels.drawerHeight() == 40, "the closed bar is how the next session starts");
        panels.toggleDrawer();
        check(panels.drawerHeight() == 300, "and it reopens at the default, not the old 500");
    }
}

}  // namespace

void exerciseAppSettings(const std::filesystem::path& settingsPath) {
    // Task 4.5: the settings page's persisted toggles. Defaults follow
    // the reference: number keys off, windowed. Task UI.3c adds the Show
    // page's thumbnail size step: middle of five, clamped in both
    // directions so no caller can park it on a step the page cannot draw.
    AppSettings settings(nullptr, settingsPath);
    check(!settings.numberKeysPresent() && !settings.fullscreen(),
          "app settings default to number keys off and windowed");
    check(settings.showThumbLevel() == 2, "thumbnails start at the middle step");
    settings.setNumberKeysPresent(true);
    settings.setFullscreen(true);
    settings.setShowThumbLevel(9);  // the clamp belongs to the setting, not the caller
    const AppSettings reopened(nullptr, settingsPath);
    check(reopened.numberKeysPresent() && reopened.fullscreen(),
          "both toggles are remembered by a fresh session");
    check(reopened.showThumbLevel() == AppSettings::kMaxThumbLevel,
          "an over-high size step is remembered clamped to the largest");
    AppSettings back(nullptr, settingsPath);
    back.setFullscreen(false);
    back.setShowThumbLevel(-3);
    const AppSettings again(nullptr, settingsPath);
    check(!again.fullscreen() && again.numberKeysPresent(),
          "switching one toggle back is remembered without disturbing the other");
    check(again.showThumbLevel() == AppSettings::kMinThumbLevel,
          "an under-low size step lands on the smallest instead");
}

// Milestone FO: the machine's layer matrix and routes are workspace state.
// Applying validates the whole answer, keeps the desired masks, never
// restores a destination as enabled, and an invalid/hand-edited block falls
// back to the role defaults instead of refusing to start.
void exerciseOutputRouting(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const std::filesystem::path settings = root / "routing.ini";
    std::filesystem::remove(settings);
    {
        AppSettings fresh(nullptr, settings);
        check(fresh.outputRouting().isEnabled(openshow::domain::OutputTarget::Audience) ==
                      false &&
                  fresh.outputRouting().mask(openshow::domain::OutputTarget::Broadcast) ==
                      openshow::domain::routingLayerBit(
                          openshow::domain::RoutingLayer::Words),
              "a fresh machine starts disabled with the FO.0 default masks");
        check(fresh.audienceMonitor() == -1 && fresh.stageMonitor() == -1 &&
                  fresh.broadcastSource() == QStringLiteral("OpenShow"),
              "a fresh machine has no chosen display and the default NDI source name");
        check(fresh.applyOutputRouting(QStringLiteral(
                  R"({"audience":{"enabled":true,"layers":["background","images","words"]},
                      "stage":{"enabled":true,"layers":["words","next"]}})")) &&
                  fresh.outputRoutingError().isEmpty(),
              "a whole valid matrix applies");
        check(!fresh.outputRouting().isEnabled(openshow::domain::OutputTarget::Stage) &&
                  fresh.outputRouting().mask(openshow::domain::OutputTarget::Stage) ==
                      (openshow::domain::routingLayerBit(
                           openshow::domain::RoutingLayer::Words) |
                       openshow::domain::routingLayerBit(
                           openshow::domain::RoutingLayer::Next)),
              "Apply keeps the desired masks and starts the column disabled");
        check(!fresh.applyOutputRouting(QStringLiteral(
                  R"({"broadcast":{"layers":["next"]}})")),
              "an illegal stage-only cell on the broadcast is refused");
        check(!fresh.outputRoutingError().isEmpty() &&
                  fresh.outputRouting().mask(openshow::domain::OutputTarget::Stage) ==
                      (openshow::domain::routingLayerBit(
                           openshow::domain::RoutingLayer::Words) |
                       openshow::domain::routingLayerBit(
                           openshow::domain::RoutingLayer::Next)),
              "a refusal reports why and retains the last accepted matrix");
        fresh.setStageMonitor(2);
        fresh.setAudienceMonitor(0);
        fresh.setBroadcastSource(QStringLiteral("  FOH lower third  "));
    }
    {
        AppSettings reopened(nullptr, settings);
        check(reopened.outputRouting().mask(openshow::domain::OutputTarget::Stage) ==
                      (openshow::domain::routingLayerBit(
                           openshow::domain::RoutingLayer::Words) |
                       openshow::domain::routingLayerBit(
                           openshow::domain::RoutingLayer::Next)) &&
                  !reopened.outputRouting().isEnabled(
                      openshow::domain::OutputTarget::Stage),
              "the desired matrix is remembered and the column starts disabled");
        check(reopened.stageMonitor() == 2 && reopened.audienceMonitor() == 0 &&
                  reopened.broadcastSource() == QStringLiteral("FOH lower third"),
              "routes and the NDI source name are remembered, trimmed");
        reopened.setBroadcastSource(QStringLiteral("   "));
        check(reopened.broadcastSource() == QStringLiteral("OpenShow"),
              "a blank NDI source name falls back rather than starting an unnamed stream");
    }
    {
        // A hand-edited ini naming an illegal cell is not a hard failure:
        // the matrix falls back to the role defaults, outputs stay off.
        QSettings raw(QString::fromStdString(settings.string()), QSettings::IniFormat);
        raw.setValue(QStringLiteral("outputRouting"),
                     QStringLiteral(R"({"audience":{"layers":["notes"]}})"));
        raw.sync();
        AppSettings edited(nullptr, settings);
        check(edited.outputRouting().mask(openshow::domain::OutputTarget::Audience) ==
                      openshow::domain::defaultRoutingMask(
                          openshow::domain::OutputTarget::Audience) &&
                  edited.outputRoutingJson().toStdString().find("notes") ==
                      std::string::npos,
              "an invalid saved matrix falls back to the defaults without a stray cell");
    }
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const std::filesystem::path settings =
        std::filesystem::temp_directory_path() /
        ("openshow-panels-" + std::to_string(QCoreApplication::applicationPid()) + ".ini");
    std::filesystem::remove(settings);
    exerciseSnap();
    exerciseSession(settings);
    exerciseDrawer(settings);
    exerciseAppSettings(settings);
    const std::filesystem::path routingRoot =
        std::filesystem::temp_directory_path() /
        ("openshow-routing-" + std::to_string(QCoreApplication::applicationPid()));
    exerciseOutputRouting(routingRoot);
    std::filesystem::remove_all(routingRoot);
    std::filesystem::remove(settings);
    if (gFailed) {
        return 1;
    }
    std::printf("workspace_panels_tests: the resize snaps to the reference\n");
    return 0;
}
