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
#include "BibleImport.h"
#include "ScriptureImport.h"
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

    // Workspace controls inherit the neutral dark palette and blue accent
    // from Main.qml; shared compact buttons provide the flat toolbar style.
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

    // Milestone BI: the Bible importer's two sessions. The lookup (BI.2)
    // owns the transient passage and dies with the page; the page session
    // (BI.5) packs the selection through the worker's own text layout
    // (BI.4), previews it off air, and offers the schedule as its only
    // destination (BI.3). Both bind to the objects the whole shell
    // already answers to; neither owns them.
    BibleImport bibleImport;
    openshow::ui::ScriptureImport scriptureImport(&bibleImport, &documents, &worker);

    QObject::connect(&app, &QGuiApplication::aboutToQuit,
                     [&worker] { worker.shutdownNow(); });

    // Task 6.12 (review R7): when a save adopts the package's own copies of
    // asset bytes, the scene payloads the worker controller still holds have
    // to stop naming the old files — those are what the operator is now free
    // to delete, and the autosave tree among them is deleted by the save
    // itself.
    QObject::connect(&documents, &openshow::ui::DocumentController::assetsAdopted, &worker,
                     &openshow::ui::WorkerController::adoptAssetPaths);

    // Task TR.1: the cue lane asks the DOCUMENTS how a slide's cue moves, at
    // the instant Go is issued, and carries the answer as a value through the
    // prepare intent to scene.present. The worker controller never learns what
    // a Theme is — it asks, this callback answers, and the record is that
    // cue's for as long as the prepare is in flight.
    worker.setTransitionResolver(
        [&documents](const boost::json::object& scene) {
            return documents.transitionForSceneJson(scene);
        });

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
    // Milestone BI: the importer's page. The lookup session owns the
    // transient passage (BI.2); the page session turns a selection into
    // worker-measured slides (BI.4), shows them off air through the same
    // preview transport the edit canvas uses, and Add is the one door to
    // today's schedule (BI.3) — there is no other destination and there
    // never will be one from these objects.
    engine.rootContext()->setContextProperty("bible", &bibleImport);
    engine.rootContext()->setContextProperty("scripture", &scriptureImport);
    // Fixed-size capture seam for the workspace layout checks (PLAN.md UI
    // acceptance: "agreed captures at fixed sizes"; the reference's
    // measurements were taken at 1680x1020).
    const QStringList wanted =
        qEnvironmentVariable("OPENSHOW_UI_SIZE", "1380x800").split(QLatin1Char('x'));
    const QSize uiSize(wanted.value(0).toInt(), wanted.value(1).toInt());
    engine.rootContext()->setContextProperty(
        "uiSize", uiSize.width() >= 400 && uiSize.height() >= 400 ? uiSize
                                                                  : QSize(1380, 800));

    // The inspector read model (task CS.4) travels with the module itself
    // (QML_ELEMENT), so the app and the interaction fixture resolve it the
    // same way they resolve PreviewView.
    engine.loadFromModule("OpenShow", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
