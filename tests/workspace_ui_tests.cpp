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
#include <QtMath>

#include "AppSettings.h"
#include "DocumentController.h"
#include "WorkerController.h"
#include "WorkspacePanels.h"
#include "openshow/protocol/preview_wire.h"

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
    docs.addSlide();
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
    auto* mediaBin = window->findChild<QQuickItem*>("mediaBin");
    auto* grid = window->findChild<QQuickItem*>("operatorSlideGrid");
    auto* editor = window->findChild<QQuickItem*>("slideTextEditor");
    auto* preview = window->findChild<QQuickItem*>("audiencePreview");
    if (!left || !right || !mediaBin || !grid || !editor || !preview) return 1;
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
        check(mediaBin->mapToScene(QPointF(0, 0)).y() > 300,
              "the media bin leaves the top of the slide workspace visible");
    }
    panels.resizeDrawer(300, window->height() - 364);
    QTest::qWait(30);
    // The library rows this drag used to test now live in the sidebar, so the
    // "a row dragged into blank space is not displaced" pin lives with them in
    // the UI.2 section below, where collectionLibraryRow delegates exist.
    // --- task UI.3b: cue triggers, running-order stepping and markers ------
    std::function<QQuickItem*(QQuickItem*, const char*)> findNamed =
        [&](QQuickItem* item, const char* name) -> QQuickItem* {
            if (item->objectName() == QLatin1String(name)) return item;
            for (auto* child : item->childItems())
                if (auto* hit = findNamed(child, name)) return hit;
            return nullptr;
        };
    std::function<void(QQuickItem*, const char*, QList<QQuickItem*>&)> collectNamed =
        [&](QQuickItem* item, const char* name, QList<QQuickItem*>& out) {
            if (item->objectName() == QLatin1String(name)) out.append(item);
            for (auto* child : item->childItems()) collectNamed(child, name, out);
        };
    auto* showPageItem = window->findChild<QQuickItem*>("showPage");
    auto* cueGrid = window->findChild<QQuickItem*>("operatorSlideGrid");
    check(showPageItem && cueGrid, "the Show page and its cue grid are in the operator shell");
    {
        // The running order is Good Grace (18 cues) then Welcome (1): the
        // grid renders the ORDER, not whatever the editing cursor sits on.
        const QVariantList rows = showPageItem->property("cueRows").toList();
        QStringList groupsInOrder;
        for (const QVariant& row : rows) {
            const QString name = row.toMap().value("showName").toString();
            if (groupsInOrder.isEmpty() || groupsInOrder.last() != name)
                groupsInOrder << name;
        }
        check(rows.size() == 19 &&
                  groupsInOrder == (QStringList{"Good Grace", "Welcome"}),
              "the grid shows the running order's cues, not just the edited show");
        // The implicit follow-arming state is gone from the operator screen.
        std::function<bool(QQuickItem*)> followArmed = [&](QQuickItem* item) {
            if (item->property("text").toString() == QLatin1String("Follow selection"))
                return true;
            for (auto* child : item->childItems())
                if (followArmed(child)) return true;
            return false;
        };
        check(!followArmed(window->contentItem()),
              "no follow-arming control remains on Show");
        // Clicking a cue picks it and asks the worker; with no acknowledgement
        // available nothing may light, and the editing cursor may not move.
        QList<QQuickItem*> tiles;
        collectNamed(cueGrid, "operatorCueTile", tiles);
        QQuickItem* firstTile = nullptr;
        for (auto* tile : tiles)
            if (tile->property("index").toInt() == 0) firstTile = tile;
        check(firstTile != nullptr, "the first cue is on screen");
        if (firstTile != nullptr) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              firstTile->mapToScene(QPointF(25, 25)).toPoint());
            QTest::qWait(30);
            check(showPageItem->property("cueIndex").toInt() == 0,
                  "clicking a cue picks that cue");
            check(docs.currentSongId() == songId && docs.currentSlide() == 0,
                  "triggering a cue never moves the editing cursor");
            check(firstTile->property("rowSelected").toBool()
                      && !firstTile->property("rowLive").toBool(),
                  "a click selects without claiming anything is live");
            check(worker.liveCueKey().isEmpty() && worker.pendingCueKey().isEmpty(),
                  "a go the worker never received leaves no marker pending");
        }
        // The presenter keys walk the running order ACROSS the presentation
        // boundary (cue 18 of Good Grace into Welcome) and clamp at the ends.
        // The cue tiles never take focus, so the window's presenter
        // shortcuts keep receiving keys after a click inside the grid.
        showPageItem->setProperty("cueIndex", 17);
        QTest::qWait(30);
        QTest::keyClick(window, Qt::Key_PageDown);
        QTest::qWait(30);
        check(showPageItem->property("cueIndex").toInt() == 18 &&
                  rows[18].toMap().value("showName").toString() == QLatin1String("Welcome"),
              "Next crosses the boundary into the following presentation");
        QTest::keyClick(window, Qt::Key_PageDown);
        QTest::qWait(20);
        check(showPageItem->property("cueIndex").toInt() == 18,
              "stepping at the end stays at the end");
        QTest::keyClick(window, Qt::Key_PageUp);
        QTest::qWait(30);
        check(showPageItem->property("cueIndex").toInt() == 17,
              "Previous crosses back into the first presentation");
        check(docs.currentSongId() == songId && docs.currentSlide() == 0,
              "the whole walk leaves the editing cursor untouched");
        // The Welcome cue at the end of the order is clickable once the
        // grid has scrolled to it (delegates only exist on screen).
        QMetaObject::invokeMethod(cueGrid, "positionViewAtEnd");
        QTest::qWait(40);
        QList<QQuickItem*> tailTiles;
        collectNamed(cueGrid, "operatorCueTile", tailTiles);
        QQuickItem* welcomeTile = nullptr;
        for (auto* tile : tailTiles)
            if (tile->property("modelData").toMap().value("showName").toString() ==
                QLatin1String("Welcome"))
                welcomeTile = tile;
        check(welcomeTile != nullptr, "the last presentation's cue is reachable by scrolling");
        int liveCount = 0;
        for (auto* tile : tailTiles)
            if (tile->property("rowLive").toBool()) ++liveCount;
        check(liveCount == 0, "nothing is marked live without an acknowledgement");
        // Browsing a presentation from the library re-sources the middle of
        // the screen off air — still no cursor move, still nothing live.
        QString welcomeId;
        for (const auto& song : docs.songs())
            if (song.toMap().value("name").toString() == QLatin1String("Welcome"))
                welcomeId = song.toMap().value("id").toString();
        showPageItem->setProperty("sourceShowId", welcomeId);
        QTest::qWait(40);
        QList<QQuickItem*> browsed;
        collectNamed(cueGrid, "operatorCueTile", browsed);
        check(browsed.size() == 1
                  && browsed.first()->property("modelData").toMap().value("showName")
                         .toString() == QLatin1String("Welcome")
                  && !browsed.first()->property("rowLive").toBool(),
              "browsing a presentation shows only its slides and presents nothing");
        check(docs.currentSongId() == songId, "browsing never moves the editing cursor");
        showPageItem->setProperty("sourceShowId", QString());
        QTest::qWait(40);
    }

    // --- task UI.2: one collection outline, one detail view --------------
    auto* outline = findNamed(window->contentItem(), "collectionOutline");
    auto* libraryList = findNamed(window->contentItem(), "collectionLibraryList");
    auto* playlistList = findNamed(window->contentItem(), "collectionPlaylistList");
    check(outline && libraryList && playlistList,
          "the sidebar is one outline above one detail view");
    check(outline->property("count").toInt() == static_cast<int>(docs.playlists().size()) + 1,
          "the outline carries the Library plus every playlist");
    check(left->property("outlineKind").toString() == QLatin1String("playlist") &&
              left->property("outlinePlaylistId").toString() == docs.currentPlaylistId(),
          "the running order under preparation is the detail view that opens");
    check(playlistList->isVisible() &&
              playlistList->property("count").toInt() ==
                  static_cast<int>(docs.playlistEntries().size()),
          "the detail view shows the selected order's cues");
    // The outline re-binds whenever the documents change, so row pointers are
    // only trusted straight after a refresh.
    QList<QQuickItem*> outlineRows;
    auto refreshOutlineRows = [&] {
        outlineRows.clear();
        collectNamed(outline, "collectionOutlineRow", outlineRows);
    };
    refreshOutlineRows();
    check(!outlineRows.isEmpty() &&
              outlineRows.first()->property("modelData").toMap().value("kind").toString() ==
                  QLatin1String("library"),
          "the Library leads the outline");
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      outlineRows.first()->mapToScene(QPointF(20, 14)).toPoint());
    QTest::qWait(20);
    check(left->property("outlineKind").toString() == QLatin1String("library") &&
              libraryList->isVisible() &&
              libraryList->property("count").toInt() == static_cast<int>(docs.songs().size()),
          "selecting the Library moves only the detail view");
    // Long names stay one bounded, selectable row.
    const QString longName(180, 'x');
    check(docs.createSong(longName), "a long-named presentation is created");
    QTest::qWait(20);
    {
        QList<QQuickItem*> rows;
        collectNamed(libraryList, "collectionLibraryRow", rows);
        QQuickItem* longRow = nullptr;
        for (auto* row : rows)
            if (row->property("modelData").toMap().value("name").toString() == longName)
                longRow = row;
        check(longRow && longRow->width() <= libraryList->width() && longRow->height() > 0,
              "a long name stays one bounded row in the detail view");
        if (longRow) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              longRow->mapToScene(QPointF(20, 14)).toPoint());
            QTest::qWait(20);
        }
        check(!docs.currentSongId().isEmpty(), "and the long row is selectable");
    }
    // Dragging a library row into blank space must not displace it — the pin
    // the flat drawer used to carry, kept with the rows that now answer for it.
    {
        QList<QQuickItem*> libraryRows;
        collectNamed(libraryList, "collectionLibraryRow", libraryRows);
        check(!libraryRows.isEmpty(), "the Library detail instantiates rows");
        if (!libraryRows.isEmpty()) {
            QPointer<QQuickItem> row = libraryRows.first();
            const auto position = row->position();
            const auto start = row->mapToScene(QPointF(25, 14)).toPoint();
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, start + QPoint(100, 20), 20);
            QTest::mouseMove(window, start + QPoint(160, -30), 20);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                start + QPoint(160, -30));
            check(row && row->position() == position,
                  "a library row dragged into blank space stays in place");
        }
    }
    docs.selectSong(songId);  // back to the fixture's working song for later checks
    // Repeated entries: two cues, two stable ids, one selection.
    QTest::qWait(20);
    refreshOutlineRows();
    check(outlineRows.size() >= 2, "the outline still rows after a document change");
    if (outlineRows.size() >= 2) {
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          outlineRows[1]->mapToScene(QPointF(20, 14)).toPoint());
    }
    QTest::qWait(20);
    check(docs.addShowToPlaylist(songId), "the order takes a second copy of the show");
    QTest::qWait(20);
    {
        QList<QQuickItem*> rows;
        collectNamed(playlistList, "collectionPlaylistRow", rows);
        QStringList copies;
        for (auto* row : rows)
            if (row->property("modelData").toMap().value("showId").toString() == songId)
                copies.append(row->property("rowEntryId").toString());
        check(copies.size() >= 2 && copies.first() != copies[1],
              "repeated entries of one show carry distinct stable ids");
        QQuickItem* firstCopy = nullptr;
        for (auto* row : rows)
            if (row->property("rowEntryId").toString() == copies.first()) firstCopy = row;
        if (firstCopy) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              firstCopy->mapToScene(QPointF(20, 15)).toPoint());
            QTest::qWait(20);
        }
        check(left->property("selectedEntryId").toString() == copies.first(),
              "clicking a cue selects that entry");
        rows.clear();
        collectNamed(playlistList, "collectionPlaylistRow", rows);
        int marked = 0;
        for (auto* row : rows)
            if (row->property("rowSelected").toBool()) ++marked;
        check(marked == 1, "exactly one row wears the cue selection");
        // Scrolling a long order: every instantiated row still names its cue.
        bool grew = true;
        for (int i = 0; i < 12; ++i) grew = grew && docs.addShowToPlaylist(songId);
        check(grew, "the running order grows");
        QTest::qWait(30);
        const auto entries = docs.playlistEntries();
        check(playlistList->property("count").toInt() == static_cast<int>(entries.size()),
              "every cue is a row");
        playlistList->setProperty("contentY", 240);
        QTest::qWait(30);
        check(playlistList->property("contentY").toReal() > 100.0, "the long order scrolls");
        rows.clear();
        collectNamed(playlistList, "collectionPlaylistRow", rows);
        bool identities = !rows.isEmpty();
        for (auto* row : rows) {
            const int index = qRound(row->y() / 30.0);
            if (index < 0 || index >= static_cast<int>(entries.size()) ||
                row->property("rowEntryId").toString() !=
                    entries[index].toMap().value("entryId").toString())
                identities = false;
        }
        check(identities, "scrolled rows keep the identities of the cues they show");
        playlistList->setProperty("contentY", 0);
    }
    // Drag a presentation from the Library detail onto a playlist outline
    // row: preparation lands at the end of that order, selection intact.
    QTest::qWait(20);
    refreshOutlineRows();
    check(outlineRows.size() >= 2, "the outline rows rebuild intact");
    if (outlineRows.size() >= 2) {
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          outlineRows.first()->mapToScene(QPointF(20, 14)).toPoint());
    }
    QTest::qWait(20);
    {
        QString welcomeId;
        for (const auto& song : docs.songs())
            if (song.toMap().value("name").toString() == QLatin1String("Welcome"))
                welcomeId = song.toMap().value("id").toString();
        QList<QQuickItem*> rows;
        collectNamed(libraryList, "collectionLibraryRow", rows);
        QQuickItem* welcomeRow = nullptr;
        for (auto* row : rows)
            if (row->property("modelData").toMap().value("id").toString() == welcomeId)
                welcomeRow = row;
        check(welcomeRow != nullptr, "the Welcome row is in the Library detail");
        if (welcomeRow) {
            const auto start = welcomeRow->mapToScene(QPointF(25, 14)).toPoint();
            const auto target = outlineRows[1]->mapToScene(QPointF(25, 14)).toPoint();
            const int before = static_cast<int>(docs.playlistEntries().size());
            // Lead the drag sideways: the detail list flicks vertically, so a
            // vertical-first gesture is stolen as a scroll before the row's
            // drag threshold passes.
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, start + QPoint(60, 0), 15);
            QTest::mouseMove(window, (start + target) / 2 + QPoint(30, 0), 15);
            QTest::mouseMove(window, target + QPoint(20, 0), 15);
            QTest::mouseMove(window, target, 15);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
            QTest::qWait(40);
            const auto after = docs.playlistEntries();
            check(static_cast<int>(after.size()) == before + 1 &&
                      after.last().toMap().value("showId").toString() == welcomeId,
                  "dragging a presentation onto a playlist prepares it at the end");
            check(left->property("outlineKind").toString() == QLatin1String("playlist"),
                  "the drop leaves the target order in the detail view");
            check(docs.currentSongId() == songId,
                  "browsing and preparing never move the presented selection");
        }
    }

    // Ctrl+F searches PRESENTATIONS, and the sidebar owns those now (UI.4a
    // moved the field there when the bottom of the window became the media
    // bin). The operator audit runs with the caret in it, and the caret is
    // what keeps the presenter's keys off: the tab strip that used to claim
    // Ctrl+1/2 for itself is gone, so nothing else has to step aside.
    window->setProperty("page", "show");
    QTest::qWait(30);
    const int cueBeforeSearch = showPageItem->property("cueIndex").toInt();
    QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
    QTest::qWait(40);
    auditOperator(window->contentItem());
    auto* searchField = findNamed(window->contentItem(), "librarySearchField");
    check(searchField != nullptr && searchField->isVisible()
              && searchField->mapToScene(QPointF(0, 0)).x() < left->width(),
          "Ctrl+F opens the presentation search in the sidebar");
    check(window->property("typingIntoField").toBool(),
          "the window knows the search caret owns the keyboard");
    QTest::keyClick(window, Qt::Key_PageDown);
    QTest::qWait(30);
    check(showPageItem->property("cueIndex").toInt() == cueBeforeSearch,
          "typing in the search never steps the presenter's order");
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(20);
    check(!window->property("typingIntoField").toBool(),
          "dismissing the search hands the presenter's keys back");
    QTest::keyClick(window, Qt::Key_PageDown);
    QTest::qWait(30);
    check(showPageItem->property("cueIndex").toInt() == cueBeforeSearch + 1,
          "and the running order walks again");
    showPageItem->setProperty("cueIndex", cueBeforeSearch);
    QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
    check(window->property("page").toString() == "edit",
          "Ctrl+2 is the page's key from anywhere now that no tab strip claims it");
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

    // --- task UI.4a: the bottom of the window is the media bin -----------
    // The bin lists the service's registered media with the kind each name
    // means to the pipeline, filters on it, says what it cannot do, and keeps
    // every activation to itself: browsing, importing and removing a file
    // never put anything near the output. Its own layout is pinned at the
    // largest supported size, where a row has room for name, kind and notice
    // side by side, and checked again at the smallest.
    {
        const QSize binSizeBefore = window->size();
        window->setProperty("page", "edit");
        window->resize(1680, 1020);
        QTest::qWait(60);
        panels.resizeDrawer(300, window->height() - 364);
        QTest::qWait(50);
        const QString files = QString::fromStdString(scratch.path().toStdString());
        check(mediaBin->property("open").toBool(), "the bin opens with a body over its bar");
        check(findNamed(mediaBin, "librarySearchField") == nullptr,
              "the bin carries no second search for presentations");
        check(docs.assets().isEmpty(), "the fixture authors no media of its own");
        check(docs.importMediaFile(files + "/loop.mp4")
                  && docs.importMediaFile(files + "/hymn.mp3")
                  && docs.importMediaFile(files + "/notes.txt"),
              "import registers a video, an audio file and a name nothing presents");
        QTest::qWait(40);
        QMap<QString, QString> kinds;
        for (const QVariant& entry : docs.assets()) {
            const QVariantMap row = entry.toMap();
            kinds.insert(row.value(QStringLiteral("name")).toString(),
                         row.value(QStringLiteral("kind")).toString());
        }
        check(kinds.value("loop.mp4") == QLatin1String("video")
                  && kinds.value("hymn.mp3") == QLatin1String("audio")
                  && kinds.value("notes.txt") == QLatin1String("other"),
              "every row carries the kind its name means to the media pipeline");

        auto collectionRows = [&] {
            QList<QQuickItem*> found;
            collectNamed(mediaBin, "mediaCollectionRow", found);
            return found;
        };
        auto mediaRows = [&] {
            QList<QQuickItem*> found;
            collectNamed(mediaBin, "mediaBinRow", found);
            return found;
        };
        auto clickAt = [window](QQuickItem* item, qreal x, qreal y) {
            if (item) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(x, y)).toPoint());
            }
        };

        QStringList labels;
        QStringList counts;
        for (auto* row : collectionRows()) {
            const QVariantMap data = row->property("modelData").toMap();
            labels << data.value(QStringLiteral("label")).toString();
            counts << data.value(QStringLiteral("count")).toString();
        }
        check(labels == (QStringList{"All media", "Video", "Audio", "Other"})
                  && counts == (QStringList{"3", "1", "1", "1"}),
              "the bin lists every kind its media fills, with a count on each");

        QStringList chips;
        {
            QList<QQuickItem*> found;
            collectNamed(mediaBin, "mediaRowKind", found);
            for (auto* chip : found) chips << chip->property("text").toString();
        }
        QStringList notices;
        bool noticesWhole = true;
        {
            QList<QQuickItem*> found;
            collectNamed(mediaBin, "mediaRowNotice", found);
            for (auto* notice : found) {
                if (notice->isVisible()) notices << notice->property("text").toString();
                noticesWhole = noticesWhole && !notice->property("elided").toBool();
            }
        }
        check(chips == (QStringList{"Video", "Audio", "Other"}),
              "each media row says which kind of file it is");
        check(notices.size() == 3 && notices.at(0).contains("background")
                  && notices.at(1).contains("nothing plays")
                  && notices.at(2).contains("nothing presents") && noticesWhole,
              "a row the product cannot use says why, in full, instead of hiding");

        // A collection filters the list to its kind, and only that.
        for (auto* row : collectionRows())
            if (row->property("modelData").toMap().value("kind").toString() == "audio")
                clickAt(row, 20, 12);
        QTest::qWait(40);
        QList<QQuickItem*> filtered = mediaRows();
        check(mediaBin->property("filter").toString() == QLatin1String("audio")
                  && filtered.size() == 1
                  && filtered.first()->property("modelData").toMap()
                         .value("name").toString() == QLatin1String("hymn.mp3"),
              "picking a collection leaves only that kind in the list");

        // Selecting a row inspects it. Nothing else happens: no go, no play,
        // no move of the editing cursor or the presenter's place.
        const int slideBeforeBin = docs.currentSlide();
        const QString songBeforeBin = docs.currentSongId();
        const int cueBeforeBin = showPageItem->property("cueIndex").toInt();
        clickAt(filtered.first(), 20, 14);
        QTest::qWait(40);
        QList<QQuickItem*> info;
        collectNamed(mediaBin, "mediaInfoKind", info);
        check(!mediaBin->property("selectedAssetId").toString().isEmpty()
                  && !info.isEmpty()
                  && info.first()->property("text").toString().contains("Audio"),
              "a clicked row is described in the bin's info column");
        check(worker.pendingCueKey().isEmpty() && worker.liveCueKey().isEmpty()
                  && !worker.outputLive() && !worker.mediaPlaying(),
              "browsing media never puts anything on the wire");
        check(docs.currentSlide() == slideBeforeBin && docs.currentSongId() == songBeforeBin
                  && showPageItem->property("cueIndex").toInt() == cueBeforeBin,
              "media browsing never moves the editing cursor or the presenter's place");

        // Back to all media: the two document actions, both Edit-only and both
        // honest about the kinds they cover.
        QList<QQuickItem*> allFirst = collectionRows();
        if (!allFirst.isEmpty()) clickAt(allFirst.first(), 20, 12);
        QTest::qWait(40);
        auto* placeButton = findNamed(window->contentItem(), "placeAssetButton");
        auto* removeButton = findNamed(window->contentItem(), "mediaRemoveButton");
        check(placeButton != nullptr && placeButton->isVisible()
                  && removeButton != nullptr && removeButton->isVisible(),
              "the bin's document actions are available while Edit is up");
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("name").toString()
                == QLatin1String("notes.txt"))
                clickAt(row, 20, 14);
        QTest::qWait(30);
        check(!placeButton->property("enabled").toBool(),
              "Add to slide stays off for a file nothing places on a slide");
        check(removeButton->property("enabled").toBool(),
              "Remove follows the selection, not the kind");
        clickAt(removeButton, 20, 12);
        QTest::qWait(40);
        const QVariantList afterRemove = docs.assets();
        QStringList remainingNames;
        for (const QVariant& entry : afterRemove)
            remainingNames << entry.toMap().value("name").toString();
        check(!remainingNames.contains(QLatin1String("notes.txt"))
                  && remainingNames.size() == 2,
              "Remove takes the row out of the media library");

        // A picture is the one kind a bin row can join today, and the button's
        // enablement is that same rule rather than a second copy of it.
        check(docs.importMediaFile(files + "/cross.png"), "a picture is imported");
        QTest::qWait(40);
        QString pictureId;
        for (const QVariant& entry : docs.assets())
            if (entry.toMap().value("name").toString() == QLatin1String("cross.png"))
                pictureId = entry.toMap().value("id").toString();
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("id").toString() == pictureId)
                clickAt(row, 20, 14);
        QTest::qWait(40);
        const int boxesBefore = docs.currentSlideBoxes().size();
        check(placeButton->property("enabled").toBool(),
              "the picture makes Add to slide live");
        clickAt(placeButton, 20, 12);
        QTest::qWait(40);
        const QVariantList boxes = docs.currentSlideBoxes();
        check(boxes.size() == boxesBefore + 1
                  && boxes.last().toMap().value("kind").toString() == "image"
                  && boxes.last().toMap().value("assetId").toString() == pictureId,
              "Add to slide places the picture the operator had selected");
        check(worker.pendingCueKey().isEmpty() && !worker.outputLive(),
              "and placing it still presents nothing");

        // Closing the bin and dragging its edge give the workspace its area
        // back without disturbing what the operator was doing.
        docs.selectSong(songBeforeBin);
        docs.selectSlide(0);
        window->setProperty("page", "show");
        QTest::qWait(80);
        const qreal openTop = mediaBin->mapToScene(QPointF(0, 0)).y();
        panels.toggleDrawer();
        QTest::qWait(60);
        check(!mediaBin->property("open").toBool()
                  && mediaBin->mapToScene(QPointF(0, 0)).y() > openTop,
              "the bar collapses the bin and stays where the operator can reach it");
        check(preview->isVisible() && grid->width() >= 190 && grid->height() > 100,
              "a collapsed bin leaves the preview and the cue grid usable");
        const auto grip = mediaBin->mapToScene(QPointF(4, 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, grip);
        QTest::mouseMove(window, grip - QPoint(0, 90), 20);
        QTest::mouseMove(window, grip - QPoint(0, 160), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, grip - QPoint(0, 160));
        QTest::qWait(60);
        check(mediaBin->property("open").toBool(),
              "dragging the top edge brings the media back");
        check(docs.currentSongId() == songBeforeBin && docs.currentSlide() == 0,
              "opening and sizing the bin never moves the editing cursor");

        // And at the smallest supported size: the notice steps aside so the
        // name stays whole, rather than a long kind label eating the name. The
        // info column still explains whichever row the operator picks.
        window->resize(1000, 680);
        QTest::qWait(80);
        {
            QList<QQuickItem*> noticeItems;
            collectNamed(mediaBin, "mediaRowNotice", noticeItems);
            QList<QQuickItem*> names;
            collectNamed(mediaBin, "mediaRowName", names);
            bool noticesAside = !noticeItems.isEmpty();
            for (auto* notice : noticeItems)
                noticesAside = noticesAside && !notice->isVisible();
            bool namesWhole = names.size() == 3;
            for (auto* name : names)
                namesWhole = namesWhole && !name->property("elided").toBool();
            check(noticesAside && namesWhole,
                  "a narrow bin drops the notice and keeps whole file names");
        }
        window->resize(binSizeBefore);
        QTest::qWait(60);
        panels.resizeDrawer(300, window->height() - 364);
        QTest::qWait(40);
    }

    window->setProperty("page", "show");
    QTest::qWait(80);
    panels.resizeDrawer(300, window->height() - 364);
    QTest::qWait(50);
    {
        // UI.3c: the thumbnail size control and the bounded band schedule it
        // re-triggers. To pin the schedule off the wire, deepen the running
        // order far past the ask envelope: one more Welcome beside its two
        // earlier copies (dedupe material) and a forty-slide presentation
        // listed twice (unique ids to push the band into the 32-scene cap).
        auto* bigger = findNamed(window->contentItem(), "operatorThumbLarger");
        auto* smaller = findNamed(window->contentItem(), "operatorThumbSmaller");
        check(bigger != nullptr && smaller != nullptr,
              "the Show heading carries the thumbnail size control");
        check(settings.showThumbLevel() == 2, "the size control starts at the middle step");
        const int rowsBefore = showPageItem->property("cueRows").toList().size();
        bool grew = docs.createSong("Festival");
        for (int i = 0; i < 40 && grew; ++i) grew = docs.addSlide();
        check(grew, "the band fixture authors a forty-slide presentation");
        const QString festivalId = docs.currentSongId();
        QString welcomeId;
        for (const auto& song : docs.songs())
            if (song.toMap().value("name").toString() == QLatin1String("Welcome"))
                welcomeId = song.toMap().value("id").toString();
        check(docs.addShowToPlaylist(welcomeId) && docs.addShowToPlaylist(festivalId)
                  && docs.addShowToPlaylist(festivalId),
              "the order takes another Welcome and two forty-slide entries");
        docs.selectSong(songId);
        docs.selectSlide(0);
        QTest::qWait(60);
        int deepRows = 0;
        for (const auto& entry : docs.playlistEntries())
            deepRows += docs.slidesOf(entry.toMap().value("showId").toString()).size();
        const QVariantList rows = showPageItem->property("cueRows").toList();
        check(rows.size() == deepRows && deepRows >= 32 * 6,
              "the running order now runs far past any ask envelope");

        // Growing the cells: three clicks in a burst walk to the largest
        // step (the third lands on the now-disabled button), the step is
        // remembered, and the asks coalesce instead of chasing every click.
        const qreal cellBefore = cueGrid->property("cellHeight").toReal();
        const QSize sizeBefore = window->size();
        const int asksBefore = showPageItem->property("bandRequests").toInt();
        const QPoint bump = bigger->mapToScene(QPointF(10, 12)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, bump);
        QTest::qWait(6);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, bump);
        QTest::qWait(6);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, bump);
        QTest::qWait(160);
        check(settings.showThumbLevel() == 4, "the size control stops at the largest step");
        check(!bigger->property("enabled").toBool(), "the largest step disables its own button");
        check(cueGrid->property("cellHeight").toReal() > cellBefore,
              "larger thumbnails really grow the cells");
        const int zoomAsks = showPageItem->property("bandRequests").toInt() - asksBefore;
        check(zoomAsks >= 1 && zoomAsks <= 2, "the zoom burst is one coalesced band ask");
        {
            const QSettings persisted(QString::fromStdString((base / "app.ini").string()),
                                      QSettings::IniFormat);
            check(persisted.value(QStringLiteral("showThumbLevel")).toInt() == 4,
                  "the size step is written to the settings file");
        }
        // The page's own two scheduling functions are what's under test, and
        // a QML function's answer comes back through QVariant - so the suite
        // calls them rather than reimplementing the arithmetic here.
        auto pageCall = [showPageItem](const char* name) {
            QVariant answer;
            QMetaObject::invokeMethod(showPageItem, name, Q_RETURN_ARG(QVariant, answer));
            return answer;
        };
        // The ask size tracks the tile's picture and stays inside the
        // envelope the wire accepts at the stream size this app starts.
        const QVariantMap px = pageCall("bandPixels").toMap();
        int askW = px.value(QStringLiteral("w")).toInt();
        const int askH = px.value(QStringLiteral("h")).toInt();
        check(askW >= 160 && askH >= 90
                  && askW == qBound(160, qRound(cueGrid->property("cellWidth").toReal() - 18.0),
                                    512)
                  && askH == qRound(askW * 9.0 / 16.0),
              "the ask follows the picture width and keeps 16:9");
        check(askW >= 32 && askW <= 512 && askH >= 32 && askH <= 512
                  && openshow::protocol::thumbnailExtentFits(
                         static_cast<std::uint32_t>(askW), static_cast<std::uint32_t>(askH),
                         openshow::protocol::previewSlotStrideBytes(640, 360)),
              "every size the control picks fits the preview slot this app streams at");

        // Shrinking to the smallest step: the ask clamps at its floor rather
        // than asking the worker for postage stamps.
        window->resize(1680, 1150);
        QTest::qWait(120);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          smaller->mapToScene(QPointF(10, 12)).toPoint());
        QTest::qWait(6);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          smaller->mapToScene(QPointF(10, 12)).toPoint());
        QTest::qWait(6);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          smaller->mapToScene(QPointF(10, 12)).toPoint());
        QTest::qWait(6);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          smaller->mapToScene(QPointF(10, 12)).toPoint());
        QTest::qWait(160);
        check(settings.showThumbLevel() == 0 && !smaller->property("enabled").toBool(),
              "the smallest step disables its own button");
        askW = pageCall("bandPixels").toMap().value("w").toInt();
        check(askW == 160, "the smallest cell still asks the 160-pixel floor");

        // Extent, cap and dedupe at this size. First prove the viewport
        // alone would overrun the cap without the clamp.
        const int cols = cueGrid->property("columns").toInt();
        const qreal cellH = cueGrid->property("cellHeight").toReal();
        const int viewportRows = (qCeil(cueGrid->property("height").toReal() / cellH) + 1) * cols;
        check(viewportRows > 32, "the tall viewport would overrun the cap without the clamp");
        int festivalStart = 0;
        while (festivalStart < rows.size()
               && rows[festivalStart].toMap().value("showId").toString() != festivalId)
            ++festivalStart;
        const int bandStart = qCeil((festivalStart + 1.0) / cols) * cols;
        cueGrid->setProperty("contentY", (bandStart / cols) * cellH);
        QTest::qWait(30);
        QStringList bandKeys;
        for (const auto& entry : pageCall("slideBand").toList())
            bandKeys << entry.toMap().value(QStringLiteral("key")).toString();
        check(bandKeys.size() == 32 && bandKeys.size() == QSet(bandKeys.cbegin(),
                                                               bandKeys.cend()).size()
                  && bandKeys.first() == rows[bandStart].toMap().value("slideId").toString()
                  && bandKeys.last() == rows[bandStart + 31].toMap().value("slideId").toString(),
              "inside unique slides the band hits the 32-scene cap and stops there");
        // Back at the top the window spans the show's cycle twice, so the
        // band dedupes to the distinct pictures: eighteen Good Grace slides
        // and one Welcome, no key asked twice.
        cueGrid->setProperty("contentY", 0);
        QTest::qWait(30);
        bandKeys.clear();
        for (const auto& entry : pageCall("slideBand").toList())
            bandKeys << entry.toMap().value(QStringLiteral("key")).toString();
        int welcomeAsks = 0;
        for (const auto& key : bandKeys)
            if (key == rows[18].toMap().value("slideId").toString()) ++welcomeAsks;
        check(bandKeys.size() == 19 && welcomeAsks == 1
                  && bandKeys.size() == QSet(bandKeys.cbegin(), bandKeys.cend()).size()
                  && bandKeys.first() == rows[0].toMap().value("slideId").toString(),
              "a band across repeated entries asks once per distinct picture");

        // Coalescing with the destination kept: five jumps inside the timer
        // window ask for nothing at all, and the one ask that follows
        // describes the END of the burst — with the cap anchored off the
        // bottom edge, the last cue is in it.
        const int asksMid = showPageItem->property("bandRequests").toInt();
        const qreal maxY = cueGrid->property("contentHeight").toReal()
                         - cueGrid->property("height").toReal();
        for (int i = 1; i <= 5; ++i) {
            cueGrid->setProperty("contentY", maxY * i / 5.0);
            QTest::qWait(6);
        }
        check(showPageItem->property("bandRequests").toInt() == asksMid,
              "five jumps inside the timer window still ask for nothing");
        QTest::qWait(200);
        check(showPageItem->property("bandRequests").toInt() - asksMid == 1,
              "the burst coalesces into exactly one ask");
        bandKeys.clear();
        for (const auto& entry : pageCall("slideBand").toList())
            bandKeys << entry.toMap().value(QStringLiteral("key")).toString();
        check(!bandKeys.isEmpty()
                  && bandKeys.size() <= 32
                  && bandKeys.contains(rows.last().toMap().value("slideId").toString()),
              "the coalesced ask covers the scroll destination, not the burst start");

        // The size buttons are Qt.NoFocus: the presenter's keys still walk
        // the order after clicking them, and nothing above moved the cursor.
        // The Edit section ran before this one, so it is also the pin for the
        // mode switch handing the keyboard back: a hidden page's text field
        // must not go on owning the presenter's keystrokes.
        check(!window->property("typingIntoField").toBool()
                  && !window->property("squattedFocus").toBool(),
              "leaving Edit releases the lyric box and the gates are live");
        showPageItem->setProperty("cueIndex", 17);
        QTest::qWait(30);
        QTest::keyClick(window, Qt::Key_PageDown);
        QTest::qWait(30);
        check(showPageItem->property("cueIndex").toInt() == 18,
              "the size buttons leave the presenter keys free");
        check(docs.currentSongId() == songId && docs.currentSlide() == 0,
              "sizing and scrolling never move the editing cursor");

        // Hand the later sections their fixture back: the entries added here
        // removed, the default step, the window it arrived with, the grid
        // scrolled to the top so the live-mode check's slides are in the
        // first band.
        const QVariantList finalEntries = docs.playlistEntries();
        QStringList doomed;
        QString extraWelcome;
        for (const auto& entry : finalEntries) {
            const QVariantMap row = entry.toMap();
            const QString show = row.value(QStringLiteral("showId")).toString();
            if (show == festivalId)
                doomed << row.value(QStringLiteral("entryId")).toString();
            else if (show == welcomeId)
                extraWelcome = row.value(QStringLiteral("entryId")).toString();
        }
        doomed << extraWelcome;  // the last Welcome entry is the one added here
        bool shed = true;
        for (const auto& entryId : doomed) shed = docs.removePlaylistEntry(entryId) && shed;
        settings.setShowThumbLevel(2);
        window->resize(sizeBefore);
        QTest::qWait(120);
        cueGrid->setProperty("contentY", 0);
        QTest::qWait(140);
        const int rowsAfter = showPageItem->property("cueRows").toList().size();
        const qreal cellAfter = cueGrid->property("cellHeight").toReal();
        check(shed && rowsAfter == rowsBefore && qFuzzyCompare(cellAfter, cellBefore),
              "the fixture shrinks back to what the later sections expect");
    }
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
