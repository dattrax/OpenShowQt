#include "WorkspacePanels.h"

#include <QDir>
#include <QStandardPaths>

namespace openshow::ui {

namespace {

const char* const kLeftKey = "leftPanel";
const char* const kRightKey = "rightPanel";
const char* const kDrawerKey = "drawerHeight";

std::filesystem::path defaultSettingsPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return std::filesystem::path(dir.toStdString()) / "workspace.ini";
}

}  // namespace

WorkspacePanels::WorkspacePanels(QObject* parent, std::filesystem::path settingsPath)
    : QObject(parent),
      fSettings(QString::fromStdString((settingsPath.empty() ? defaultSettingsPath()
                                                              : settingsPath)
                                           .string()),
                QSettings::IniFormat) {
    // A panel left closed on exit reopens at the default (the reference's
    // startup rule); anything out of range re-clamps through the same snap
    // the drag uses, so a hand-edited file cannot smuggle in a bad width.
    const auto stored = [this](const char* key) {
        const qreal raw = fSettings.value(QLatin1String(key), kDefaultWidth).toReal();
        return raw <= kHandleWidth ? kDefaultWidth : widthFor(raw);
    };
    fLeftWidth = stored(kLeftKey);
    fRightWidth = stored(kRightKey);
    // The drawer persists its height (open or closed); a stored value
    // under the closed height means a hand-edited file and clamps shut,
    // and the reopen memory starts empty like a fresh session's.
    const qreal drawer = fSettings.value(QLatin1String(kDrawerKey), kDrawerHeight).toReal();
    fDrawerHeight = drawer < kDrawerClosed ? kDrawerClosed : drawer;
}

qreal WorkspacePanels::widthFor(qreal candidate) {
    // Resizeable.svelte getWidth(), boundary-for-boundary.
    if (candidate < kCollapseLimit) return kHandleWidth;  // closed
    if (candidate < kMinFloor) return kMinFloor;          // floor
    if (candidate > kDefaultWidth - 20.0 && candidate < kDefaultWidth + 20.0) {
        return kDefaultWidth;                             // home snap
    }
    if (candidate > kMaxWidth) return kMaxWidth;          // clamp
    return candidate;
}

void WorkspacePanels::apply(const char* key, qreal* slot, qreal candidate) {
    const qreal snapped = widthFor(candidate);
    if (qFuzzyCompare(*slot, snapped)) {
        return;
    }
    *slot = snapped;
    fSettings.setValue(QLatin1String(key), snapped);
    emit geometryChanged();
}

void WorkspacePanels::resizeLeft(qreal candidate) {
    apply(kLeftKey, &fLeftWidth, candidate);
}

void WorkspacePanels::resizeRight(qreal candidate) {
    apply(kRightKey, &fRightWidth, candidate);
}

void WorkspacePanels::clickLeft() {
    if (fLeftWidth > kHandleWidth) {
        return;  // open panels ignore clicks; only closed ones restore
    }
    apply(kLeftKey, &fLeftWidth, kDefaultWidth);
}

void WorkspacePanels::clickRight() {
    if (fRightWidth > kHandleWidth) {
        return;
    }
    apply(kRightKey, &fRightWidth, kDefaultWidth);
}

qreal WorkspacePanels::heightFor(qreal candidate, qreal maxHeight) {
    // Drawer.svelte getHeight(): shut below twice the closed height,
    // clamp at the viewport the caller measured.
    if (candidate < kDrawerClosed * 2.0) {
        return kDrawerClosed;
    }
    if (maxHeight > 0.0 && candidate > maxHeight) {
        return maxHeight;
    }
    return candidate;
}

void WorkspacePanels::resizeDrawer(qreal candidate, qreal maxHeight) {
    const qreal snapped = heightFor(candidate, maxHeight);
    if (qFuzzyCompare(fDrawerHeight, snapped)) {
        return;
    }
    fDrawerHeight = snapped;
    fSettings.setValue(QLatin1String(kDrawerKey), snapped);
    emit geometryChanged();
}

void WorkspacePanels::toggleDrawer() {
    if (fDrawerHeight > kDrawerClosed) {
        fDrawerStored = fDrawerHeight;  // remembered for the reopen
        fDrawerHeight = kDrawerClosed;
    } else {
        fDrawerHeight = fDrawerStored < kDrawerHeight ? kDrawerHeight : fDrawerStored;
        fDrawerStored = 0.0;
    }
    fSettings.setValue(QLatin1String(kDrawerKey), fDrawerHeight);
    emit geometryChanged();
}

}  // namespace openshow::ui
