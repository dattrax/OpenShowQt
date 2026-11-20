/*
 * Task SL.9 (integrated): the whole Library journey on the production shell
 * with a REAL worker — clipboard import, a filtered Library row dragged into
 * a SCROLLED running order, keyboard selection/removal, one undo, and
 * service/catalog persistence across a restart. Nothing is presented.
 *
 * Needs a display for the worker's output window: desktop preset, label
 * display.
 */

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QQuickPaintedItem>
#include <QJSValue>
#include <QLineF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QTemporaryDir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "AppSettings.h"
#include "BibleImport.h"
#include "CatalogController.h"
#include "DocumentController.h"
#include "ScriptureImport.h"
#include "ReflowPlanner.h"
#include "ServiceCatalogBridge.h"
#include "ThemeApplySession.h"
#include "SongImport.h"
#include "WorkerController.h"
#include "WorkspacePanels.h"

namespace {

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        ++gFailed;
    }
    return condition;
}

template <typename Predicate>
bool waitFor(Predicate predicate, int ms = 30000) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        if (predicate()) {
            return true;
        }
        QTest::qWait(20);
    }
    return predicate();
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

QQuickItem* findNamed(QQuickItem* root, const char* name) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->objectName() == QLatin1String(name)) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name)) {
            return found;
        }
    }
    return nullptr;
}

