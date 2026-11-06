/*
 * Task SL.9 (integrated): the whole Library journey on the production shell
 * with a REAL worker — clipboard import, a filtered Library row dragged into
 * a SCROLLED running order, the keyboard/button alternative, one undo, and
 * service/catalog persistence across a restart. Nothing is presented.
 *
 * Needs a display for the worker's output window: desktop preset, label
 * display.
 */

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJSValue>
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
#include "ServiceCatalogBridge.h"
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
    context->setContextProperty("uiSize", QSize(1380, 800));
    engine.loadFromModule("OpenShow", "Main");
    auto* window = engine.rootObjects().isEmpty()
                       ? nullptr
                       : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!check(window != nullptr, "the production shell loads")) {
        worker.shutdownNow();
        return 1;
    }
    QTest::qWait(150);

    // Import is a preparation action, reachable directly from Show.
    auto click = [window](QQuickItem* item) {
        if (item) QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    };
    auto* importButton = findNamed(window->contentItem(), "libraryImportSongButton");
    auto* importLabel = importButton ? importButton->property("contentItem").value<QQuickItem*>() : nullptr;
    check(importLabel && importLabel->height() >= importLabel->implicitHeight(),
          "the compact Import song button has room to render its label");
    check(window->property("page") == "show" && importButton && importButton->isVisible(),
          "Import song is visible on Show without entering Edit");
    click(importButton);
    QTest::qWait(50);
    check(window->property("page") == "songimport", "the Library button opens clipboard import");
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
    click(findNamed(window->contentItem(), "toolbarMore"));
    QTest::qWait(40);
    auto* importMenu = findNamed(window->contentItem(), "songImportMenuItem");
    check(importMenu && importMenu->isVisible(), "More also exposes clipboard import");
    click(importMenu);
    QTest::qWait(40);
    check(window->property("page") == "songimport", "the menu opens the same import page");
    click(findNamed(window->contentItem(), "songImportBackButton"));
    QTest::qWait(40);

    // A scrolled running order: a filler presentation listed many times.
    check(docs.createSong("Filler"), "a filler presentation exists");
    const QString fillerId = docs.currentSongId();
    for (int i = 0; i < 30; ++i) {
        docs.addShowToPlaylist(fillerId);
    }
    window->setProperty("page", "show");
    QTest::qWait(120);
    auto* playlistList = findNamed(window->contentItem(), "collectionPlaylistList");
    auto* songList = findNamed(window->contentItem(), "librarySongList");
    auto* search = findNamed(window->contentItem(), "librarySearchField");
    if (!check(playlistList && songList && search, "the two Library panes are present")) {
        worker.shutdownNow();
        return 1;
    }
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
    playlistList->setProperty("contentY", 100);
    QTest::qWait(40);

    // Filter the Library to the imported song, then drag the filtered row
    // into the scrolled order.
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
    if (songRow) {
        const int before = static_cast<int>(docs.playlistEntries().size());
        const QPointF start = songRow->mapToScene(QPointF(25, 14));
        const QPointF target = playlistList->mapToScene(
            QPointF(40, std::max(10.0, playlistList->height() - 14.0)));
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        QTest::mouseMove(window, (start + QPointF(60, 0)).toPoint(), 15);
        QTest::mouseMove(window, ((start + target) / 2 + QPointF(30, 0)).toPoint(), 15);
        QTest::mouseMove(window, (target + QPointF(20, 0)).toPoint(), 15);
        QTest::mouseMove(window, target.toPoint(), 15);
        const qreal scrollBefore = playlistList->property("contentY").toReal();
        QTest::qWait(260);  // hold at the bottom edge: autoscroll ticks
        check(playlistList->property("contentY").toReal() > scrollBefore,
              "holding a drag at the edge autoscrolls the long order");
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target.toPoint());
        check(waitFor([&] { return !songService.busy(); }), "the drop prepared the song");
        check(static_cast<int>(docs.playlistEntries().size()) == before + 1,
              "the filtered song was added to the scrolled order");
        const QString showId = docs.showForCatalogItem(
            library.items().first().toMap().value("id").toString());
        check(!showId.isEmpty(), "the cue references a catalog-backed show");
        bool named = false;
        for (const QVariant& entry : docs.playlistEntries()) {
            named = named || entry.toMap().value("showId").toString() == showId;
        }
        check(named, "the scrolled order holds a cue naming the referenced show");
        check(search->property("text") == "morning",
              "dragging the filtered result keeps the search query unchanged");
        check(!docs.slidesOf(showId).isEmpty(), "the linked show carries slides");
    }
    check(!worker.wordsOnAir() && worker.sceneActive() == 0,
          "dragging and preparing never presented");

    // The keyboard/button alternative: select the row and use Add to service.
    {
        QList<QQuickItem*> rows;
        collectNamed(songList, "librarySongRow", &rows);
        if (!rows.isEmpty()) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              rows.first()->mapToScene(QPointF(20, 14)).toPoint());
            QTest::qWait(20);
        }
        auto* addButton = findNamed(window->contentItem(), "libraryAddSongButton");
        check(addButton && addButton->property("enabled").toBool(),
              "Add to service wakes for the selected song");
        const int before = static_cast<int>(docs.playlistEntries().size());
        if (addButton) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              addButton->mapToScene(
                                  QPointF(addButton->width() / 2,
                                          addButton->height() / 2)).toPoint());
        }
        check(waitFor([&] { return !songService.busy(); }),
              "the button preparation finished");
        check(static_cast<int>(docs.playlistEntries().size()) == before + 1,
              "the button added another cue");
        docs.undo();
        check(static_cast<int>(docs.playlistEntries().size()) == before,
              "one undo removes the whole button add");
    }

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
    click(findNamed(window->contentItem(), "serviceRemoveSelectedButton"));
    check(docs.playlistEntries().isEmpty(), "the visible Remove action removes the service selection");
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
    QTest::keyClick(window, Qt::Key_Delete);
    check(library.items().size() == 2 && docs.playlistEntries() == originalService,
          "Library Delete removes selected songs while leaving the service alone");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(50);
    check(library.items().size() == 4, "one Library undo restores both selected songs");
    QTest::keyClick(window, Qt::Key_Y, Qt::ControlModifier);
    check(library.items().size() == 2 && docs.playlistEntries() == originalService,
          "focused Library redo repeats the entire catalog batch without changing service cues");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(50);
    selectLibrary("catalog", alpha, Qt::NoModifier);
    selectLibrary("catalog", beta, Qt::ShiftModifier);
    selectLibrary("catalog", presentation, Qt::ControlModifier);
    check(selectedCount(browser, "selectedLibraryKeys") == 3,
          "selection spans Songs and catalog-backed Presentations without ID ambiguity");
    qputenv("OPENSHOW_CATALOG_FAULT", "write");
    QTest::keyClick(window, Qt::Key_Delete);
    qunsetenv("OPENSHOW_CATALOG_FAULT");
    check(library.items().size() == 4 && selectedCount(browser, "selectedLibraryKeys") == 3,
          "a durable-write refusal preserves the complete Library selection");
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
    click(findNamed(window->contentItem(), "libraryRemoveSelectedButton"));
    check(library.items().size() == 1 && docs.playlistEntries() == originalService,
          "the Library Remove button deletes the filtered multi-selection only");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(50);
    search->setProperty("text", "");
    QTest::qWait(50);
    const auto linkedId = library.items().first().toMap().value("id").toString();
    const auto linkedShow = docs.showForCatalogItem(linkedId);
    selectLibrary("show", fillerId, Qt::NoModifier);
    selectLibrary("catalog", linkedId, Qt::ControlModifier);
    check(selectedCount(browser, "selectedLibraryKeys") == 2,
          "typed identities allow mixed catalog-song and authored-presentation selection");
    QTest::keyClick(window, Qt::Key_Delete);
    check(docs.playlistEntries().size() == originalService.size()
              && !docs.slidesOf(linkedShow).isEmpty() && docs.songs().isEmpty(),
          "removing Library sources retains all their service cues and keeps detached songs out of Library");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    check(!docs.songs().isEmpty() && library.items().size() == 4,
          "each affected store restores its Library removal with one scoped undo");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    check(docs.songs().isEmpty() && library.items().size() == 3
              && docs.playlistEntries().size() == originalService.size(),
          "focused mixed redo removes both Library sources while preserving the service");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);

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
