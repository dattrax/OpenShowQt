/*
 * Task 4.1 fixtures: WorkspacePanels — the reference's panel resize,
 * pinned without a window (Qt Core only).
 *
 * The numbers are the measured ones (docs/reference.md §2): default 290,
 * max 638, the 100.05/200.1 snap thresholds and the 270-310 home snap.
 * The boundaries are the product — an operator dragging through the snap
 * zone feels exactly those jumps — so the exact endpoints get checked.
 */

#include <cstdio>
#include <filesystem>

#include <QCoreApplication>

#include "WorkspacePanels.h"

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

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const std::filesystem::path settings =
        std::filesystem::temp_directory_path() /
        ("openshow-panels-" + std::to_string(QCoreApplication::applicationPid()) + ".ini");
    std::filesystem::remove(settings);
    exerciseSnap();
    exerciseSession(settings);
    std::filesystem::remove(settings);
    if (gFailed) {
        return 1;
    }
    std::printf("workspace_panels_tests: the resize snaps to the reference\n");
    return 0;
}
