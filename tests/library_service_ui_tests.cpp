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
    click(findNamed(window->contentItem(), "songImportBackButton"));
    QTest::qWait(40);
    check(window->property("page") == "edit", "toolbar song import returns to its originating page");

    // A scrolled running order: a filler presentation listed many times.
    check(docs.createSong("Filler"), "a filler presentation exists");
    const QString fillerId = docs.currentSongId();
    docs.addSlide();
    docs.setSlideText("Synthetic presentation words");
    docs.flushText();
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
    if (songRow) for (auto* child : songRow->childItems())
        check(!child->property("text").toString().startsWith("CCLI "),
              "song rows show their title without a CCLI number");
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
        auto* addMarker = findNamed(window->contentItem(), "serviceDropIndicator");
        check(addMarker && addMarker->isVisible() && addMarker->height() >= 4,
              "a Library drag shows a prominent service insertion marker");
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

    // Service browsing must resolve actual shows, even when they are absent
    // from the Library's authored-presentation projection.
    auto* showPage = findNamed(window->contentItem(), "showPage");
    auto browseServiceEntry = [&](int index) {
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
    QList<QQuickItem*> groupRules;
    collectNamed(cueGrid, "serviceGroupSeparator", &groupRules);
    int visibleRules = 0;
    for (auto* rule : groupRules) {
        if (!rule->isVisible()) continue;
        ++visibleRules;
        check(rule->width() >= cueGrid->width() - 12 && rule->height() >= 2
                  && rule->y() + rule->height() < 0,
              "service boundaries have a full-width visible rule in the gap above the next group");
    }
    check(visibleRules > 0, "separate occurrences of the same show still have group separators");
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
            : playlistList->mapToScene(QPointF(60, scroll ? playlistList->height() - 6 : gap * 30 + 2)).toPoint();
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
