#include "PowerPointImport.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace openshow::ui {

namespace {

QString cacheRoot() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath() + QStringLiteral("/openshow-cache");
    }
    return base + QStringLiteral("/pptx");
}

QString executableName() {
#ifdef Q_OS_WIN
    return QStringLiteral("pptx-render.exe");
#else
    return QStringLiteral("pptx-render");
#endif
}

}  // namespace

PowerPointImport::PowerPointImport(QObject* parent) : QObject(parent) {
    fTimeout = new QTimer(this);
    fTimeout->setSingleShot(true);
    connect(fTimeout, &QTimer::timeout, this, [this] {
        if (!fBusy) {
            return;
        }
        if (fProcess != nullptr) {
            fProcess->kill();
        }
        failWith(QStringLiteral("the renderer did not finish within 3 minutes"));
    });
}

PowerPointImport::~PowerPointImport() {
    if (fProcess != nullptr) {
        fProcess->kill();
        fProcess->waitForFinished(1000);
    }
}

QString PowerPointImport::binaryPath() const {
    const QString overridden = qEnvironmentVariable("OPENSHOW_PPTX_RENDER");
    if (!overridden.isEmpty()) {
        const QFileInfo info(overridden);
        if (!info.isFile() || !info.isExecutable()) {
            return {};
        }
        return info.absoluteFilePath();
    }
    const QString name = executableName();
    const QString app = QCoreApplication::applicationDirPath();
    QStringList candidates{
        app + QLatin1Char('/') + name,
        app + QStringLiteral("/../../cargo/release/") + name,
        app + QStringLiteral("/../cargo/release/") + name,
    };
#ifdef OPENSHOW_PPTX_RENDER_BIN
    candidates << QString::fromUtf8(OPENSHOW_PPTX_RENDER_BIN);
#endif
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("pptx-render"));
}

bool PowerPointImport::start(const QString& deckPath) {
    if (fBusy) {
        emit failed(QStringLiteral("another deck is already rendering"));
        return false;
    }
    const QString binary = binaryPath();
    if (binary.isEmpty()) {
        emit failed(QStringLiteral(
            "the PowerPoint renderer is not installed beside this build"));
        return false;
    }

    QFile deck(deckPath);
    if (!deck.open(QIODevice::ReadOnly)) {
        emit failed(QStringLiteral("cannot read '%1': %2")
                        .arg(deckPath, deck.errorString()));
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&deck)) {
        emit failed(QStringLiteral("cannot read '%1': %2")
                        .arg(deckPath, deck.errorString()));
        return false;
    }
    deck.close();

    fSource = QFileInfo(deckPath).fileName();
    fCacheDir = cacheRoot() + QLatin1Char('/') +
                QString::fromLatin1(hash.result().toHex());

    Deck cached;
    QString error;
    if (parseManifest(fCacheDir, &cached, &error)) {
        cached.source = fSource;
        setBusy(true);
        const Deck ready = cached;
        QTimer::singleShot(0, this, [this, ready] { finishWithDeck(ready); });
        return true;
    }

    QDir(fCacheDir).removeRecursively();
    if (!QDir().mkpath(fCacheDir)) {
        emit failed(QStringLiteral("cannot create the render cache '%1'")
                        .arg(fCacheDir));
        return false;
    }

    fProcess = new QProcess(this);
    fProcess->setProcessChannelMode(QProcess::SeparateChannels);
    connect(fProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError processError) {
                if (!fBusy) {
                    return;
                }
                if (processError == QProcess::FailedToStart) {
                    failWith(fProcess->errorString());
                }
            });
    connect(fProcess, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
                if (!fBusy) {
                    return;
                }
                const QString stderrText =
                    QString::fromLocal8Bit(fProcess->readAllStandardError()).trimmed();
                if (status != QProcess::NormalExit || exitCode != 0) {
                    failWith(stderrText.isEmpty()
                                 ? QStringLiteral("the renderer exited with code %1")
                                       .arg(exitCode)
                                 : stderrText);
                    return;
                }
                Deck rendered;
                QString manifestError;
                if (!parseManifest(fCacheDir, &rendered, &manifestError)) {
                    failWith(manifestError);
                    return;
                }
                rendered.source = fSource;
                finishWithDeck(rendered);
            });

    setBusy(true);
    const QStringList arguments{
        QStringLiteral("--input"), QFileInfo(deckPath).absoluteFilePath(),
        QStringLiteral("--out-dir"), fCacheDir,
        QStringLiteral("--width"), QStringLiteral("1920"),
        QStringLiteral("--height"), QStringLiteral("1080"),
    };
    fProcess->start(binary, arguments);
    fTimeout->start(180000);
    return true;
}

void PowerPointImport::cancel() {
    if (fProcess != nullptr) {
        fProcess->kill();
    }
    failWith(QStringLiteral("cancelled"));
}

void PowerPointImport::setBusy(bool busy) {
    if (fBusy == busy) {
        return;
    }
    fBusy = busy;
    if (!busy && fTimeout != nullptr) {
        fTimeout->stop();
    }
    emit busyChanged();
}

void PowerPointImport::finishWithDeck(const Deck& deck) {
    if (fProcess != nullptr) {
        fProcess->deleteLater();
        fProcess = nullptr;
    }
    setBusy(false);
    emit finished(deck);
}

void PowerPointImport::failWith(const QString& message) {
    if (fProcess != nullptr) {
        fProcess->deleteLater();
        fProcess = nullptr;
    }
    setBusy(false);
    emit failed(message);
}

bool PowerPointImport::parseManifest(const QString& directory, Deck* deck,
                                     QString* error) const {
    QFile file(directory + QStringLiteral("/manifest.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("the render did not produce a manifest");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("the render manifest is not valid JSON");
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toInt() != 1) {
        *error = QStringLiteral("the render manifest is an unknown format");
        return false;
    }
    const QJsonArray pages = root.value(QStringLiteral("pages")).toArray();
    if (pages.isEmpty()) {
        *error = QStringLiteral("the deck produced no visible pages");
        return false;
    }
    QVector<Page> parsed;
    for (const QJsonValue& value : pages) {
        const QJsonObject page = value.toObject();
        const QString name = page.value(QStringLiteral("file")).toString();
        const int width = page.value(QStringLiteral("width")).toInt();
        const int height = page.value(QStringLiteral("height")).toInt();
        if (name.isEmpty() || name.contains(QLatin1Char('/')) ||
            name.contains(QLatin1Char('\\')) || QFileInfo(name).fileName() != name ||
            width <= 0 || height <= 0 ||
            !QFileInfo(directory + QLatin1Char('/') + name).isFile()) {
            *error = QStringLiteral("the render manifest names a missing or unsafe page");
            return false;
        }
        parsed.push_back(Page{page.value(QStringLiteral("page")).toInt(), name, width,
                              height, page.value(QStringLiteral("skippedImages")).toInt()});
    }

    deck->cacheDir = directory;
    deck->pages = parsed;
    deck->source = root.value(QStringLiteral("source")).toString();
    deck->hiddenPages = root.value(QStringLiteral("hiddenPages")).toArray().size();
    deck->substitutions.clear();
    for (const QJsonValue& value : root.value(QStringLiteral("fontSubstitutions")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString requested = entry.value(QStringLiteral("requested")).toString();
        const QString selected = entry.value(QStringLiteral("selected")).toString();
        if (!requested.isEmpty() && !selected.isEmpty()) {
            deck->substitutions << QStringLiteral("%1 → %2").arg(requested, selected);
        }
    }
    return true;
}

}  // namespace openshow::ui
