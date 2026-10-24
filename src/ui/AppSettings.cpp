#include "AppSettings.h"

#include <QStandardPaths>

namespace openshow::ui {

namespace {

const char* const kNumberKeysKey = "numberKeysPresent";
const char* const kFullscreenKey = "fullscreen";

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
      fFullscreen(fSettings.value(QLatin1String(kFullscreenKey), false).toBool()) {}

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

}  // namespace openshow::ui
