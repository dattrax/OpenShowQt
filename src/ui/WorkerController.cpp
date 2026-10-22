/*
 * WorkerController implementation. Notes:
 *
 * - The worker path comes from the build ($<TARGET_FILE>); packaging can
 *   point it next to the UI later (task 6.3).
 * - The socket lives under XDG_RUNTIME_DIR with the UI pid, so two UIs
 *   never collide and bind has a fresh path (the worker also unlinks a
 *   stale one).
 * - QLocalSocket with a path containing '/' connects to exactly the file
 *   the worker bound, using the same framing both sides share.
 */

#include "WorkerController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTimer>

#include <chrono>

namespace openshow::ui {

namespace proto = openshow::protocol;

namespace {

constexpr int kConnectRetryMs = 100;
constexpr int kStatusPollMs = 500;

}  // namespace

WorkerController::WorkerController(QObject* parent) : QObject(parent) {
    fSocketPath = QDir(qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath()))
                      .filePath(QStringLiteral("openshow-ui-%1.sock")
                                    .arg(QCoreApplication::applicationPid()));

    connect(&fSocket, &QLocalSocket::connected, this, &WorkerController::onSocketConnected);
    connect(&fSocket, &QLocalSocket::disconnected, this,
            &WorkerController::onSocketDisconnected);
    connect(&fSocket, &QLocalSocket::readyRead, this, &WorkerController::onSocketReady);
    // The worker binds its socket a moment after we spawn it, so the first
    // connect attempts fail; retry on a short timer until it answers, the
    // worker dies, or patience runs out.
    fConnectRetry.setInterval(kConnectRetryMs);
    connect(&fConnectRetry, &QTimer::timeout, this, &WorkerController::tryConnect);
    connect(&fProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setStatus(QStringLiteral("worker failed to start"));
        }
    });
    connect(&fProcess, &QProcess::finished, this, &WorkerController::onProcessFinished);
}

WorkerController::~WorkerController() {
    if (fProcess.state() != QProcess::NotRunning) {
        fProcess.kill();
        fProcess.waitForFinished(1000);
    }
    QFile::remove(fSocketPath);
}

void WorkerController::start() {
#ifdef OPENSHOW_WORKER_PATH
    const QString workerPath = QStringLiteral(OPENSHOW_WORKER_PATH);
#else
    const QString workerPath =
        QCoreApplication::applicationDirPath() + QStringLiteral("/openshow-worker");
#endif
    setStatus(QStringLiteral("starting worker..."));
    QStringList arguments{QStringLiteral("--socket"), fSocketPath};
    // Development hook, e.g. OPENSHOW_WORKER_EXTRA_ARGS="--monitor 1" to
    // send the audience output to the second display while debugging.
    const QString extra = qEnvironmentVariable("OPENSHOW_WORKER_EXTRA_ARGS");
    if (!extra.isEmpty()) {
        arguments.append(extra.split(u' ', Qt::SkipEmptyParts));
    }
    // Forward the worker's output into the UI's own log so supervision is
    // observable (M6.2 will turn this into proper diagnostics).
    fProcess.setProcessChannelMode(QProcess::ForwardedChannels);
    fProcess.start(workerPath, arguments);
    fConnectAttempts = 0;
    fConnectRetry.start();
    tryConnect();
}

void WorkerController::tryConnect() {
    const QLocalSocket::LocalSocketState state = fSocket.state();
    if (state == QLocalSocket::ConnectedState || state == QLocalSocket::ConnectingState) {
        return;
    }
    if (fShutdownRequested || fProcess.state() == QProcess::NotRunning) {
        fConnectRetry.stop();
        return;
    }
    if (++fConnectAttempts > 100) {  // ten seconds of patience
        fConnectRetry.stop();
        setStatus(QStringLiteral("could not reach the worker on %1").arg(fSocketPath));
        return;
    }
    fSocket.connectToServer(fSocketPath);
}

void WorkerController::onSocketConnected() {
    fConnectRetry.stop();
    fConnected = true;
    setStatus(QStringLiteral("handshaking..."));
    boost::json::object params = proto::helloParams("openshow-ui", proto::kProtocolMajor,
                                                    proto::kProtocolMinor);
    sendRequest("hello", std::move(params));
    emit stateChanged();
}

void WorkerController::onSocketDisconnected() {
    fConnected = false;
    fHandshakeDone = false;
    emit stateChanged();
    if (!fShutdownRequested) {
        setStatus(QStringLiteral("worker connection lost"));
    }
}

void WorkerController::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    fConnected = false;
    fOutputStopped = true;
    emit stateChanged();
    if (fShutdownRequested) {
        setStatus(QStringLiteral("worker stopped (exit %1)").arg(exitCode));
    } else if (exitStatus == QProcess::CrashExit) {
        setStatus(QStringLiteral("worker crashed"));
    } else {
        setStatus(QStringLiteral("worker exited (%1)").arg(exitCode));
    }
}

