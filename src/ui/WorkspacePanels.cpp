#include "WorkspacePanels.h"

#include <QDir>
#include <QStandardPaths>

namespace openshow::ui {

namespace {

const char* const kLeftKey = "leftPanel";
const char* const kRightKey = "rightPanel";

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

}  // namespace openshow::ui
