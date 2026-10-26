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
#include <QQuickStyle>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <atomic>
#include <csignal>

#include "openshow/domain/version.h"
#include "openshow/protocol/protocol.h"

#include "DocumentController.h"
#include "WorkerController.h"
#include "WorkspacePanels.h"
#include "AppSettings.h"

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

    // M4: the reference is a flat dark UI; the default light controls sit
    // wrong on its palette. Material's dark theme, with the reference's
    // accent (--secondary #f0008c) attached on the root in QML.
    QQuickStyle::setStyle("Material");

    openshow::ui::WorkerController worker;
    worker.start();

    // Task 3.3: the editable documents. Constructed after the application
    // object so QStandardPaths resolves; the lyric panel binds to it.
    openshow::ui::DocumentController documents;

    // Task 6.14b (review R14): scratch persistence runs on the bounded,
    // coalescing executor — the UI thread only ever pays for a snapshot
    // copy, an in-flight autosave cannot overwrite a save or discard,
    // and durability is acknowledged back on this thread.
    documents.startBackgroundPersistence();

    // Task 4.1: the workspace shell's resizable side panels. The snap
    // maths is C++ (fixture-tested); QML maps drags onto it.
    openshow::ui::WorkspacePanels panels;

    // Task 4.5: the settings page's persisted toggles (number keys,
    // fullscreen); the page and the shortcut gates both bind to it.
    openshow::ui::AppSettings settings;

    QObject::connect(&app, &QGuiApplication::aboutToQuit,
                     [&worker] { worker.shutdownNow(); });

    // Task 6.12 (review R7): when a save adopts the package's own copies of
    // asset bytes, the scene payloads the worker controller still holds have
    // to stop naming the old files — those are what the operator is now free
    // to delete, and the autosave tree among them is deleted by the save
    // itself.
    QObject::connect(&documents, &openshow::ui::DocumentController::assetsAdopted, &worker,
                     &openshow::ui::WorkerController::adoptAssetPaths);

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
    engine.rootContext()->setContextProperty("panels", &panels);
    engine.rootContext()->setContextProperty("appSettings", &settings);
    // Fixed-size capture seam for the workspace layout checks (PLAN.md UI
    // acceptance: "agreed captures at fixed sizes"; the reference's
    // measurements were taken at 1680x1020).
    const QStringList wanted =
        qEnvironmentVariable("OPENSHOW_UI_SIZE", "1380x800").split(QLatin1Char('x'));
    const QSize uiSize(wanted.value(0).toInt(), wanted.value(1).toInt());
    engine.rootContext()->setContextProperty(
        "uiSize", uiSize.width() >= 400 && uiSize.height() >= 400 ? uiSize
                                                                  : QSize(1380, 800));

    engine.loadFromModule("OpenShow", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
