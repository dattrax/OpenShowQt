#pragma once

#include <filesystem>

#include <QObject>
#include <QSettings>

namespace openshow::ui {

// The handful of app-level toggles the settings page (task 4.5) remembers
// across sessions: presenter number keys and window fullscreen, plus the
// Show page's thumbnail size step (task UI.3c). They live in the same
// workspace.ini as the panel widths, under their own keys, and follow the
// same injectable-path pattern so the fixtures stay hermetic. Defaults
// match the reference: number keys off (its setting is off until the
// operator enables it), windowed, thumbnails at the middle of five steps
// (the size the grid had before the control existed).
class AppSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool numberKeysPresent READ numberKeysPresent WRITE
                   setNumberKeysPresent NOTIFY settingsChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY settingsChanged)
    Q_PROPERTY(int showThumbLevel READ showThumbLevel WRITE
                   setShowThumbLevel NOTIFY settingsChanged)

public:
    // The Show page owns the step table (docs/reference.md §4: the reference
    // runs a zoom slider; ours is five discrete steps and step 2 is today's
    // cell). This half of the contract is the range the value never leaves
    // and the step a fresh session starts on.
    static constexpr int kMinThumbLevel = 0;
    static constexpr int kMaxThumbLevel = 4;
    static constexpr int kDefaultThumbLevel = 2;

    explicit AppSettings(QObject* parent = nullptr,
                         std::filesystem::path settingsPath = {});

    bool numberKeysPresent() const { return fNumberKeys; }
    void setNumberKeysPresent(bool on);
    bool fullscreen() const { return fFullscreen; }
    void setFullscreen(bool on);
    int showThumbLevel() const { return fThumbLevel; }
    void setShowThumbLevel(int level);

signals:
    void settingsChanged();

private:
    QSettings fSettings;
    bool fNumberKeys = false;
    bool fFullscreen = false;
    int fThumbLevel = kDefaultThumbLevel;
};

}  // namespace openshow::ui