void WorkerController::onSocketReady() {
    const QByteArray incoming = fSocket.readAll();
    fDecoder.feed(std::string_view(incoming.constData(),
                                   static_cast<std::size_t>(incoming.size())));
    drainMessages();
}

void WorkerController::drainMessages() {
    boost::json::value message;
    std::string error;
    while (fDecoder.next(&message, &error)) {
        if (message.is_object()) {
            handleReply(message.as_object());
        }
    }
    if (fDecoder.failed()) {
        setStatus(QStringLiteral("worker broke the protocol: %1")
                      .arg(QString::fromStdString(error)));
        fSocket.disconnectFromServer();
    }
}

void WorkerController::handleReply(const boost::json::object& message) {
    const auto error = message.find("error");
    if (error != message.end() && error->value().is_object()) {
        const boost::json::object& reported = error->value().as_object();
        const auto text = reported.find("message");
        setStatus(QStringLiteral("worker error: %1")
                      .arg(text != reported.end() && text->value().is_string()
                               ? QString::fromStdString(
                                     std::string(text->value().as_string()))
                               : QStringLiteral("unspecified")));
        return;
    }
    const auto result = message.find("result");
    if (result == message.end() || !result->value().is_object()) {
        return;
    }
    const boost::json::object& payload = result->value().as_object();

    if (payload.contains("application")) {  // the hello answer
        fHandshakeDone = true;
        const boost::json::object& negotiated = payload.at("protocol").as_object();
        setStatus(QStringLiteral("connected to worker %1 (protocol %2.%3)")
                      .arg(QString::fromStdString(
                               std::string(payload.at("version").as_string())),
                           QString::number(static_cast<qlonglong>(
                               negotiated.at("major").to_number<std::uint64_t>())),
                           QString::number(static_cast<qlonglong>(
                               negotiated.at("minor").to_number<std::uint64_t>()))));
        // Ask for the first status immediately, then keep it fresh while
        // connected; the poll stops itself once the socket is gone.
        QTimer::singleShot(0, this, &WorkerController::pollStatus);
        auto* poll = new QTimer(this);
        poll->setInterval(kStatusPollMs);
        connect(poll, &QTimer::timeout, this, [this, poll] {
            if (!fConnected) {
                poll->stop();
                poll->deleteLater();
                return;
            }
            pollStatus();
        });
        poll->start();
        return;
    }

    if (payload.contains("frames")) {  // output.status
        fFrames = static_cast<qint64>(payload.at("frames").to_number<std::uint64_t>());
        fRevision = static_cast<qint64>(payload.at("revision").to_number<std::uint64_t>());
        if (payload.contains("device")) {
            fDevice = QString::fromStdString(std::string(payload.at("device").as_string()));
        }
        QVariantMap layers;
        for (const auto& entry : payload.at("layers").as_object()) {
            layers.insert(QString::fromStdString(std::string(entry.key())),
                          entry.value().as_bool());
        }
        fLayers = layers;
        emit outputStatusChanged();
    }
    // layer.setEnabled acks and shutdown ok carry no display state of
    // their own; the next status poll confirms them.
}

void WorkerController::pollStatus() {
    if (fConnected && fHandshakeDone) {
        sendRequest("output.status", {});
    }
}

void WorkerController::setLayerEnabled(const QString& name, bool enabled) {
    if (!fConnected || !fHandshakeDone) {
        return;
    }
    sendRequest("layer.setEnabled",
                boost::json::object{{"layer", name.toStdString()},
                                    {"enabled", enabled}});
}

void WorkerController::sendRequest(const char* method, boost::json::object params) {
    std::string wire;
    proto::appendFrame(proto::makeRequest(fNextId++, method, std::move(params)), &wire);
    fSocket.write(wire.data(), static_cast<qint64>(wire.size()));
}

void WorkerController::shutdownNow() {
    if (fShutdownRequested) {
        return;
    }
    fShutdownRequested = true;
    if (fConnected && fHandshakeDone) {
        sendRequest("shutdown", {});
        fSocket.flush();
    }
    // Grace first: a healthy worker exits on its own the moment the
    // request lands. Anything left after two seconds is ours to kill.
    if (!fProcess.waitForFinished(2000)) {
        fProcess.terminate();
        if (!fProcess.waitForFinished(1000)) {
            fProcess.kill();
            fProcess.waitForFinished(1000);
        }
    }
    setStatus(QStringLiteral("worker stopped"));
}

void WorkerController::setStatus(const QString& text) {
    fStatusText = text;
    emit stateChanged();
}

}  // namespace openshow::ui
