// Interaction regressions for the operator workspace. No worker or user files.
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>

#include "AppSettings.h"
#include "DocumentController.h"
#include "WorkerController.h"
#include "WorkspacePanels.h"

#include <cstdio>
#include <functional>

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
    docs.createSong("Good Grace");
    const QString songId = docs.currentSongId();
    docs.addSlide();
    docs.setSlideText("People come together\nStrange as neighbours, our blood is one");
    docs.flushText();
    for (int i = 1; i < 18; ++i) {
        docs.addSlide();
        docs.setSlideText(QStringLiteral("Verse %1\nA presentation prepared for Sunday").arg(i));
        docs.flushText();
    }
    docs.addShowToPlaylist(songId);
    docs.createSong("Welcome");
    docs.setSlideText("Welcome\nWe are glad you are here");
    docs.flushText();
    docs.addShowToPlaylist(docs.currentSongId());
    docs.selectSong(songId);
    docs.selectSlide(0);

    QQmlApplicationEngine engine;
    QStringList warnings;
    QObject::connect(&engine, &QQmlEngine::warnings, [&warnings](const QList<QQmlError>& errors) {
        for (const auto& error : errors) warnings.append(error.toString());
    });
    auto* context = engine.rootContext();
    context->setContextProperty("applicationVersion", "test");
    context->setContextProperty("workerOffer", "test");
    context->setContextProperty("worker", &worker);
    context->setContextProperty("docs", &docs);
    context->setContextProperty("panels", &panels);
    context->setContextProperty("appSettings", &settings);
    context->setContextProperty("uiSize", QSize(1380, 800));
    engine.loadFromModule("OpenShow", "Main");
    if (engine.rootObjects().isEmpty()) return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!window) return 1;
    QTest::qWait(100);
    bool failed = false;
    auto check = [&failed](bool good, const char* message) {
        if (!good) { std::fprintf(stderr, "FAIL: %s\n", message); failed = true; }
    };
    auto* left = window->findChild<QQuickItem*>("libraryDock");
    auto* right = window->findChild<QQuickItem*>("outputDock");
    auto* drawer = window->findChild<QQuickItem*>("libraryDrawer");
    auto* grid = window->findChild<QQuickItem*>("operatorSlideGrid");
    auto* editor = window->findChild<QQuickItem*>("slideTextEditor");
    auto* preview = window->findChild<QQuickItem*>("audiencePreview");
    if (!left || !right || !drawer || !grid || !editor || !preview) return 1;
    auto* destination = window->findChild<QQuickItem*>("audienceDestinationPicker");
    check(destination && !destination->isVisible(), "Show hides audience routing configuration");
    // Walk the actual visible tree: destination/layer setup and arbitrary
    // writable fields cannot leak back into the operator screen.
    std::function<void(QQuickItem*)> auditOperator = [&](QQuickItem* item) {
        if (item->isVisible()) {
            check(item->objectName() != "audienceContentSetting", "Show hides audience content configuration");
            const bool input = item->property("cursorPosition").isValid()
                && item->property("readOnly").isValid() && !item->property("readOnly").toBool();
            check(!input || item->objectName() == "librarySearchField",
                  "Show permits search input but no raw paths or slide authoring fields");
        }
        for (auto* child : item->childItems()) auditOperator(child);
    };
    auditOperator(window->contentItem());
    window->setProperty("page", "settings");
    QTest::qWait(20);
    check(destination && destination->isVisible(), "Settings exposes audience routing");
    window->setProperty("page", "show");
    QTest::qWait(20);
    check(!editor->isVisible(), "Show has no visible text editor");
    const qreal leftWidth = panels.leftWidth();
    const qreal rightWidth = panels.rightWidth();
    // Real hover events, including moving along each handle with no button down.
    for (auto* dock : {left, right}) {
        const qreal x = dock == left ? dock->width() - 2 : 2;
        const auto point = dock->mapToScene(QPointF(x, 60)).toPoint();
        QTest::mouseMove(window, point);
        QTest::mouseMove(window, point + QPoint(0, 90));
    }
    check(panels.leftWidth() == leftWidth && panels.rightWidth() == rightWidth,
          "hovering resize handles leaves dock widths unchanged");
    const auto grip = left->mapToScene(QPointF(left->width() - 2, 60)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, grip);
    QTest::mouseMove(window, grip - QPoint(40, 0), 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, grip - QPoint(40, 0));
    check(panels.leftWidth() < leftWidth, "dragging a pressed handle still resizes the dock");
    panels.resizeLeft(leftWidth);
    const auto slidesBefore = docs.slides();
    const auto playlistBefore = docs.playlistEntries();
    for (const auto key : {Qt::Key_D, Qt::Key_X, Qt::Key_V, Qt::Key_Z}) {
        QTest::keyClick(window, key, Qt::ControlModifier);
        check(docs.slides() == slidesBefore && docs.playlistEntries() == playlistBefore,
              "Show shortcuts cannot duplicate, cut, paste or undo document content");
    }
    for (const auto size : {QSize(1000, 680), QSize(1380, 800), QSize(1680, 1020)}) {
        window->resize(size);
        for (const auto* page : {"edit", "settings", "show"}) {
            window->setProperty("page", page);
            QTest::qWait(20);
            check(left->height() > 0 && right->height() > 0 && preview->isVisible(),
                  "page navigation retains both docks and the audience preview");
            check(right->mapToScene(QPointF(0, right->height())).y() == window->height(),
                  "audience dock extends to the bottom beside the library");
        }
        check(grid->width() >= 190 && grid->height() > 100,
              "slide grid remains usable at supported sizes");
        check(drawer->mapToScene(QPointF(0, 0)).y() > 300,
              "opening the library preserves the slide workspace");
    }
    panels.resizeDrawer(300, window->height() - 364);
    QTest::qWait(30);
    // Dragging a library entry into blank space must not displace its row.
    QList<QQuickItem*> rows;
    std::function<void(QQuickItem*)> findRows = [&](QQuickItem* item) {
        if (item->objectName() == "libraryShowRow") rows.append(item);
        for (auto* child : item->childItems()) findRows(child);
    };
    findRows(window->contentItem());
    check(!rows.isEmpty(), "library rows are instantiated");
    if (!rows.isEmpty()) {
        QPointer<QQuickItem> row = rows.first();
        const auto position = row->position();
        const auto start = row->mapToScene(QPointF(25, 14)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(100, 20), 20);
        QTest::mouseMove(window, start + QPoint(160, -30), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start + QPoint(160, -30));
        check(row && row->position() == position, "library row remains in place after a drag");
    }
    QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
    QTest::qWait(20);
    auditOperator(window->contentItem());
    QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
    check(window->property("page").toString() == "show"
          && drawer->property("tab").toString() == "media",
          "focused library search owns its tab shortcuts");
    QTest::keyClick(window, Qt::Key_Escape);
    check(!drawer->property("containsFocus").toBool(), "dismissing search releases presenter keys");
    drawer->setProperty("tab", "shows");
    // Verify moved editor accepts words and synchronizes after navigation.
    window->setProperty("page", "edit");
    QTest::qWait(20);
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    for (const char c : std::string("Updated lyrics")) QTest::keyClick(window, c);
    docs.flushText();
    check(docs.currentSlideText() == "Updated lyrics", "text editing still works in Edit");
    const int selectedSlide = docs.currentSlide();
    window->contentItem()->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_PageDown);
    check(docs.currentSlide() == selectedSlide, "Edit does not run presenter navigation");
    docs.selectSlide(1);
    QTest::qWait(20);
    check(editor->property("text").toString() == docs.currentSlideText(),
          "editor follows slide selection without copying the old text over it");
    docs.selectSlide(0);
    window->setProperty("page", "show");
    QTest::qWait(80);
    panels.resizeDrawer(300, window->height() - 364);
    QTest::qWait(50);
    if (qEnvironmentVariableIsSet("OPENSHOW_WORKSPACE_LIVE")) {
        worker.start();
        QElapsedTimer timeout;
        timeout.start();
        const auto firstId = docs.slides().first().toMap().value("id").toString();
        const auto lastId = docs.slides().last().toMap().value("id").toString();
        while (timeout.elapsed() < 10000 &&
               (worker.thumbnail(firstId).isNull() || worker.thumbnail(lastId).isNull()))
            QTest::qWait(50);
        check(worker.outputLive() && !worker.thumbnail(firstId).isNull()
              && !worker.thumbnail(lastId).isNull(),
              "visible slide grid receives thumbnails from the production worker");
    }
    const auto capture = qEnvironmentVariable("OPENSHOW_WORKSPACE_CAPTURE");
    if (!capture.isEmpty()) {
        window->grabWindow().save(capture);
        window->resize(QSize(1380, 800));
        QTest::qWait(50);
        window->grabWindow().save(capture + ".1380.png");
        window->resize(QSize(1000, 680));
        QTest::qWait(50);
        window->grabWindow().save(capture + ".1000.png");
        window->setProperty("page", "edit");
        QTest::qWait(50);
        window->grabWindow().save(capture + ".edit.png");
        window->setProperty("page", "settings");
        QTest::qWait(50);
        window->grabWindow().save(capture + ".settings.png");
    }
    worker.shutdownNow();
    for (const auto& warning : warnings) {
        if (warning.contains("Shortcut: Only binding to one of multiple key bindings")) continue;
        std::fprintf(stderr, "QML: %s\n", qPrintable(warning));
        failed = true;
    }
    return failed ? 1 : 0;
}
