#pragma once

#include <filesystem>

#include <QObject>
#include <QSettings>

namespace openshow::ui {

// The handful of app-level toggles the settings page (task 4.5) remembers
// across sessions: presenter number keys and window fullscreen. They live
// in the same workspace.ini as the panel widths, under their own keys,
// and follow the same injectable-path pattern so the fixtures stay
// hermetic. Defaults match the reference: number keys off (its setting
// is off until the operator enables it), windowed.
class AppSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool numberKeysPresent READ numberKeysPresent WRITE
                   setNumberKeysPresent NOTIFY settingsChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY settingsChanged)

public:
    explicit AppSettings(QObject* parent = nullptr,
                         std::filesystem::path settingsPath = {});

    bool numberKeysPresent() const { return fNumberKeys; }
    void setNumberKeysPresent(bool on);
    bool fullscreen() const { return fFullscreen; }
    void setFullscreen(bool on);

signals:
    void settingsChanged();

private:
    QSettings fSettings;
    bool fNumberKeys = false;
    bool fFullscreen = false;
};

}  // namespace openshow::ui
