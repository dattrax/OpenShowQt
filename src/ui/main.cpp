/*
 * openshow-ui: the Qt 6 Quick operator frontend.
 *
 * Task 1.2 adds worker supervision: the WorkerController spawns the
 * openshow-worker process over its Unix socket, runs the handshake and
 * exposes live output state (plus layer commands) to QML. Closing the
 * window shuts the worker down.
 */

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QTimer>

#include <atomic>
#include <csignal>

#include "openshow/domain/version.h"
#include "openshow/protocol/protocol.h"

#include "DocumentController.h"
#include "WorkerController.h"

namespace {

// SIGTERM/SIGINT end the event loop the normal way, so aboutToQuit still
// runs and the worker gets its graceful "shutdown" instead of an EOF.
std::atomic<bool> gSignalQuit{false};

void handleQuitSignal(int) { gSignalQuit.store(true); }

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("openshow-ui");
    app.setApplicationVersion(QString::fromLatin1(openshow::domain::applicationVersion()));

    openshow::ui::WorkerController worker;
    worker.start();

    // Task 3.3: the editable documents. Constructed after the application
    // object so QStandardPaths resolves; the lyric panel binds to it.
    openshow::ui::DocumentController documents;

    QObject::connect(&app, &QGuiApplication::aboutToQuit,
                     [&worker] { worker.shutdownNow(); });

    std::signal(SIGTERM, handleQuitSignal);
    std::signal(SIGINT, handleQuitSignal);
    QTimer signalPoll;
    QObject::connect(&signalPoll, &QTimer::timeout, [&app] {
        if (gSignalQuit.load()) {
            app.quit();
        }
    });
    signalPoll.start(100);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("applicationVersion", app.applicationVersion());
    engine.rootContext()->setContextProperty("workerOffer",
                                            QString::fromStdString(
                                                openshow::protocol::offerPayload()));
    engine.rootContext()->setContextProperty("worker", &worker);
    engine.rootContext()->setContextProperty("docs", &documents);

    engine.loadFromModule("OpenShow", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
