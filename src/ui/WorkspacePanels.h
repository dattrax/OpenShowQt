/*
 * The workspace shell's resizable side panels (M4 task 4.1).
 *
 * In the reference, each side panel is itself the drag target: a mousedown
 * within handleWidth of its inner edge starts a resize, and every candidate
 * width during the drag runs through one snap function (Resizeable.svelte,
 * docs/reference.md §2, verified by dragging in the running app): below
 * 100.05 the panel collapses to the handle width, 100.05–200.1 floors at
 * 200.1, 270–310 snaps to the default 290, and above 290 × 2.2 = 638 is
 * clamped. A click on the strip only ever restores a collapsed panel —
 * the reference deliberately never closes one on a click — and widths
 * persist across sessions, except that a panel left closed reopens at the
 * default on startup.
 *
 * The maths lives here, pure and fixture-tested; the QML side maps mouse
 * coordinates to candidate widths and binds the panel rectangles to
 * leftWidth/rightWidth.
 */
#pragma once

#include <filesystem>

#include <QObject>
#include <QSettings>

namespace openshow::ui {

class WorkspacePanels : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal leftWidth READ leftWidth NOTIFY geometryChanged)
    Q_PROPERTY(qreal rightWidth READ rightWidth NOTIFY geometryChanged)
    Q_PROPERTY(qreal drawerHeight READ drawerHeight NOTIFY geometryChanged)

public:
    // Measured at 1680x1020 (docs/reference.md §2): the panel is 290 wide,
    // 286 of content plus the 4px drag handle inside.
    static inline constexpr qreal kDefaultWidth = 290.0;
    static inline constexpr qreal kMaxWidth = 638.0;       // 290 * 2.2
    static inline constexpr qreal kMinFloor = 200.1;       // 290 * 0.69
    static inline constexpr qreal kCollapseLimit = 100.05; // 290 * 0.69 / 2
    static inline constexpr qreal kHandleWidth = 4.0;

    // settingsPath is a seam for fixtures; empty means the user's
    // workspace.ini under the application config directory.
    explicit WorkspacePanels(QObject* parent = nullptr,
                             std::filesystem::path settingsPath = {});

    // What the reference's snap does to a candidate width. Pure, so the
    // boundary values are pinned without a window.
    static qreal widthFor(qreal candidate);

    qreal leftWidth() const { return fLeftWidth; }
    qreal rightWidth() const { return fRightWidth; }

    // Called with the raw candidate width (window coordinates) while the
    // handle is dragged; the snap decides what the panel actually becomes.
    Q_INVOKABLE void resizeLeft(qreal candidate);
    Q_INVOKABLE void resizeRight(qreal candidate);

    // Strip click: restore a collapsed panel to the default, and otherwise
    // do nothing (click-to-close is deliberately not a thing).
    Q_INVOKABLE void clickLeft();
    Q_INVOKABLE void clickRight();

    // --- the library drawer (task 4.2) ---

    // Bottom library strip: 300 open, a 40px tab bar closed. Its drag has
    // its own rules (Drawer.svelte getHeight): below twice the closed
    // height the drag snaps shut, and it clamps at the available
    // viewport; closing stores the open height and the reopen goes to
    // max(stored, 300). A session that starts closed opens at the
    // default, because the reference's "stored" survives a close, not
    // a restart.
    static inline constexpr qreal kDrawerHeight = 300.0;
    static inline constexpr qreal kDrawerClosed = 40.0;

    // The drawer's drag snap, pure like widthFor.
    static qreal heightFor(qreal candidate, qreal maxHeight);

    qreal drawerHeight() const { return fDrawerHeight; }

    Q_INVOKABLE void resizeDrawer(qreal candidate, qreal maxHeight);
    // Strip click / Ctrl+D.
    Q_INVOKABLE void toggleDrawer();

signals:
    void geometryChanged();

private:
    void apply(const char* key, qreal* slot, qreal candidate);

    qreal fLeftWidth = kDefaultWidth;
    qreal fRightWidth = kDefaultWidth;
    qreal fDrawerHeight = kDrawerHeight;
    qreal fDrawerStored = 0.0;  // height remembered by a close; 0 = none
    QSettings fSettings;
};

}  // namespace openshow::ui