void collectNamed(QQuickItem* root, const char* name, QList<QQuickItem*>* out) {
    if (root == nullptr) {
        return;
    }
    if (root->objectName() == QLatin1String(name)) {
        out->append(root);
    }
    for (QQuickItem* child : root->childItems()) {
        collectNamed(child, name, out);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        return 1;
    }
    const std::filesystem::path root = std::filesystem::path(scratch.path().toStdString());

    openshow::ui::WorkerController worker;
    worker.start();
    const auto measureReady = [&] {
        QVariantMap probe;
        probe.insert(QStringLiteral("key"), QStringLiteral("probe"));
        probe.insert(QStringLiteral("lines"), QVariantList{QStringLiteral("x")});
        probe.insert(QStringLiteral("fontSize"), 40.0);
        probe.insert(QStringLiteral("width"), 200.0);
        probe.insert(QStringLiteral("lineSpacing"), 1.25);
        return worker.measureText(QVariantList{probe}) != 0;
    };
    if (!check(waitFor(measureReady), "the real worker came up and measures")) {
        worker.shutdownNow();
        return 1;
    }

    openshow::ui::CatalogController library(nullptr, root);
    openshow::ui::DocumentController docs(nullptr, root / "service");
    openshow::ui::WorkspacePanels panels(nullptr, root / "workspace.ini");
    openshow::ui::AppSettings settings(nullptr, root / "app.ini");
    BibleImport bible;
    openshow::ui::ScriptureImport scripture(&bible, &docs, &worker);
    openshow::ui::SongImport songImport(&library, &docs, &worker);
    openshow::ui::ServiceCatalogBridge songService(&library, &docs, &worker);
    openshow::ui::ReflowPlanner reflow(&docs, &worker);
    openshow::ui::ThemeApplySession applySession(&docs, &library);

    const std::string text =
        readFile(OPENSHOW_SOURCE_DIR "/docs/fixtures/songs/morning-light.txt");
    QGuiApplication::clipboard()->setText(QString::fromUtf8(text.c_str()));
    QQmlApplicationEngine engine;
    QStringList warnings;
    QObject::connect(&engine, &QQmlEngine::warnings,
                     [&warnings](const QList<QQmlError>& errors) {
                         for (const QQmlError& error : errors) {
                             // The same narrow StandardKey-alias exclusion as
                             // the workspace and capture suites.
                             if (error.toString().contains(
                                     "Shortcut: Only binding to one of multiple key bindings")) {
                                 continue;
                             }
                             warnings.append(error.toString());
                         }
                     });
    QQmlContext* context = engine.rootContext();
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
    context->setContextProperty("reflow", &reflow);
    context->setContextProperty("applySession", &applySession);
    context->setContextProperty("uiSize", QSize(1380, 800));
    engine.loadFromModule("OpenShow", "Main");
    auto* window = engine.rootObjects().isEmpty()
                       ? nullptr
                       : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!check(window != nullptr, "the production shell loads")) {
        for (const auto& warning : warnings)
            std::fprintf(stderr, "  QML: %s\n", qPrintable(warning));
        worker.shutdownNow();
        return 1;
    }
    QTest::qWait(150);

    // Import is a preparation action, reachable directly from Show.
    auto click = [window](QQuickItem* item) {
        if (item) QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    };
    check(!findNamed(window->contentItem(), "libraryImportSongButton")
              && !findNamed(window->contentItem(), "libraryAddSongButton"),
          "Library has no redundant Import song or Add to service buttons");
    click(findNamed(window->contentItem(), "toolbarMore"));
    QTest::qWait(40);
    auto* importMenu = findNamed(window->contentItem(), "songImportMenuItem");
    check(importMenu && importMenu->isVisible(), "the toolbar exposes clipboard song import on Show");
    click(importMenu);
    QTest::qWait(50);
    check(window->property("page") == "songimport", "the toolbar menu opens clipboard import");
    click(findNamed(window->contentItem(), "songPasteButton"));
    check(waitFor([&] { return !songImport.measuring() && songImport.canAdd(); }),
          "pasting through the visible page measures the clipboard fixture");
    // The measured model arrives before the next QML layout pass. Wait for
    // that pass before reading the ScrollView's new content extent.
    QTest::qWait(80);
    auto* importPage = findNamed(window->contentItem(), "songImportPage");
    auto* importScroll = importPage ? importPage->property("contentItem").value<QQuickItem*>() : nullptr;
    if (importScroll) importScroll->setProperty("contentY", qMax(0.0,
        importScroll->property("contentHeight").toDouble() - importScroll->height()));
    QTest::qWait(40);
    const auto debugCapture = qEnvironmentVariable("OPENSHOW_LIBRARY_CAPTURE");
    if (!debugCapture.isEmpty())
        window->grabWindow().save(debugCapture + "-import.png");
    auto* addToLibrary = findNamed(window->contentItem(), "songAddToLibraryButton");
    check(importScroll && addToLibrary && QRectF(0, 0, importScroll->width(), importScroll->height()).contains(
              addToLibrary->mapToItem(importScroll, QPointF(addToLibrary->width() / 2, addToLibrary->height() / 2))),
          "the import action is actually inside the scrolled viewport");
    click(addToLibrary);
    if (!check(library.songCount() == 1 && window->property("page") == "show",
               "Add to Library creates one song and returns to Show")) {
        worker.shutdownNow();
        return 1;
    }
    window->setProperty("page", "edit");
    QTest::qWait(40);
    click(findNamed(window->contentItem(), "toolbarMore"));
    QTest::qWait(40);
    importMenu = findNamed(window->contentItem(), "songImportMenuItem");
    check(importMenu && importMenu->isVisible(), "More keeps clipboard import accessible after importing");
    click(importMenu);
    QTest::qWait(40);
    check(window->property("page") == "songimport", "the menu opens the same import page");
    // The page keeps the scroll position it had when the operator left it
    // (the Add flow scrolled to the bottom to reach the button). Bring the
    // header back into the viewport before clicking, as reopening the page in
    // the application does by hand; a click outside the viewport lands on
    // nothing.
    if (importScroll) {
        importScroll->setProperty("contentY", 0.0);
        QTest::qWait(40);
    }
    click(findNamed(window->contentItem(), "songImportBackButton"));
    QTest::qWait(40);
    check(window->property("page") == "edit", "toolbar song import returns to its originating page");

    // A scrolled running order: a filler presentation listed many times.
    check(docs.createSong("Filler"), "a filler presentation exists");
    const QString fillerId = docs.currentSongId();
    docs.addSlide();
    docs.setSlideText("Synthetic presentation words");
    docs.flushText();

    // --- EW.4/EW.5: in-place editing against the real worker ---------------
    {
        window->setProperty("page", "edit");
        QTest::qWait(120);
        docs.selectSlide(0);
        QTest::qWait(80);
        auto* canvas = findNamed(window->contentItem(), "editCanvas");
        auto* sourceDrawer = findNamed(window->contentItem(), "slideTextEditor");
        check(worker.textEditorSupported(),
              "the real worker offers text.editor for in-place editing");
        check(canvas && sourceDrawer && !sourceDrawer->isVisible(),
              "the bottom field is an explicit drawer and stays closed on a capable worker");
        check(canvas && !canvas->property("textMode").toBool(),
              "Edit opens in box mode, not text mode");
        const auto box = docs.currentSlideBoxes().at(0).toMap();
        const double zoom = canvas->property("zoom").toDouble();
        const double stageX =
            (canvas->width() - canvas->property("stageWidth").toDouble()) / 2.0;
        const double stageY =
            (canvas->height() - canvas->property("stageHeight").toDouble()) / 2.0;
        auto boxPoint = [&](double fx, double fy) {
            return canvas->mapToScene(QPointF(
                stageX + (box.value("x").toDouble()
                          + box.value("width").toDouble() * fx) * zoom,
                stageY + (box.value("y").toDouble()
                          + box.value("height").toDouble() * fy) * zoom)).toPoint();
        };
        const QString originalWords = docs.currentSlideText();
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, boxPoint(0.5, 0.5));
        QTest::qWait(120);
        auto* inlineEditor = window->findChild<QObject*>("inlineTextEditor");
        auto* inlineInput = window->findChild<QQuickItem*>("inlineTextInput");
        check(canvas->property("textMode").toBool() && inlineEditor
                  && inlineEditor->property("active").toBool(),
              "double-clicking a text box enters in-place editing");
        check(inlineInput && window->activeFocusItem() == inlineInput,
              "the native text input owns the keyboard while editing");
        check(waitFor([&] {
                  return inlineEditor->property("geometryFresh").toBool();
              }),
              "the worker's caret geometry arrives for the draft");
        check(inlineEditor->property("caretRect").toRectF().height() > 1.0,
              "the caret has the worker's real line height");
        // Typing edits the draft; the document keeps the emitted words after
        // the controller's own debounce, and the caret chrome follows.
        QTest::keyClick(window, Qt::Key_X);
        check(waitFor([&] {
                  return docs.currentSlideText().contains(QStringLiteral("x")) &&
                      docs.currentSlideText().length() == originalWords.length() + 1;
              }, 1500),
              "typing reaches the document's debounced text path");
        check(inlineEditor->property("caretRect").toRectF().width() >= 1.0 &&
                  !std::isnan(inlineEditor->property("caretRect").toRectF().x()),
              "the caret follows the draft with a real position");
        const QString afterTyping = docs.currentSlideText();
        // Pointer selection uses the worker's hit test, not Qt's metrics.
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, boxPoint(0.45, 0.5));
        QTest::mouseMove(window, boxPoint(0.60, 0.5), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, boxPoint(0.60, 0.5));
        check(waitFor([&] {
                  return inlineEditor->property("selectionRects").toList().size() > 0;
              }),
              "dragging selects through the worker's hit test and paints its rects");
        check(inlineInput->property("selectionStart").toInt()
                  != inlineInput->property("selectionEnd").toInt(),
              "the native input carries the selected range");
        // Enter is an authored line break, not a commit or a new slide.
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(60);
        check(inlineEditor->property("draft").toString().contains(QStringLiteral("\n")),
              "Enter inserts a line break into the draft");
        check(canvas->property("textMode").toBool(),
              "Enter while editing never leaves text mode");
        // Escape leaves the mode with the committed words kept. The line
        // break and the typed letter are separate bursts, so each is one
        // undo and nothing else moves.
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(80);
        check(!canvas->property("textMode").toBool(),
              "Escape leaves in-place editing");
        check(!sourceDrawer->isVisible(),
              "leaving text mode does not resurrect the source drawer");
        check(docs.currentSlideText().contains(QStringLiteral("\n")),
              "the committed words survive leaving the mode");
        docs.undo();
        check(docs.currentSlideText() == afterTyping,
              "one undo takes the line break away and leaves the typed letter");
        docs.undo();
        check(docs.currentSlideText() == originalWords,
              "the next undo takes the typed burst away");
        // Enter on a selected text box is the keyboard way in; F2 keeps its
        // output meaning and never enters text mode.
        auto* gesture = findNamed(canvas, "editGestureArea");
        if (gesture) gesture->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_F2);
        QTest::qWait(40);
        check(!canvas->property("textMode").toBool(),
              "F2 keeps its output meaning and does not enter text mode");
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(80);
        check(canvas->property("textMode").toBool(),
              "Enter on a selected text box enters in-place editing");
        // The caret resumes where it was left (the remembered caret), not at
        // a default the document could not know.
        const int resumed = inlineInput->property("cursorPosition").toInt();
        check(resumed > 0, "keyboard entry resumes the remembered caret");
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(40);
        check(!canvas->property("textMode").toBool(), "Escape returns to box mode");
        // Clicking empty canvas finishes editing even though it is not a
        // focusable field. There is no separate Done-button workflow.
        if (gesture) gesture->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(80);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          canvas->mapToScene(QPointF(2, 2)).toPoint());
        check(waitFor([&] { return !canvas->property("textMode").toBool(); }),
              "clicking outside the text box leaves editing and keeps its words");
        check(docs.currentSlideText() == originalWords,
              "clicking off the box never discards committed words");

        // Up/Down follow rendered lines and retain the desired horizontal
        // position across a short line; Shift extends the same selection.
        docs.customizeCurrentSlide(QStringLiteral("fontFamily"), QStringLiteral("DejaVu Sans Mono"));
        docs.customizeCurrentSlide(QStringLiteral("fontSize"), 60.0);
        docs.customizeCurrentSlide(QStringLiteral("align"), QStringLiteral("left"));
        const QString multiline = QStringLiteral("0123456789abcdef\nxy\n0123456789abcdef");
        docs.setSlideText(multiline);
        docs.flushText();
        if (gesture) gesture->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        check(waitFor([&] { return inlineEditor->property("geometryFresh").toBool(); }),
              "the multiline editing geometry arrives");
        inlineInput->setProperty("cursorPosition", 8);
        check(waitFor([&] { return inlineEditor->property("pendingGeometry").toInt() == 0; }),
              "the initial column is measured");
        QTest::keyClick(window, Qt::Key_Down);
        check(waitFor([&] { return inlineInput->property("cursorPosition").toInt() == 19; }),
              "Down moves to the short line's end");
        QTest::keyClick(window, Qt::Key_Down);
        check(waitFor([&] { return inlineInput->property("cursorPosition").toInt() == 28; }),
              "Down restores the remembered column on the following longer line");
        QTest::keyClick(window, Qt::Key_Up);
        QTest::keyClick(window, Qt::Key_Up);
        check(waitFor([&] { return inlineInput->property("cursorPosition").toInt() == 8; }),
              "rapid Up presses return to the original line and column");
        QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
        QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
        check(waitFor([&] {
                  return inlineInput->property("selectionStart").toInt() == 8 &&
                         inlineInput->property("selectionEnd").toInt() == 28;
              }), "Shift+Down extends the selection across rendered lines");
        check(docs.currentSlideText() == multiline, "vertical navigation changes no words");
        docs.setTextBoxGeometry(300, 100, 400, 800);
        inlineInput->setProperty("cursorPosition", 3);
        check(waitFor([&] {
                  return inlineEditor->property("pendingGeometry").toInt() == 0 &&
                         inlineEditor->property("caretLine").toInt() == 0;
              }), "a narrow box provides a wrapped-line fixture");
        QTest::keyClick(window, Qt::Key_Down);
        check(waitFor([&] {
                  const int offset = inlineInput->property("cursorPosition").toInt();
                  return inlineEditor->property("caretLine").toInt() == 1 && offset > 3 && offset < 17;
              }, 3000), "Down follows a soft wrap rather than the next authored newline");
        QTest::keyClick(window, Qt::Key_Up);
        check(waitFor([&] { return inlineInput->property("cursorPosition").toInt() == 3; }, 3000),
              "Up returns to the same column across a soft wrap");
        docs.revertCurrentSlideField(QStringLiteral("placement"));
        inlineInput->setProperty("text", QStringLiteral("0123456789abcdef\n\n0123456789abcdef"));
        inlineInput->setProperty("cursorPosition", 3);
        check(waitFor([&] { return inlineEditor->property("pendingGeometry").toInt() == 0; }),
              "empty-line navigation starts from matching geometry");
        QTest::keyClick(window, Qt::Key_Down);
        check(waitFor([&] { return inlineInput->property("cursorPosition").toInt() == 17; }, 3000),
              "Down can enter an empty authored line");
        QTest::keyClick(window, Qt::Key_Down);
        check(waitFor([&] { return inlineInput->property("cursorPosition").toInt() == 21; }, 3000),
              "Down leaves an empty line at the remembered column");
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          canvas->mapToScene(QPointF(stageX + 20 * zoom, stageY + 20 * zoom)).toPoint());
        check(waitFor([&] { return !canvas->property("textMode").toBool(); }),
              "clicking off a selected text range also finishes editing");
        docs.setSlideText(originalWords);
        docs.flushText();
        docs.revertCurrentSlideField(QStringLiteral("fontFamily"));
        docs.revertCurrentSlideField(QStringLiteral("fontSize"));
        docs.revertCurrentSlideField(QStringLiteral("align"));
        if (gesture) gesture->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(80);
        auto* inspectorField = findNamed(window->contentItem(), "themeSizeField");
        check(inspectorField != nullptr, "an inspector text field is available for focus handoff");
        if (inspectorField) {
            inspectorField->forceActiveFocus();
            check(waitFor([&] { return !canvas->property("textMode").toBool(); }),
                  "moving to an inspector ends inline editing");
            check(window->activeFocusItem() == inspectorField,
                  "finishing text editing does not steal focus back from the inspector");
        }
        // The source drawer is still an explicit action on a capable worker.
        auto* sourceButton = findNamed(window->contentItem(), "sourceTextButton");
        check(sourceButton != nullptr, "the source-text action is offered");
        if (sourceButton) {
            QMetaObject::invokeMethod(sourceButton, "clicked");
            QTest::qWait(60);
            check(sourceDrawer->isVisible(), "the source drawer opens only when asked");
            QMetaObject::invokeMethod(sourceButton, "clicked");
            QTest::qWait(40);
            check(!sourceDrawer->isVisible(), "and closes again");
        }
        // Deleting the box under a live session retires the session; the
        // controller's flush lands the burst before the box goes.
        if (gesture) gesture->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(100);
        if (canvas->property("textMode").toBool()) {
            QTest::keyClick(window, Qt::Key_Q);
            QTest::qWait(40);
            const int boxesBefore = docs.currentSlideBoxes().size();
            docs.removeCurrentTextBox();
            QTest::qWait(80);
            check(!canvas->property("textMode").toBool(),
                  "deleting the edited box leaves text mode");
            check(docs.currentSlideBoxes().size() <= boxesBefore,
                  "the deleted box is gone and no second box was invented");
        }

        // --- EW.10: the whole journey at the canvas ------------------------
        // Edit a verse, insert a slide, type into it, reorder it, undo/redo —
        // all without presenting: the audience revision must not move.
        const int sceneBefore = worker.sceneActive();
        docs.selectSlide(0);
        QTest::qWait(40);
        const QString fresh = docs.insertSlideAfterCurrent();
        QTest::qWait(80);
        check(!fresh.isEmpty() && docs.currentSlide() == 1,
              "the journey inserts a fresh slide after the edited verse");
        if (gesture) gesture->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(120);
        check(canvas->property("textMode").toBool(),
              "the fresh slide's empty box enters text mode from the keyboard");
        QTest::keyClick(window, Qt::Key_A);
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_E);
        QTest::keyClick(window, Qt::Key_D);
        QTest::qWait(80);
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(120);
        check(docs.currentSlideText() == QStringLiteral("added"),
              "the inserted slide carries the typed words");
        docs.moveCurrentSlideTo(0);
        QTest::qWait(40);
        check(docs.currentSlide() == 0 &&
                  docs.currentSlideText() == QStringLiteral("added"),
              "reordering keeps the typed slide selected by id");
        docs.undo();
        check(docs.currentSlide() == 1 &&
                  docs.currentSlideText() == QStringLiteral("added"),
              "one undo restores the order with the words still on their slide");
        docs.redo();
        check(docs.currentSlide() == 0 &&
                  docs.currentSlideText() == QStringLiteral("added"),
              "redo republishes the reorder without touching the words");
        check(worker.sceneActive() == sceneBefore && !worker.wordsOnAir(),
              "the whole editing journey never presented to the audience");
        docs.undo();  // the move
        docs.undo();  // the typed burst
        docs.undo();  // the insertion
        QTest::qWait(40);
        check(docs.slides().size() == 1,
              "the journey's structural edits undo one at a time");
        window->setProperty("page", "show");
        QTest::qWait(60);
    }

    for (int i = 0; i < 30; ++i) {
        docs.addShowToPlaylist(fillerId);
    }
    window->setProperty("page", "show");
    QTest::qWait(120);
    auto* playlistList = findNamed(window->contentItem(), "collectionPlaylistList");
    auto* songList = findNamed(window->contentItem(), "librarySongList");
    auto* search = findNamed(window->contentItem(), "librarySearchField");
    auto* songsFolder = findNamed(window->contentItem(), "librarySongsHeader");
    auto* presentationsFolder =
        findNamed(window->contentItem(), "libraryPresentationsHeader");
    auto* serviceFolder = findNamed(window->contentItem(), "libraryServiceHeader");
    if (!check(playlistList && songList && search, "the sidebar's lists and search are present")) {
        worker.shutdownNow();
        return 1;
    }
    if (!check(songsFolder && presentationsFolder && serviceFolder,
               "the strip lists Songs, Presentations and Service as folders")) {
        worker.shutdownNow();
        return 1;
    }
    auto selectFolder = [&](QQuickItem* folder) {
        if (!folder) return;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          folder->mapToScene(QPointF(40, 13)).toPoint());
        QTest::qWait(70);
    };
    check(songList->isVisible() && !playlistList->isVisible()
              && !findNamed(window->contentItem(), "collectionLibraryList")->isVisible(),
          "the open Songs folder fills the pane and no other folder's files show");
    {
        auto* folderBox = findNamed(window->contentItem(), "libraryFolderBox");
        auto* contentBox = findNamed(window->contentItem(), "libraryContentBox");
        check(folderBox && contentBox
                  && qAbs(folderBox->width() - search->width()) < 1
                  && qAbs(contentBox->width() - search->width()) < 1
                  && qAbs(folderBox->mapToScene(QPointF(0, 0)).x()
                          - search->mapToScene(QPointF(0, 0)).x()) < 1
                  && qAbs(contentBox->mapToScene(QPointF(0, 0)).x()
                          - search->mapToScene(QPointF(0, 0)).x()) < 1,
              "the folder and file boxes share the search field's width and edges");
    }
    // Visual capture: the Service folder open in the same pane.
    selectFolder(serviceFolder);
    check(playlistList->isVisible() && !songList->isVisible()
              && playlistList->property("count").toInt() ==
                     static_cast<int>(docs.playlistEntries().size()),
          "opening Service lists the running order's cues in the same pane");
    if (!debugCapture.isEmpty())
        window->grabWindow().save(debugCapture + "-service-folder.png");
    selectFolder(songsFolder);
    check(!findNamed(window->contentItem(), "toolbarSearchButton"),
          "the redundant toolbar Search control is absent");
    auto* hint = findNamed(search, "fixedFieldPlaceholder");
    check(hint && search->property("placeholderText").toString().isEmpty(),
          "the Library filter uses a fixed hint rather than an animated Material placeholder");
    const auto hintOrigin = hint ? hint->mapToScene(QPointF()) : QPointF();
    QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
    QTest::qWait(80);
    check(search->hasActiveFocus() && hint && hint->mapToScene(QPointF()) == hintOrigin,
          "Ctrl+F focuses search without moving its empty hint");
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(80);
    check(hint && hint->mapToScene(QPointF()) == hintOrigin,
          "leaving search keeps the hint in the same position");
    // Filter the Library to the imported song, then drag the filtered row
    // onto the Service FOLDER — the file joins the running order without the
    // operator having to open the service first.
    search->setProperty("text", "morning");
    QTest::qWait(60);
    QList<QQuickItem*> songRows;
    collectNamed(songList, "librarySongRow", &songRows);
    QQuickItem* songRow = nullptr;
    for (QQuickItem* row : songRows) {
        if (row->property("modelData").toMap().value("title").toString()
                == QStringLiteral("Morning Light")) {
            songRow = row;
        }
    }
    check(songRow != nullptr, "the filtered Library shows the imported song");
    if (songRow) for (auto* child : songRow->childItems())
        check(!child->property("text").toString().startsWith("CCLI "),
              "song rows show their title without a CCLI number");
    if (songRow) {
        const int before = static_cast<int>(docs.playlistEntries().size());
        const QPointF start = songRow->mapToScene(QPointF(25, 14));
        const QPointF target = serviceFolder->mapToScene(QPointF(40, 13));
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        QTest::mouseMove(window, (start + QPointF(60, 0)).toPoint(), 15);
        QTest::mouseMove(window, ((start + target) / 2 + QPointF(30, 0)).toPoint(), 15);
        QTest::mouseMove(window, (target + QPointF(20, 0)).toPoint(), 15);
        QTest::mouseMove(window, target.toPoint(), 15);
        QTest::qWait(60);
        check(serviceFolder->property("hovering").toBool(),
              "the Service folder marks itself as the drop target");
        QList<QQuickItem*> addBadges;
        collectNamed(window->contentItem(), "listDragFeedback", &addBadges);
        int shownAddBadges = 0;
        for (auto* badge : addBadges) if (badge->isVisible()) {
            ++shownAddBadges;
            check(QLineF(badge->mapToScene(QPointF()), target).length() < 2,
                  "the drag hotspot follows the pointer without losing its initial movement");
        }
        check(shownAddBadges == 1, "Library dragging shows one clear title badge");
        if (!debugCapture.isEmpty()) window->grabWindow().save(debugCapture + "-drag-add.png");
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target.toPoint());
        check(waitFor([&] { return !songService.busy(); }), "the drop prepared the song");
        check(static_cast<int>(docs.playlistEntries().size()) == before + 1,
              "the filtered song joined the running order through the folder");
        const QString showId = docs.showForCatalogItem(
            library.items().first().toMap().value("id").toString());
        check(!showId.isEmpty(), "the cue references a catalog-backed show");
        bool named = false;
        for (const QVariant& entry : docs.playlistEntries()) {
            named = named || entry.toMap().value("showId").toString() == showId;
        }
        check(named, "the running order holds a cue naming the referenced show");
        check(songList->isVisible() && !playlistList->isVisible(),
              "the drop leaves the Songs folder open, its filter untouched");
        check(search->property("text") == "morning",
              "dragging the filtered result keeps the search query unchanged");
        check(!docs.slidesOf(showId).isEmpty(), "the linked show carries slides");
    }
    check(!worker.wordsOnAir() && worker.sceneActive() == 0,
          "dragging and preparing never presented");

    // The Edit rail shows real worker thumbnails for this song alone,
    // follows selection and relinquishes its lane when the page is hidden.
    {
        const auto songId = library.items().first().toMap().value("id").toString();
        const auto showId = docs.showForCatalogItem(songId);
        docs.selectSong(showId);
        docs.selectSlide(0);
        const auto framesBeforeEdit = worker.thumbnailFrames();
        window->setProperty("page", "edit");
        auto* rail = findNamed(window->contentItem(), "editorSlideRail");
        auto* list = findNamed(window->contentItem(), "editorSlideList");
        check(rail && list && rail->isVisible()
                  && !findNamed(window->contentItem(), "libraryDock")->isVisible(),
              "Edit replaces the Library/service pane with its slide previews");
        const auto slides = docs.slides();
        check(!slides.isEmpty() && list->property("count").toInt() == slides.size(),
              "the preview rail shows only the selected song's slides");
        check(waitFor([&] { return worker.thumbnailFrames() > framesBeforeEdit
            && !worker.thumbnail(slides.first().toMap().value("id").toString()).isNull(); }),
              "the Edit rail receives real rendered slide thumbnails");
        const int last = slides.size() - 1;
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, last), Q_ARG(int, 1));
        QTest::qWait(100);
        QList<QQuickItem*> tiles;
        collectNamed(list, "editorSlideTile", &tiles);
        QQuickItem* tail = nullptr;
        for (auto* tile : tiles) if (tile->property("index").toInt() == last) tail = tile;
        if (tail) click(tail);
        check(tail && docs.currentSlide() == last, "clicking a scrolled thumbnail selects it for editing");
        check(waitFor([&] { return !worker.thumbnail(slides.last().toMap().value("id").toString()).isNull()
                                  && worker.selectionReady(); }),
              "both the selected canvas and the last thumbnail receive worker pictures");
        {
            const auto originalSize = window->size();
            const bool drawerWasOpen = panels.drawerHeight() > 40;
            if (drawerWasOpen) panels.toggleDrawer();
            for (const auto size : {QSize(1000, 680), QSize(1380, 800), QSize(1680, 1020)}) {
                window->resize(size);
                QTest::qWait(250);
                QList<QQuickItem*> resizedTiles;
                collectNamed(list, "editorSlideTile", &resizedTiles);
                bool selectedVisible = false;
                for (auto* tile : resizedTiles) if (tile->property("selected").toBool()) {
                    selectedVisible = QRectF(-1, -1, list->width() + 2, list->height() + 2).contains(
                        QRectF(tile->mapToItem(list, QPointF()), QSizeF(tile->width(), tile->height())));
                }
                check(selectedVisible, "resizing keeps the selected slide thumbnail fully visible");
                if (!debugCapture.isEmpty())
                    check(window->grabWindow().save(debugCapture + "-edit-" + QString::number(size.width()) + ".png"),
                          "capture the populated Edit preview rail");
            }
            window->resize(originalSize);
            if (drawerWasOpen) panels.toggleDrawer();
            QTest::qWait(80);
        }
        window->setProperty("page", "show");
        const int sent = rail->property("bandSent").toInt();
        QTest::qWait(300);
        check(rail->property("bandSent").toInt() == sent && !rail->isVisible(),
              "hidden Edit sends no thumbnail requests");
        check(!worker.wordsOnAir() && worker.sceneActive() == 0,
              "editing-slide navigation never presents to the audience");
    }

    // Section captions work across the service grid and Edit rail, including
    // a continuation whose preceding slide has scrolled out of view.
    {
        const auto oldShow = docs.currentSongId();
        const int oldSlide = docs.currentSlide();
        const auto oldEntries = docs.playlistEntries();
        const QStringList labels{"Blank", "Verse 1", "Verse 1", "Chorus", "CHORUS", "Bridge", "Chorus"};
        const QList<bool> starts{true, true, false, true, false, true, true};
        QVariantList plan;
        for (int i = 0; i < labels.size(); ++i) {
            QVariantMap row{{"group", labels[i]}, {"label", labels[i]}, {"blank", i == 0}};
            row.insert("lines", i == 0 ? QStringList{} : QStringList{QString("Synthetic lyric words %1").arg(i)});
            plan.append(row);
        }
        const auto fixture = docs.upsertCatalogSong("caption-fixture", "Section captions", "", "", "", plan, true);
        check(!fixture.isEmpty(), "prepare a song whose sections span consecutive slides");
        auto* show = findNamed(window->contentItem(), "showPage");
        auto* grid = findNamed(window->contentItem(), "operatorSlideGrid");
        window->setProperty("browsedShowId", "");
        QTest::qWait(60);
        const auto value = show->property("cueRows");
        const auto rows = value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toList() : value.toList();
        int matched = 0;
        for (const auto& value : rows) {
            const auto row = value.toMap();
            if (row.value("showId").toString() != fixture) continue;
            const int i = row.value("slideIndex").toInt();
            ++matched;
            check(row.value("sectionLabel").toString() == labels[i]
                      && row.value("sectionStart").toBool() == starts[i],
                  "section headings appear at changes, including a returning Chorus, but not on continuations");
        }
        check(matched == labels.size(), "caption styling leaves all service slides in place");
        const bool drawerWasOpen = panels.drawerHeight() > 40;
        if (drawerWasOpen) panels.toggleDrawer();
        QMetaObject::invokeMethod(grid, "positionViewAtEnd");
        QTest::qWait(120);
        QList<QQuickItem*> tiles;
        collectNamed(grid, "operatorCueTile", &tiles);
        int continuations = 0;
        for (auto* tile : tiles) {
            const auto row = tile->property("modelData").toMap();
            if (row.value("showId").toString() != fixture) continue;
            const int i = row.value("slideIndex").toInt();
            check(tile->property("showSectionLabel").toBool() == starts[i], "Show passes section boundaries to its actual cards");
            const QString ordinal = QString::number(i + 1);
            const QString displayed = tile->property("displayCaption").toString();
            check(!displayed.contains(QLatin1String("Section captions")),
                  "a slide strip no longer repeats the service item's name");
            check(displayed == (starts[i]
                                ? ordinal + "  " + tile->property("sectionTitle").toString()
                                : ordinal),
                  "each slide strip carries its number, and only a section start names the section");
            if (!starts[i]) ++continuations;
        }
        check(continuations == 2, "both continued sections remain visible as thumbnail cards");
        const auto fixtureSlides = docs.slidesOf(fixture);
        check(waitFor([&] {
            for (const auto& slide : fixtureSlides)
                if (worker.thumbnail(slide.toMap().value("id").toString()).isNull()) return false;
            return true;
        }), "all section and continuation cards receive real lyric thumbnails");
        for (const auto& slide : fixtureSlides) {
            const auto pixels = worker.thumbnail(slide.toMap().value("id").toString());
            check(pixels.hasAlphaChannel() && qAlpha(pixels.pixel(0, 0)) == 0,
                  "real song thumbnails preserve alpha outside the presentation content");
        }
        for (auto* tile : tiles) {
            const auto row = tile->property("modelData").toMap();
            if (row.value("showId").toString() != fixture || row.value("slideIndex").toInt() != 0) continue;
            auto* surface = tile->property("surface").value<QQuickItem*>();
            auto* painterItem = qobject_cast<QQuickPaintedItem*>(surface->property("preview").value<QQuickItem*>());
            check(painterItem != nullptr, "the blank card has a preview painter");
            if (!painterItem) continue;
            QImage painted(painterItem->size().toSize(), QImage::Format_ARGB32_Premultiplied);
            painted.fill(Qt::transparent);
            QPainter painter(&painted);
            painterItem->paint(&painter);
            painter.end();
            check(painted.pixelColor(1, 1) != painted.pixelColor(9, 1)
                      && painted.pixelColor(1, 1).alpha() == 255,
                  "the preview paints a checkerboard behind genuine alpha pixels");
        }
        if (!debugCapture.isEmpty()) window->grabWindow().save(debugCapture + "-sections-show.png");
        const auto selectedFrames = worker.selectionFrames();
        window->setProperty("page", "edit");
        docs.selectSong(fixture);
        docs.selectSlide(2);
        QTest::qWait(100);
        auto* list = findNamed(window->contentItem(), "editorSlideList");
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 2), Q_ARG(int, 0));
        QTest::qWait(100);
        tiles.clear();
        collectNamed(list, "editorSlideTile", &tiles);
        bool continued = false;
        for (auto* tile : tiles) if (tile->property("index").toInt() == 2) {
            continued = tile->property("displayCaption").toString() == QLatin1String("3")
                && tile->property("sectionTitle").toString() == "Verse 1";
        }
        check(continued && docs.currentSlideText() == "Synthetic lyric words 2",
              "Edit hides the repeated heading but keeps the slide number and the lyric words");
        check(waitFor([&] { return worker.selectionReady() && worker.selectionFrames() > selectedFrames; }),
              "the continuation's lyric words also render on the Edit canvas");
        check(!worker.selectionImage().isNull() && qAlpha(worker.selectionImage().pixel(0, 0)) == 0,
              "the large Edit preview also retains alpha outside the presentation content");
        check(waitFor([&] {
            for (int i = 2; i < fixtureSlides.size(); ++i)
                if (worker.thumbnail(fixtureSlides[i].toMap().value("id").toString()).isNull()) return false;
            return true;
        }), "Edit continuation cards receive their lyric thumbnails after changing lanes");
        if (!debugCapture.isEmpty()) window->grabWindow().save(debugCapture + "-sections-edit.png");
        window->setProperty("page", "show");
        if (drawerWasOpen) panels.toggleDrawer();
        docs.undo();
        docs.selectSong(oldShow);
        docs.selectSlide(oldSlide);
        check(docs.playlistEntries() == oldEntries, "section-caption browsing adds no document edits");
    }

    // Service browsing must resolve actual shows, even when they are absent
    // from the Library's authored-presentation projection.
    auto* showPage = findNamed(window->contentItem(), "showPage");
    auto browseServiceEntry = [&](int index) {
        selectFolder(serviceFolder);
        const auto entry = docs.playlistEntries()[index].toMap();
        const auto showId = entry.value("showId").toString();
        QMetaObject::invokeMethod(playlistList, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 1));
        QTest::qWait(50);
        QList<QQuickItem*> rows;
        collectNamed(playlistList, "collectionPlaylistRow", &rows);
        bool clicked = false;
        for (auto* row : rows) {
            if (row->property("modelData").toMap().value("entryId") != entry.value("entryId")) continue;
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              row->mapToScene(QPointF(25, 14)).toPoint());
            clicked = true;
            break;
        }
        QTest::qWait(30);
        const auto value = showPage->property("cueRows");
        const auto cues = value.canConvert<QJSValue>()
            ? value.value<QJSValue>().toVariant().toList() : value.toList();
        QStringList expectedKeys, actualKeys;
        for (const auto& groupValue : docs.runningOrderCues()) {
            const auto group = groupValue.toMap();
            for (const auto& slide : group.value("slides").toList())
                expectedKeys.append(group.value("entryId").toString() + ":" + slide.toMap().value("id").toString());
        }
        for (const auto& cue : cues) {
            actualKeys.append(cue.toMap().value("cueKey").toString());
            check(!docs.sceneJsonForSlide(cue.toMap().value("slideId").toString()).isEmpty(),
                  "every service slide still compiles after selecting a group");
        }
        check(clicked && !cues.isEmpty() && actualKeys == expectedKeys,
              "service clicks retain every slide and repeated occurrence in running order");
        const int current = showPage->property("cueIndex").toInt();
        check(current >= 0 && current < cues.size()
                  && cues[current].toMap().value("entryId") == entry.value("entryId")
                  && cues[current].toMap().value("slideIndex").toInt() == 0,
              "service clicks select the first slide of the chosen occurrence without filtering");
        check(showPage->property("sourceShowId").toString().isEmpty(),
              "service clicks leave Library browsing and follow the full service");
        check(!worker.wordsOnAir() && worker.sceneActive() == 0,
              "service browsing never presents to the audience");
    };
    window->setProperty("browsedShowId", fillerId);  // previously browsing just a Library deck
    browseServiceEntry(0);  // authored presentation
    auto* cueGrid = findNamed(window->contentItem(), "operatorSlideGrid");
    QList<QQuickItem*> groupHeaders;
    collectNamed(cueGrid, "serviceGroupHeader", &groupHeaders);
    int visibleHeaders = 0;
    bool namedFirst = false;
    const auto firstEntryName = docs.playlistEntries().isEmpty()
        ? QString() : docs.playlistEntries().first().toMap()
              .value("showName").toString();
    for (auto* header : groupHeaders) {
        if (!header->isVisible()) continue;
        ++visibleHeaders;
        auto* label = findNamed(header, "serviceGroupHeaderText");
        check(header->width() >= cueGrid->width() - 1 && header->height() >= 18,
              "service boundaries have a bigger full-width band above the next group");
        check(label && label->isVisible() && !label->property("text").toString().isEmpty()
                  && !label->property("text").toString().contains(QLatin1Char('\n'))
                  && label->width() >= header->width() - 16,
              "the band names the service item in one line");
        if (label && label->property("text").toString() == firstEntryName)
            namedFirst = true;
    }
    check(visibleHeaders > 0,
          "every service occurrence, the first included, is headed by its item's name");
    check(namedFirst, "the first group's band carries that item's name");
    browseServiceEntry(docs.playlistEntries().size() - 1);  // catalog song
    check(docs.addScripturePassage(docs.currentPlaylistId(), "Mark 1:1-4", "TEST", "Fixture",
                                  {QStringList{"Synthetic passage words"}}), "prepare a schedule-only passage");
    browseServiceEntry(docs.playlistEntries().size() - 1);
    docs.undo();
    check(docs.removeLibraryShows({fillerId}), "detach the authored presentation from the Library");
    browseServiceEntry(0);  // retained service copy after Library deletion
    docs.undo();

    // Reordering is a Show-mode operator action. Exercise the real mouse
    // gesture, preserving occurrence identities even for repeated shows.
    auto entryKeys = [](const QVariantList& entries) {
        QStringList keys;
        for (const auto& entry : entries) keys.append(entry.toMap().value("entryId").toString());
        return keys;
    };
    auto* dropArea = findNamed(window->contentItem(), "serviceDropArea");
    auto dragService = [&](int from, int gap, bool scroll, bool outside) {
        playlistList->setProperty("contentY", 0);
        QTest::qWait(50);
        const auto before = docs.playlistEntries();
        const auto ids = entryKeys(before);
        QList<QQuickItem*> rows;
        collectNamed(playlistList, "collectionPlaylistRow", &rows);
        QQuickItem* source = nullptr;
        for (auto* row : rows)
            if (row->property("rowEntryId").toString() == ids[from]) source = row;
        if (!check(source != nullptr, "the reorder source is visible")) return;
        const auto start = source->mapToScene(QPointF(25, 15)).toPoint();
        const auto target = outside ? QPoint(window->width() / 2, 15)
            : playlistList->mapToScene(QPointF(60, scroll ? playlistList->height() - 6 : gap * 28 + 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(45, 0), 20);
        QTest::mouseMove(window, target, 20);
        QTest::qWait(scroll ? 300 : 40);
        auto* marker = findNamed(window->contentItem(), "serviceDropIndicator");
        QList<QQuickItem*> badges;
        collectNamed(window->contentItem(), "listDragFeedback", &badges);
        int visibleBadges = 0;
        for (auto* badge : badges) if (badge->isVisible()) {
            ++visibleBadges;
            check(QLineF(badge->mapToScene(QPointF()), target).length() < 2,
                  "service drag feedback stays at the pointer while the list scrolls");            check(badge->property("payload").toString() == "entry:" + ids[from]
                      && badge->property("moving").toBool(), "drag feedback identifies the exact service occurrence");
        }
        check(visibleBadges == 1 && marker && marker->isVisible() == !outside,
              "a service drag shows one overlay badge and a marker only over a valid destination");
        const int landedGap = dropArea->property("insertionIndex").toInt();
        if (scroll) check(playlistList->property("contentY").toReal() > 0,
                          "a service reorder autoscrolls with its source held outside the viewport");
        else if (!outside) check(landedGap == gap, "the insertion marker follows the exact pointer gap");
        if (scroll && !debugCapture.isEmpty()) window->grabWindow().save(debugCapture + "-drag-move.png");
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
        QTest::qWait(50);
        auto expected = ids;
        if (!outside) {
            const auto moved = expected.takeAt(from);
            expected.insert(from < landedGap ? landedGap - 1 : landedGap, moved);
        }
        check(entryKeys(docs.playlistEntries()) == expected,
              "dropping reorders exact entry identities without adding or losing a service item");
        if (!outside) check(findNamed(window->contentItem(), "currentServicePanel")
                                ->property("selectedEntryId").toString() == ids[from],
                            "the dropped occurrence remains selected for subsequent keyboard actions");
        check(marker && !marker->isVisible(), "the insertion marker disappears after release");
        check(!worker.wordsOnAir() && worker.sceneActive() == 0, "service reordering never presents");
        if (expected != ids) {
            if (scroll) {
                check(docs.save(), "the reordered service saves");
                openshow::ui::DocumentController reopened(nullptr, root / "service");
                check(entryKeys(reopened.playlistEntries()) == expected, "the reordered service survives restart");
            }
            docs.undo();
            check(docs.playlistEntries() == before, "one undo restores the exact order before a drag");
        }
    };
    check(window->property("page") == "show", "reorder tests run in Show mode");
    dragService(0, 2, false, false);  // down across another occurrence
    dragService(2, 0, false, false);  // up to the beginning
    dragService(1, 1, false, false);  // same gap changes nothing
    dragService(0, 0, false, true);   // release outside any target
    dragService(0, 0, true, false);   // scroll while dragging

    // Windows-style selection/deletion is available on Show, scoped to the
    // active list and keyed by entry identity (including repeated shows).
    auto selectedCount = [](QObject* panel, const char* property) {
        const auto value = panel->property(property);
        return value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toList().size()
                                           : value.toList().size();
    };
    auto* servicePanel = findNamed(window->contentItem(), "currentServicePanel");
    playlistList->setProperty("contentY", 0);
    QTest::qWait(60);
    const auto beforeRemoval = docs.playlistEntries();
    auto selectEntry = [&](int index, Qt::KeyboardModifiers modifiers) {
        QList<QQuickItem*> rows;
        collectNamed(playlistList, "collectionPlaylistRow", &rows);
        const auto id = beforeRemoval[index].toMap().value("entryId");
        for (auto* row : rows) if (row->property("modelData").toMap().value("entryId") == id) {
            QTest::mouseClick(window, Qt::LeftButton, modifiers,
                row->mapToScene(QPointF(25, 14)).toPoint());
            return;
        }
        check(false, "the service selection row is visible");
    };
    check(!findNamed(window->contentItem(), "serviceRemoveSelectedButton"),
          "the service header no longer has a redundant Remove button");
    selectEntry(0, Qt::NoModifier);
    QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
    check(selectedCount(servicePanel, "selectedEntryIds") == 3,
          "Shift+Down extends the range from the clicked anchor");
    QTest::keyClick(window, Qt::Key_Up, Qt::ShiftModifier);
    check(selectedCount(servicePanel, "selectedEntryIds") == 2,
          "Shift+Up contracts the range without losing its anchor");
    QTest::keyClick(window, Qt::Key_Delete);
    const auto afterKeyboardRemoval = docs.playlistEntries();
    bool keyboardSurvivors = afterKeyboardRemoval.size() == beforeRemoval.size() - 2;
    for (int i = 0; i < afterKeyboardRemoval.size(); ++i)
        keyboardSurvivors = keyboardSurvivors && afterKeyboardRemoval[i].toMap().value("entryId")
                            == beforeRemoval[i + 2].toMap().value("entryId");
    check(keyboardSurvivors, "Delete removes exactly the keyboard-selected service occurrences");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    check(docs.playlistEntries() == beforeRemoval,
          "one undo restores the keyboard range in order");
    selectEntry(0, Qt::NoModifier);
    QTest::keyClick(window, Qt::Key_Up, Qt::ShiftModifier);
    check(selectedCount(servicePanel, "selectedEntryIds") == 1,
          "Shift+Up stops at the first service entry");
    for (int i = 0; i < beforeRemoval.size() + 2; ++i)
        QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
    check(selectedCount(servicePanel, "selectedEntryIds") == beforeRemoval.size()
              && playlistList->property("contentY").toReal() > 0,
          "keyboard selection reaches offscreen rows, scrolls and stops at the last entry");
    QTest::keyClick(window, Qt::Key_Up);
    check(selectedCount(servicePanel, "selectedEntryIds") == 1,
          "an unmodified arrow replaces the range with one row");
    playlistList->setProperty("contentY", 0);
    QTest::qWait(40);
    selectEntry(0, Qt::NoModifier);
    selectEntry(2, Qt::ControlModifier);
    check(servicePanel && selectedCount(servicePanel, "selectedEntryIds") == 2,
          "Ctrl-click selects two distinct service entries on Show");
    QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
    QTest::qWait(40);
    search->setProperty("text", "morningx");
    QTest::keyClick(window, Qt::Key_End);
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Delete);
    check(search->property("text") == "morning" && docs.playlistEntries() == beforeRemoval,
          "Delete while typing edits search text and leaves selected service entries alone");
    QTest::keyClick(window, Qt::Key_Escape);
    selectEntry(0, Qt::NoModifier);
    selectEntry(2, Qt::ShiftModifier);
    check(selectedCount(servicePanel, "selectedEntryIds") == 3, "Shift-click selects the service range");
    selectEntry(1, Qt::ControlModifier);
    check(selectedCount(servicePanel, "selectedEntryIds") == 2, "Ctrl-click toggles a member out of the range");
    QTest::keyClick(window, Qt::Key_Delete);
    check(docs.playlistEntries().size() == beforeRemoval.size() - 2 && library.songCount() == 1,
          "Delete removes only the two selected occurrences and preserves the Library song");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(40);
    check(docs.playlistEntries() == beforeRemoval, "one undo restores the entire service selection in order");
    servicePanel->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    check(selectedCount(servicePanel, "selectedEntryIds") == beforeRemoval.size(),
          "Ctrl+A selects the complete service, including scrolled-off rows");
    QTest::keyClick(window, Qt::Key_Delete);
    check(docs.playlistEntries().isEmpty(), "Delete removes the complete service selection without a dialog");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    check(docs.playlistEntries() == beforeRemoval, "one undo restores a complete service removal");

    // Persistence: reopen the service and the catalog.
    const int entries = static_cast<int>(docs.playlistEntries().size());
    check(docs.save(), "the service saves");
    {
        openshow::ui::DocumentController reopened(nullptr, root / "service");
        check(static_cast<int>(reopened.playlistEntries().size()) == entries,
              "the running order survives a service restart");
        const QString songId =
            library.items().first().toMap().value("id").toString();
        check(!reopened.showForCatalogItem(songId).isEmpty(),
              "the catalog reference survives a service restart");
    }
    {
        openshow::ui::CatalogController reopened(nullptr, root);
        check(reopened.songCount() == 1, "the Library survives a restart");
        check(reopened.items().first().toMap().value("ccli").toString()
                  == QStringLiteral("9000001"),
              "the CCLI number survives a restart");
    }
    // The same stable-ID selection works across both Library branches.
    const QVariantList stanzas{QVariantMap{{"label", "Verse"}, {"lines", QVariantList{"Bulk words"}}}};
    const auto alpha = library.addSong("Bulk Alpha", stanzas, "", "", "");
    const auto beta = library.addSong("Bulk Beta", stanzas, "", "", "");
    const auto presentation = library.addSong("Bulk Presentation", stanzas, "", "", "");
    library.moveItemToCategory(presentation, "presentations");
    search->setProperty("text", "bulk");
    selectFolder(songsFolder);
    songList->setProperty("contentY", 0);
    QTest::qWait(50);
    auto* browser = findNamed(window->contentItem(), "libraryBrowser");
    auto* presentationList = findNamed(window->contentItem(), "collectionLibraryList");
    auto selectLibrary = [&](const QString& kind, const QString& id, Qt::KeyboardModifiers modifiers) {
        QQuickItem* list = nullptr;
        int index = -1;
        for (const auto& spec : {std::pair<const char*, QQuickItem*>{"songRows", songList},
                                {"presentationRows", presentationList}}) {
            const auto property = browser->property(spec.first);
            const auto model = property.canConvert<QJSValue>()
                ? property.value<QJSValue>().toVariant().toList() : property.toList();
            for (int i = 0; i < model.size(); ++i) {
                const auto data = model[i].toMap();
                if (data.value("id").toString() == id && data.value("rowKind", "catalog").toString() == kind) {
                    list = spec.second; index = i; break;
                }
            }
            if (list) break;
        }
        if (!check(list != nullptr, "the Library selection row exists in its filtered model")) return;
        // The row only exists in the folder that lists it.
        selectFolder(list == songList ? songsFolder : presentationsFolder);
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 1));
        QTest::qWait(30);
        QList<QQuickItem*> rows;
        collectNamed(list, list == songList ? "librarySongRow" : "collectionLibraryRow", &rows);
        for (auto* row : rows) {
            const auto data = row->property("modelData").toMap();
            if (data.value("id").toString() != id || data.value("rowKind", "catalog").toString() != kind) continue;
            const auto point = row->mapToItem(list, QPointF(25, 14));
            if (!check(QRectF(0, 0, list->width(), list->height()).contains(point),
                       "selection scrolls the real row centre inside its viewport")) return;
            QTest::mouseClick(window, Qt::LeftButton, modifiers, list->mapToScene(point).toPoint());
            return;
        }
        check(false, "the Library selection row is instantiated after scrolling");
    };
    const auto originalService = docs.playlistEntries();
    selectLibrary("catalog", alpha, Qt::NoModifier);
    selectLibrary("catalog", beta, Qt::ControlModifier);
    check(selectedCount(browser, "selectedLibraryKeys") == 2, "Ctrl-click selects multiple Library songs");
    const auto capture = qEnvironmentVariable("OPENSHOW_LIBRARY_CAPTURE");
    if (!capture.isEmpty()) {
        for (const auto size : {QSize(1000, 680), QSize(1380, 800), QSize(1680, 1020)}) {
            window->resize(size);
            QTest::qWait(200);
            check(window->grabWindow().save(capture + "-" + QString::number(size.width()) + ".png"),
                  "save the populated Library multi-selection frame");
        }
        window->resize(1380, 800);
        QTest::qWait(60);
    }
    check(!findNamed(window->contentItem(), "libraryRemoveSelectedButton"),
          "the Library header no longer has a redundant Remove button");
    auto requestLibraryDelete = [&] {
        QTest::keyClick(window, Qt::Key_Delete);
        waitFor([&] {
            auto* button = findNamed(window->contentItem(), "libraryConfirmDeleteButton");
            auto* cancel = findNamed(window->contentItem(), "libraryCancelDeleteButton");
            return button && button->isVisible() && cancel && cancel->hasActiveFocus();
        }, 1000);
        auto* confirm = findNamed(window->contentItem(), "libraryConfirmDeleteButton");
        auto* warning = findNamed(window->contentItem(), "libraryDeleteWarningText");
        check(confirm && confirm->isVisible() && warning
                  && warning->property("text").toString().contains("cannot be undone"),
              "Library Delete opens an explicit permanent-deletion warning");
    };
    auto confirmLibraryDelete = [&] {
        click(findNamed(window->contentItem(), "libraryConfirmDeleteButton"));
        waitFor([&] { return browser->hasActiveFocus(); }, 1000);
    };
    requestLibraryDelete();
    if (!capture.isEmpty()) window->grabWindow().save(capture + "-confirmation.png");
    check(library.items().size() == 4 && docs.playlistEntries() == originalService,
          "opening the warning changes neither database nor service");
    QTest::keyClick(window, Qt::Key_Return);
    waitFor([&] { return browser->hasActiveFocus(); }, 1000);
    check(library.items().size() == 4 && browser->hasActiveFocus(),
          "Enter defaults to Cancel and returns focus to Library");
    requestLibraryDelete();
    QTest::keyClick(window, Qt::Key_Escape);
    waitFor([&] { return browser->hasActiveFocus(); }, 1000);
    check(library.items().size() == 4 && selectedCount(browser, "selectedLibraryKeys") == 2,
          "Escape cancels deletion and retains the selection");
    requestLibraryDelete();
    click(findNamed(window->contentItem(), "libraryCancelDeleteButton"));
    waitFor([&] { return browser->hasActiveFocus(); }, 1000);
    check(library.items().size() == 4, "the Cancel button preserves the database");
    selectLibrary("catalog", alpha, Qt::NoModifier);
    selectLibrary("catalog", beta, Qt::ShiftModifier);
    selectLibrary("catalog", presentation, Qt::ControlModifier);
    check(selectedCount(browser, "selectedLibraryKeys") == 3,
          "selection spans Songs and catalog-backed Presentations without ID ambiguity");
    requestLibraryDelete();
    qputenv("OPENSHOW_CATALOG_FAULT", "write");
    confirmLibraryDelete();
    qunsetenv("OPENSHOW_CATALOG_FAULT");
    check(library.items().size() == 4 && selectedCount(browser, "selectedLibraryKeys") == 3,
          "a confirmed deletion with a failed durable write preserves all records and selection");
    requestLibraryDelete();
    library.removeItem(beta);  // model changes while the confirmation is open
    confirmLibraryDelete();
    check(library.items().size() == 3 && !library.itemOf(alpha).isEmpty()
              && !library.itemOf(presentation).isEmpty(),
          "a stale confirmation refuses the whole batch without deleting valid members");
    library.undo();
    check(library.items().size() == 4, "the external database change remains independently undoable");
    QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
    QTest::qWait(40);
    search->setProperty("text", "bulk ");
    QTest::keyClick(window, Qt::Key_End);
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Delete);
    check(search->property("text") == "bulk" && library.items().size() == 4,
          "Delete in Library search edits only its text, even with a retained selection");
    browser->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    check(selectedCount(browser, "selectedLibraryKeys") == 3,
          "Ctrl+A selects filtered Library items and excludes the hidden Morning Light song");
    requestLibraryDelete();
    confirmLibraryDelete();
    check(library.items().size() == 1 && docs.playlistEntries() == originalService,
          "confirmed Delete permanently removes the filtered selection only");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    check(library.items().size() == 1, "Library undo cannot restore permanently deleted items");
    QTest::keyClick(window, Qt::Key_Y, Qt::ControlModifier);
    check(library.items().size() == 1, "Library redo cannot restore permanently deleted items");
    search->setProperty("text", "");
    QTest::qWait(50);
    const auto linkedId = library.items().first().toMap().value("id").toString();
    const auto linkedShow = docs.showForCatalogItem(linkedId);
    selectLibrary("show", fillerId, Qt::NoModifier);
    selectLibrary("catalog", linkedId, Qt::ControlModifier);
    check(selectedCount(browser, "selectedLibraryKeys") == 2,
          "typed identities allow mixed catalog-song and authored-presentation selection");
    requestLibraryDelete();
    confirmLibraryDelete();
    check(library.items().isEmpty() && docs.playlistEntries().size() == originalService.size()
              && !docs.slidesOf(linkedShow).isEmpty() && docs.songs().isEmpty(),
          "confirmed mixed deletion retains service cues and detached song slides");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    check(docs.songs().isEmpty() && library.items().isEmpty()
              && docs.showForCatalogItem(linkedId).isEmpty(),
          "mixed undo cannot resurrect deleted sources or a deleted catalog link");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    check(docs.songs().isEmpty() && library.items().isEmpty(),
          "mixed redo cannot resurrect either permanently deleted source");
    browseServiceEntry(0);
    browseServiceEntry(docs.playlistEntries().size() - 1);
    while (docs.canUndo()) {
        docs.undo();
        check(docs.songs().isEmpty() && docs.showForCatalogItem(linkedId).isEmpty(),
              "older service undo cannot resurrect Library sources or deleted catalog links");
    }
    while (docs.canRedo()) {
        docs.redo();
        check(docs.songs().isEmpty() && docs.showForCatalogItem(linkedId).isEmpty(),
              "older service redo cannot resurrect Library sources or deleted catalog links");
    }

    check(warnings.isEmpty(), "the shell emitted no QML warnings");
    for (const QString& warning : warnings) {
        std::fprintf(stderr, "  QML: %s\n", qPrintable(warning));
    }
    check(!worker.wordsOnAir() && worker.sceneActive() == 0,
          "the whole journey presented nothing");

    worker.shutdownNow();
    if (gFailed) {
        std::fprintf(stderr, "library_service_ui_tests: FAILED\n");
        return 1;
    }
    std::printf("library_service_ui_tests: all checks passed\n");
    return 0;
}
