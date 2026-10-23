/*
 * WorkerController: the frontend side of task 1.2 supervision.
 *
 * Spawns openshow-worker --socket PATH, connects with QLocalSocket, runs
 * the JSON-RPC handshake through the shared openshow::protocol framing,
 * and keeps the QML status properties updated with output.status polls.
 * The same session operations the UI calls are what the RPC exposes:
 * layer toggles now, scenes later.
 *
 * Lifecycle per PLAN.md: the worker lives only while connected. Closing
 * the UI sends "shutdown"; a worker that dies or hangs anyway is
 * terminated, and its state is reported honestly.
 */

#pragma once

#include <boost/json.hpp>

#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

#include "openshow/protocol/protocol.h"

namespace openshow::ui {

class WorkerController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY stateChanged)
    Q_PROPERTY(bool outputLive READ outputLive NOTIFY stateChanged)
    Q_PROPERTY(qint64 frames READ frames NOTIFY outputStatusChanged)
    Q_PROPERTY(qint64 revision READ revision NOTIFY outputStatusChanged)
    Q_PROPERTY(QString device READ device NOTIFY outputStatusChanged)
    Q_PROPERTY(QVariantMap layers READ layers NOTIFY outputStatusChanged)
    // Destination control (task 1.3): -1 windowed, >=0 display index, plus
    // the enumerated panels for the picker.
    Q_PROPERTY(qint64 currentMonitor READ currentMonitor NOTIFY outputStatusChanged)
    Q_PROPERTY(QVariantList displays READ displays NOTIFY displaysChanged)

public:
    explicit WorkerController(QObject* parent = nullptr);
    ~WorkerController() override;

    // Spawns the worker and starts connecting. Called once at startup.
    void start();

    // Send "shutdown" and reap; escalating to terminate/kill if the
    // worker ignores its frontend. Connected to the UI window closing.
    Q_INVOKABLE void shutdownNow();

    // One output.status round trip; also drives the periodic poll.
    // Move the audience output (-1 windowed, >=0 display index).
    Q_INVOKABLE void selectDisplay(qint64 index);

    Q_INVOKABLE void setLayerEnabled(const QString& name, bool enabled);

    QString statusText() const { return fStatusText; }
    bool isConnected() const { return fConnected; }
    bool outputLive() const { return fConnected && !fOutputStopped; }
    qint64 frames() const { return fFrames; }
    qint64 revision() const { return fRevision; }
    QString device() const { return fDevice; }
    QVariantMap layers() const { return fLayers; }
    qint64 currentMonitor() const { return fCurrentMonitor; }
    QVariantList displays() const { return fDisplays; }

signals:
    void stateChanged();
    void outputStatusChanged();
    void displaysChanged();

private slots:
    void tryConnect();
    void onSocketReady();
    void onSocketConnected();
    void onSocketDisconnected();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void pollStatus();

private:
    void sendRequest(const char* method, boost::json::object params);
    void drainMessages();
    void handleReply(const boost::json::object& message);
    void setStatus(const QString& text);

    QProcess fProcess;
    QLocalSocket fSocket;
    openshow::protocol::Decoder fDecoder;
    QString fSocketPath;
    QTimer fConnectRetry;
    int fConnectAttempts = 0;

    std::int64_t fNextId = 1;
    bool fHandshakeDone = false;
    bool fConnected = false;
    bool fOutputStopped = false;
    bool fShutdownRequested = false;

    QString fStatusText = QStringLiteral("worker not started");
    qint64 fFrames = 0;
    qint64 fRevision = 0;
    QString fDevice;
    QVariantMap fLayers;
    qint64 fCurrentMonitor = -1;
    QVariantList fDisplays;
};

}  // namespace openshow::ui
