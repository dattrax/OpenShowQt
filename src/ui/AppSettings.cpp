#include "AppSettings.h"

#include <QStandardPaths>

#include <boost/json.hpp>

namespace openshow::ui {

namespace {

const char* const kNumberKeysKey = "numberKeysPresent";
const char* const kFullscreenKey = "fullscreen";
const char* const kShowThumbLevelKey = "showThumbLevel";
const char* const kOutputRoutingKey = "outputRouting";
const char* const kAudienceMonitorKey = "output.monitor.audience";
const char* const kStageMonitorKey = "output.monitor.stage";
const char* const kBroadcastSourceKey = "output.source.broadcast";

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
                         kMaxThumbLevel)),
      fAudienceMonitor(fSettings.value(QLatin1String(kAudienceMonitorKey), -1).toInt()),
      fStageMonitor(fSettings.value(QLatin1String(kStageMonitorKey), -1).toInt()),
      fBroadcastSource(fSettings.value(QLatin1String(kBroadcastSourceKey),
                                      QStringLiteral("OpenShow"))
                           .toString()) {
    // The saved matrix is read through the same contract the Apply verb
    // quotes: an invalid or hand-edited block falls back to the role
    // defaults instead of refusing to start, and enablement is always
    // dropped — a restart must never go on air by itself.
    const QString stored = fSettings.value(QLatin1String(kOutputRoutingKey)).toString();
    if (!stored.isEmpty()) {
        try {
            const boost::json::value parsed = boost::json::parse(stored.toStdString());
            openshow::domain::OutputRouting routing;
            if (openshow::domain::outputRoutingFromJson(parsed, &routing, nullptr)) {
                fRouting = routing;
            }
        } catch (const std::exception&) {
            // A corrupt block is not a reason to refuse to start; the role
            // defaults above are the answer.
        }
    }
    for (std::size_t i = 0; i < openshow::domain::kOutputTargetCount; ++i) {
        fRouting.enabled[i] = false;
    }
}

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

QString AppSettings::outputRoutingJson() const {
    return QString::fromStdString(
        boost::json::serialize(openshow::domain::outputRoutingToJson(fRouting)));
}

bool AppSettings::applyOutputRouting(const QString& json) {
    boost::json::value parsed;
    try {
        parsed = boost::json::parse(json.toStdString());
    } catch (const std::exception& error) {
        fOutputError = QStringLiteral("routing is not JSON: %1")
                           .arg(QString::fromUtf8(error.what()));
        emit settingsChanged();
        return false;
    }
    openshow::domain::OutputRouting routing;
    std::string why;
    if (!openshow::domain::outputRoutingFromJson(parsed, &routing, &why)) {
        fOutputError = QString::fromStdString(why);
        emit settingsChanged();
        return false;
    }
    // The desired masks are what a settings write keeps; whether a column is
    // ON is session state, so it is never written and a restart cannot go on
    // air by itself. The retained answer is the last ACCEPTED matrix.
    for (std::size_t i = 0; i < openshow::domain::kOutputTargetCount; ++i) {
        routing.enabled[i] = false;
    }
    fRouting = routing;
    fOutputError.clear();
    writeRouting();
    return true;
}

void AppSettings::writeRouting() {
    fSettings.setValue(QLatin1String(kOutputRoutingKey), outputRoutingJson());
    fSettings.sync();
    emit settingsChanged();
}

void AppSettings::setAudienceMonitor(int index) {
    if (fAudienceMonitor == index) {
        return;
    }
    fAudienceMonitor = index;
    fSettings.setValue(QLatin1String(kAudienceMonitorKey), index);
    fSettings.sync();
    emit settingsChanged();
}

void AppSettings::setStageMonitor(int index) {
    if (fStageMonitor == index) {
        return;
    }
    fStageMonitor = index;
    fSettings.setValue(QLatin1String(kStageMonitorKey), index);
    fSettings.sync();
    emit settingsChanged();
}

void AppSettings::setBroadcastSource(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed == fBroadcastSource) {
        return;
    }
    fBroadcastSource = trimmed.isEmpty() ? QStringLiteral("OpenShow") : trimmed;
    fSettings.setValue(QLatin1String(kBroadcastSourceKey), fBroadcastSource);
    fSettings.sync();
    emit settingsChanged();
}

}  // namespace openshow::ui
