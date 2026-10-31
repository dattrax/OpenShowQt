#include "AppSettings.h"

#include <QStandardPaths>

namespace openshow::ui {

namespace {

const char* const kNumberKeysKey = "numberKeysPresent";
const char* const kFullscreenKey = "fullscreen";
const char* const kShowThumbLevelKey = "showThumbLevel";

std::filesystem::path defaultSettingsPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return std::filesystem::path(dir.toStdString()) / "workspace.ini";
}

}  // namespace

AppSettings::AppSettings(QObject* parent, std::filesystem::path settingsPath)
    : QObject(parent),
      fSettings(QString::fromStdString((settingsPath.empty() ? defaultSettingsPath()
                                                             : settingsPath)
                                           .string()),
                QSettings::IniFormat),
      fNumberKeys(fSettings.value(QLatin1String(kNumberKeysKey), false).toBool()),
      fFullscreen(fSettings.value(QLatin1String(kFullscreenKey), false).toBool()),
      // The level is read clamped, so a hand-edited or future-version ini
      // value still lands on a step the Show page can render.
      fThumbLevel(qBound(kMinThumbLevel,
                         fSettings.value(QLatin1String(kShowThumbLevelKey),
                                         kDefaultThumbLevel)
                             .toInt(),
                         kMaxThumbLevel)) {}

void AppSettings::setNumberKeysPresent(bool on) {
    if (fNumberKeys == on) {
        return;
    }
    fNumberKeys = on;
    fSettings.setValue(QLatin1String(kNumberKeysKey), on);
    fSettings.sync();  // toggles are rare; make the next session see it
    emit settingsChanged();
}

void AppSettings::setFullscreen(bool on) {
    if (fFullscreen == on) {
        return;
    }
    fFullscreen = on;
    fSettings.setValue(QLatin1String(kFullscreenKey), on);
    fSettings.sync();
    emit settingsChanged();
}

void AppSettings::setShowThumbLevel(int level) {
    const int clamped = qBound(kMinThumbLevel, level, kMaxThumbLevel);
    if (fThumbLevel == clamped) {
        return;
    }
    fThumbLevel = clamped;
    fSettings.setValue(QLatin1String(kShowThumbLevelKey), clamped);
    fSettings.sync();
    emit settingsChanged();
}

}  // namespace openshow::ui
