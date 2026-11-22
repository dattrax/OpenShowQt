#pragma once

#include <filesystem>

#include <QObject>
#include <QSettings>
#include <QString>

#include "openshow/domain/outputrouting.h"

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
    // Milestone FO: the machine's output routes and layer matrix. These are
    // workspace settings, never portable service/theme state. `enabled` is
    // deliberately not persisted: a restart always begins with every
    // destination off, and an operator re-enables explicitly.
    Q_PROPERTY(QString outputRoutingJson READ outputRoutingJson NOTIFY settingsChanged)
    Q_PROPERTY(QString outputRoutingError READ outputRoutingError NOTIFY settingsChanged)
    Q_PROPERTY(int audienceMonitor READ audienceMonitor WRITE setAudienceMonitor NOTIFY
                   settingsChanged)
    Q_PROPERTY(int stageMonitor READ stageMonitor WRITE setStageMonitor NOTIFY settingsChanged)
    // The connection-scoped identity the saved stage index named when it was
    // chosen (stage delivery FO.5a.2). On reopen, a saved index whose panel
    // identity no longer matches is STALE: the dialog requires an explicit
    // reselection instead of silently using whatever now occupies the index.
    Q_PROPERTY(QString stageMonitorIdentity READ stageMonitorIdentity
               WRITE setStageMonitorIdentity NOTIFY settingsChanged)
    Q_PROPERTY(QString broadcastSource READ broadcastSource WRITE setBroadcastSource NOTIFY
                   settingsChanged)

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

    // The desired masks as the settings/RPC JSON. Read is cheap and always
    // returns this machine's answer; `applyOutputRouting` validates the
    // WHOLE configuration before writing anything and answers false with
    // `outputRoutingError` naming the refusal. A successful apply stores the
    // masks with every destination disabled and emits settingsChanged.
    QString outputRoutingJson() const;
    QString outputRoutingError() const { return fOutputError; }
    Q_INVOKABLE bool applyOutputRouting(const QString& json);
    openshow::domain::OutputRouting outputRouting() const { return fRouting; }

    // Display/source routes are machine-local too. -1 means "not chosen".
    int audienceMonitor() const { return fAudienceMonitor; }
    void setAudienceMonitor(int index);
    int stageMonitor() const { return fStageMonitor; }
    QString stageMonitorIdentity() const { return fStageMonitorIdentity; }
    void setStageMonitor(int index);
    void setStageMonitorIdentity(const QString& identity);
    QString broadcastSource() const { return fBroadcastSource; }
    void setBroadcastSource(const QString& name);

signals:
    void settingsChanged();

private:
    void writeRouting();

    QSettings fSettings;
    bool fNumberKeys = false;
    bool fFullscreen = false;
    int fThumbLevel = kDefaultThumbLevel;
    openshow::domain::OutputRouting fRouting;
    QString fOutputError;
    int fAudienceMonitor = -1;
    int fStageMonitor = -1;
    QString fStageMonitorIdentity;
    QString fBroadcastSource = QStringLiteral("OpenShow");
};

}  // namespace openshow::ui
