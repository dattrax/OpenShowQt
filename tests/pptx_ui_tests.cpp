// PX.3/PX.4: the two deck drop doors and the More-menu entries.
//
// The shell is the real one. The external drop is synthesized as a real
// QDropEvent (URL mime data) over each DropArea, so this exercises the QML
// handler rather than only the controller verb behind it.
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "AppSettings.h"
#include "BibleImport.h"
#include "CatalogController.h"
#include "DocumentController.h"
#include "ScriptureImport.h"
#include "ServiceCatalogBridge.h"
#include "SongImport.h"
#include "WorkerController.h"
#include "WorkspacePanels.h"

#include <cstdio>
#include <filesystem>

namespace {

bool waitForImport(openshow::ui::DocumentController& docs, int timeoutMs = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (docs.powerPointImportBusy() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !docs.powerPointImportBusy();
}

bool dropUrlOn(QQuickWindow* window, QQuickItem* target, const QUrl& url) {
    if (window == nullptr || target == nullptr || !target->isVisible()) return false;
    const QPoint point =
        target->mapToScene(QPointF(target->width() / 2.0, target->height() / 2.0)).toPoint();
    QMimeData mime;
    mime.setUrls({url});
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &enter);
    QDragMoveEvent move(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &move);
    QDropEvent drop(QPointF(point), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &drop);
    return drop.isAccepted();
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");
    QTemporaryDir scratch;
    if (!scratch.isValid()) return 1;
    const auto base = std::filesystem::path(scratch.path().toStdString());

    openshow::ui::DocumentController docs(nullptr, base / "service");
    openshow::ui::WorkspacePanels panels(nullptr, base / "workspace.ini");
    openshow::ui::AppSettings settings(nullptr, base / "app.ini");
    openshow::ui::WorkerController worker;
    BibleImport bible;
    openshow::ui::ScriptureImport scripture(&bible, &docs, &worker);
    openshow::ui::CatalogController library(nullptr, base);
    openshow::ui::SongImport songImport(&library, &docs, &worker);
    openshow::ui::ServiceCatalogBridge songService(&library, &docs, &worker);

    docs.createSong(QStringLiteral("Existing"));
    docs.addSlide();
    docs.setSlideText(QStringLiteral("Existing service content"));
    docs.flushText();
    docs.addShowToPlaylist(docs.currentSongId());

    QQmlApplicationEngine engine;
    auto* context = engine.rootContext();
    context->setContextProperty("applicationVersion", "test");
    context->setContextProperty("workerOffer", "test");
    context->setContextProperty("worker", &worker);
    context->setContextProperty("docs", &docs);
    context->setContextProperty("panels", &panels);
    context->setContextProperty("appSettings", &settings);
    context->setContextProperty("bible", &bible);
    context->setContextProperty("scripture", &scripture);
    context->setContextProperty("library", &library);
    context->setContextProperty("songImport", &songImport);
    context->setContextProperty("songService", &songService);
    context->setContextProperty("uiSize", QSize(1380, 800));
    engine.loadFromModule("OpenShow", "Main");
    if (engine.rootObjects().isEmpty()) return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window == nullptr) return 1;
    QTest::qWait(150);

    bool failed = false;
    auto check = [&failed](bool good, const char* message) {
        if (!good) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            failed = true;
        }
    };

    auto* serviceDrop = window->findChild<QQuickItem*>(QStringLiteral("serviceDropArea"));
    auto* libraryDrop = window->findChild<QQuickItem*>(QStringLiteral("presentationsDrop"));
    QObject* libraryItem =
        window->findChild<QObject*>(QStringLiteral("powerPointLibraryMenuItem"));
    QObject* serviceItem =
        window->findChild<QObject*>(QStringLiteral("powerPointServiceMenuItem"));
    check(serviceDrop != nullptr, "the service has a deck drop area");
    check(libraryDrop != nullptr, "the Library header has a deck drop area");
    check(libraryItem == nullptr && serviceItem == nullptr,
          "redundant PowerPoint import menu entries are absent");
    check(window->findChild<QObject*>(QStringLiteral("powerPointImportDialog")) == nullptr,
          "the menu-only PowerPoint file picker is removed");
    check(window->findChild<QObject*>(QStringLiteral("removeUnusedMediaMenuItem"))
              != nullptr,
          "the explicit unused-media sweep is reachable from More");
    check(serviceDrop != nullptr
              && serviceDrop->property("keys").toStringList().contains(
                     QStringLiteral("text/uri-list")),
          "the service drops accept URL payloads");
    check(libraryDrop != nullptr
              && libraryDrop->property("keys").toStringList().contains(
                     QStringLiteral("text/uri-list")),
          "the Library header accepts URL payloads");

#ifdef OPENSHOW_PPTX_RENDER_BIN
    const QUrl deck =
        QUrl::fromLocalFile(QStringLiteral(OPENSHOW_SOURCE_DIR
                                          "/tools/pptx-render/tests/fixtures/aug-8.pptx"));
    const int songsBefore = docs.songs().size();
    const int entriesBefore = docs.playlistEntries().size();

    // Library door: a real URL drop makes a reusable presentation and leaves
    // the running order alone.
    check(dropUrlOn(window, libraryDrop, deck),
          "a .pptx URL drop is accepted by the Library header");
    check(waitForImport(docs), "the Library drop finishes rendering");
    check(docs.songs().size() == songsBefore + 1,
          "the Library drop added exactly one presentation");
    check(docs.playlistEntries().size() == entriesBefore,
          "the Library drop did not touch the running order");
    check(docs.status().contains(QStringLiteral("imported 6 slides")),
          "the Library drop imported the operator deck's six slides");
    docs.undo();
    check(docs.songs().size() == songsBefore && docs.playlistEntries().size() == entriesBefore,
          "one undo removes the dropped Library presentation");

    // Service door: the same drop inserts a schedule-owned show at the
    // running order and does not create a Library row.
    check(dropUrlOn(window, serviceDrop, deck),
          "a .pptx URL drop is accepted by the service");
    check(waitForImport(docs), "the service drop finishes rendering");
    check(docs.playlistEntries().size() == entriesBefore + 1,
          "the service drop added one running-order row");
    check(docs.songs().size() == songsBefore,
          "the service drop is not a Library row");
    docs.undo();
    check(docs.playlistEntries().size() == entriesBefore,
          "one undo removes the dropped service row");
#else
    std::fprintf(stderr, "SKIP: no renderer path in this build\n");
#endif

    if (failed) return 1;
    std::printf("pptx_ui_tests: all fixtures passed\n");
    return 0;
}
