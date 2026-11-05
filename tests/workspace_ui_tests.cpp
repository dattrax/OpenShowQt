// Interaction regressions for the operator workspace. No worker or user files.
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QJsonDocument>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>

#include "AppSettings.h"
#include "BibleImport.h"
#include "DocumentController.h"
#include "ScriptureImport.h"
#include "WorkerController.h"
#include "WorkspacePanels.h"
#include "openshow/protocol/preview_wire.h"

#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
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
    // Milestone BI: the importer's sessions ride the same shell the
    // operator sees; the workspace test proves the page loads and behaves
    // off air with no worker and no network at all.
    BibleImport bible;
    openshow::ui::ScriptureImport scripture(&bible, &docs, &worker);
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
    context->setContextProperty("bible", &bible);
    context->setContextProperty("scripture", &scripture);
    context->setContextProperty("uiSize", QSize(1380, 800));
    engine.loadFromModule("OpenShow", "Main");
    if (engine.rootObjects().isEmpty()) {
        // A page that fails to compile leaves no window and, without this,
        // no clue either: the collected engine warnings are the whole story.
        for (const auto& warning : warnings) {
            std::fprintf(stderr, "load QML: %s\n", qPrintable(warning));
        }
        return 1;
    }
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
    // CS.4: the ONE read model the shared rails will wear is instantiable
    // from a QML file in the same OpenShow namespace the app resolves, and
    // answers through QML in the canonical shape.
    {
        QQmlComponent inspectorType(&engine);
        inspectorType.setData(
            "import QtQuick\nimport OpenShow\n"
            "InspectorAdapter { document: docs; target: ({kind: \"service\"}) }\n",
            QUrl());
        check(!inspectorType.isError(), "QML can name the InspectorAdapter type");
        QObject* rail = inspectorType.isError() ? nullptr : inspectorType.create(nullptr);
        QVariantMap state;
        const bool asked =
            rail != nullptr &&
            QMetaObject::invokeMethod(rail, "fieldState", Q_RETURN_ARG(QVariantMap, state),
                                      Q_ARG(QString, QStringLiteral("transition")));
        check(asked && state.value(QStringLiteral("ok")).toBool() &&
                  state.value(QStringLiteral("source")).toString() ==
                      QStringLiteral("the service"),
              "the read model answers through QML in the canonical shape");
        delete rail;
    }
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
    // CS.2: target isolation is asserted on the PIXEL PAINTER inside each
    // pane (the one PreviewView item the surface wraps), not on the wrapper —
    // a pane may describe itself however it likes, the painter's target and
    // key are the wire truth it will paint from.
    std::function<QQuickItem*(QQuickItem*, const char*)> findByClass =
        [&](QQuickItem* item, const char* cls) -> QQuickItem* {
            if (QString::fromLatin1(item->metaObject()->className())
                        .endsWith(QLatin1String(cls)))
                return item;
            for (auto* child : item->childItems())
                if (auto* hit = findByClass(child, cls)) return hit;
            return nullptr;
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
            // CS.2: what the cue tile wears. The card captions its own row;
            // the surface underneath is bound to the keyed Thumbnail target
            // with THIS row's identity, and the pixel painter it wraps reads
            // exactly that target — never the audience frame, whatever the
            // rail is painting.
            const QVariantMap firstRow = rows[0].toMap();
            auto* tileSurface = firstTile->property("surface").value<QQuickItem*>();
            check(firstTile->property("caption").toString()
                      .contains(QStringLiteral("1  ")
                                + firstRow.value(QStringLiteral("preview")).toString()),
                  "the cue card wears the caption of the row it stands for");
            check(tileSurface
                      && tileSurface->property("target").toString()
                             == QLatin1String("thumbnail")
                      && tileSurface->property("key").toString()
                             == firstRow.value(QStringLiteral("slideId")).toString(),
                  "the cue card's surface names the thumbnail target and its own key");
            auto* tilePainter = tileSurface ? findByClass(tileSurface, "PreviewView")
                                            : nullptr;
            check(tilePainter && tilePainter->property("target").toInt() == 2,
                  "the painter under the cue card reads keyed thumbnails, never the audience");
            const QRectF tileRect = tileSurface ? tileSurface->property("contentRect").toRectF()
                                                : QRectF();
            check(qAbs(tileRect.width() * 9 - tileRect.height() * 16) < 1.0
                      && tilePainter
                      && qAbs(tilePainter->width() - tileRect.width()) < 1.0,
                  "the card surface letterboxes 16:9 and its painter rides the fitted rect");
            // With no worker on the wire NOTHING is `ready`: the flag follows
            // the pixels this one item painted, so a tile can never report a
            // frame it does not hold and swap its placeholder out.
            check(tileSurface && !tileSurface->property("ready").toBool()
                      && tilePainter && !tilePainter->property("hasFrame").toBool(),
                  "an unanswered thumbnail ask leaves the surface honestly unready");
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

    // --- task UI.4a and UI.4b: the bottom of the window is the media bin --
    // The bin lists the service's registered media with the kind each name
    // means to the pipeline, filters on it, says what it cannot do, and keeps
    // activation to one explicit control: browsing, importing and removing a
    // file never put anything near the output. Its own layout is pinned at the
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
        // The files behind these names are written for real (bytes nobody
        // decodes: this fixture has no worker, and the bin never claims a file
        // plays). What a row's `exists` says is a fact about the disk, so the
        // rows here are the ordinary kind — and a fourth name is registered
        // with nothing on disk behind it, which is the row that must speak up.
        for (const char* name : {"loop.mp4", "hymn.mp3", "notes.txt"})
            std::ofstream(std::filesystem::path(scratch.path().toStdString()) / name) << "BYTES";
        check(docs.importMediaFile(files + "/loop.mp4")
                  && docs.importMediaFile(files + "/hymn.mp3")
                  && docs.importMediaFile(files + "/notes.txt"),
              "import registers a video, an audio file and a name nothing presents");
        QTest::qWait(40);
        QMap<QString, QString> kinds;
        QStringList present;
        for (const QVariant& entry : docs.assets()) {
            const QVariantMap row = entry.toMap();
            kinds.insert(row.value(QStringLiteral("name")).toString(),
                         row.value(QStringLiteral("kind")).toString());
            present << row.value(QStringLiteral("name")).toString()
                    + (row.value(QStringLiteral("exists")).toBool() ? "=there" : "=gone");
        }
        check(present == (QStringList{"loop.mp4=there", "hymn.mp3=there", "notes.txt=there"}),
              "a row says whether the file it names is on disk");
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
        int noticeRows = 0;
        bool noticesWhole = true;
        {
            QList<QQuickItem*> found;
            collectNamed(mediaBin, "mediaRowNotice", found);
            noticeRows = found.size();
            for (auto* notice : found) {
                if (notice->isVisible()) notices << notice->property("text").toString();
                noticesWhole = noticesWhole && !notice->property("elided").toBool();
            }
        }
        check(chips == (QStringList{"Video", "Audio", "Other"}),
              "each media row says which kind of file it is");
        // The two rows nothing here can fire say so, in full; the audio row
        // says nothing, because UI.4b gave it a control instead of a notice.
        check(noticeRows == 3 && notices.size() == 2 && notices.at(0).contains("background")
                  && notices.at(1).contains("nothing presents") && noticesWhole,
              "a row the product cannot use says why, in full, instead of hiding");

        // The table those notices come out of, driven directly. An offscreen
        // fixture with no worker cannot make a track live or a probe refuse,
        // so the rows' states are invented here rather than staged; the live
        // frontend journey proves the worker really answers these two.
        auto notice = [mediaBin](const char* kind, bool present, bool live, bool refused) {
            QVariant answer;
            QMetaObject::invokeMethod(mediaBin, "noticeForState",
                                      Q_RETURN_ARG(QVariant, answer),
                                      Q_ARG(QVariant, QString::fromUtf8(kind)),
                                      Q_ARG(QVariant, present), Q_ARG(QVariant, live),
                                      Q_ARG(QVariant, refused));
            return answer.toString();
        };
        check(notice("audio", true, true, false) == QLatin1String("on air")
                  && notice("video", true, true, false) == QLatin1String("on air"),
              "the file the worker says is playing is marked before anything else");
        check(notice("audio", false, false, false) == QLatin1String("file is not here")
                  && notice("video", false, false, false) == QLatin1String("file is not here")
                  && notice("picture", false, false, false) == QLatin1String("file is not here"),
              "a kind the product can use says so when its file has gone");
        check(notice("other", false, false, false)
                  == QLatin1String("nothing presents this file type"),
              "an unusable type keeps its reason even when its file is missing");
        check(notice("video", true, false, false).startsWith(QLatin1String("background only"))
                  && notice("picture", true, false, false).isEmpty(),
              "the standing kind rule: a video's route is Edit, a picture has none to explain");
        check(notice("audio", true, false, true) == QLatin1String("cannot play")
                  && notice("audio", true, false, false).isEmpty(),
              "a refused probe marks the row, and an audio cue nobody has tried says nothing");

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

        // UI.4b: the audio row has a cue control, and with no output to
        // receive it the control is honest rather than hopeful. Clicking a
        // row selected it; nothing else has happened, and nothing here can
        // pretend a track is up — that answer only ever comes from a worker.
        auto* activate = findNamed(window->contentItem(), "mediaActivateButton");
        auto* statusLine = findNamed(mediaBin, "mediaInfoStatus");
        check(activate != nullptr && activate->isVisible()
                  && !activate->property("enabled").toBool(),
              "an audio cue offers Play, and no connected output keeps it off");
        check(statusLine != nullptr
                  && statusLine->property("text").toString().contains("no output connected"),
              "the info column names what stands between this row and sound");
        check(worker.standalonePath().isEmpty() && !worker.standaloneOnAir()
                  && worker.standaloneRefusal().isEmpty()
                  && worker.standaloneRefusalPath().isEmpty(),
              "and a bin with no output has said nothing about any track");
        clickAt(activate, 20, 12);
        QTest::qWait(30);
        check(!worker.standaloneOnAir() && worker.standalonePath().isEmpty(),
              "a disabled cue sends nothing");

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

        // The kinds with no runtime to fire are offered no button at all: a
        // greyed-out Play would read as something wrong with the output, where
        // the truth is that this file has no cue to fire — which is what the
        // notice column and the info line say instead.
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("name").toString()
                == QLatin1String("loop.mp4"))
                clickAt(row, 20, 14);
        QTest::qWait(30);
        check(!activate->isVisible()
                  && statusLine->property("text").toString().contains("background"),
              "a video row is not offered a cue, and is told where it does go");

        // A name whose file the service has lost (a moved or renamed file is
        // how this arrives) keeps its row and says so, and names itself in the
        // info column rather than the path — Show identifies by name.
        check(docs.importMediaFile(files + "/ghost.mp3"),
              "an audio name with no file behind it is registered like any other");
        QTest::qWait(40);
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("name").toString()
                == QLatin1String("ghost.mp3"))
                clickAt(row, 20, 14);
        QTest::qWait(40);
        {
            QList<QQuickItem*> missingNotices;
            collectNamed(mediaBin, "mediaRowNotice", missingNotices);
            QString ghostNotice;
            for (auto* item : missingNotices)
                if (item->isVisible()
                    && item->property("text").toString() == QLatin1String("file is not here"))
                    ghostNotice = item->property("text").toString();
            check(!ghostNotice.isEmpty()
                      && statusLine->property("text").toString()
                             .contains("this file is not where the service says"),
                  "a row whose file has gone says so in both columns");
        }
        check(activate->isVisible(),
              "and its Play is not disabled by a snapshot: the worker's probe decides");
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
                  && remainingNames.size() == 3,
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
            bool namesWhole = names.size() == 4;
            for (auto* name : names)
                namesWhole = namesWhole && !name->property("elided").toBool();
            check(noticesAside && namesWhole,
                  "a narrow bin drops the notice and keeps whole file names");
        }
        window->resize(binSizeBefore);
        QTest::qWait(60);
        panels.resizeDrawer(300, window->height() - 364);
        QTest::qWait(40);
        // Leave the bin on the row UI.4b is about: the audio cue, its control
        // visible and still saying there is nothing to send it to. This is also
        // the state the artefact captures below are taken in.
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("name").toString()
                == QLatin1String("hymn.mp3"))
                clickAt(row, 20, 14);
        QTest::qWait(40);
        check(activate->isVisible() && !activate->property("enabled").toBool()
                  && !worker.standaloneOnAir(),
              "the last word is the cue control, honest about having no output");
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
        // CS.7: the window is placed by the collection's MAPPED geometry,
        // not by row arithmetic over a flat cell model. The second Festival
        // occurrence is a run of 40 distinct slides starting at column zero;
        // from its first cue the capped window is exactly the first 32 of
        // that run. (When the grid cannot scroll the group's top row all the
        // way up, the bottom-edge anchor is the clause under test instead,
        // and the last cue must be inside — the burst check below pins it.)
        int festivalTwice = -1;
        int festivalRuns = 0;
        QString lastEntryId;
        for (int i = 0; i < rows.size(); ++i) {
            const QVariantMap row = rows[i].toMap();
            const QString entryId = row.value(QStringLiteral("entryId")).toString();
            if (entryId == lastEntryId) continue;
            lastEntryId = entryId;
            if (row.value(QStringLiteral("showId")).toString() != festivalId) continue;
            ++festivalRuns;
            if (festivalRuns == 2) { festivalTwice = i; break; }
        }
        check(festivalTwice > 0, "the fixture lists Festival twice in the order");
        QVariant rectV;
        QMetaObject::invokeMethod(cueGrid, "cueRect", Q_RETURN_ARG(QVariant, rectV),
                                  Q_ARG(QVariant, festivalTwice));
        const qreal groupTop = rectV.toMap().value(QStringLiteral("y")).toReal();
        const qreal scrollMax = cueGrid->property("contentHeight").toReal()
                              - cueGrid->property("height").toReal();
        cueGrid->setProperty("contentY", qMin(groupTop, scrollMax));
        QTest::qWait(30);
        QStringList bandKeys;
        for (const auto& entry : pageCall("slideBand").toList())
            bandKeys << entry.toMap().value(QStringLiteral("key")).toString();
        check(bandKeys.size() == 32 && bandKeys.size() == QSet(bandKeys.cbegin(),
                                                               bandKeys.cend()).size()
                  && (groupTop > scrollMax
                      ? bandKeys.contains(rows.last().toMap().value("slideId").toString())
                      : (bandKeys.first()
                             == rows[festivalTwice].toMap().value("slideId").toString()
                         && bandKeys.last()
                             == rows[festivalTwice + 31].toMap()
                                    .value("slideId").toString())),
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
        QVariant visibleV;
        QMetaObject::invokeMethod(cueGrid, "visibleCueIndexes", Q_RETURN_ARG(QVariant, visibleV));
        QStringList expectedTop;
        for (const auto& value : visibleV.toList()) {
            const auto key = rows[value.toInt()].toMap().value("slideId").toString();
            if (!expectedTop.contains(key)) expectedTop.append(key);
        }
        if (expectedTop.size() > 32) expectedTop = expectedTop.mid(0, 32);
        check(bandKeys == expectedTop && welcomeAsks == 1
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
    // --- task CS.7: previews grouped by running-order ENTRY ---------------
    // Row boundaries are OCCURRENCE boundaries: two copies of one show are
    // two groups, every group starts at column zero, group padding is inert,
    // and the flat cue order (keyboard, live identity, clicking) is exactly
    // what it was. A fresh playlist keeps the fixture deterministic; the
    // block hands the service back at the end.
    {
        const QString homePlaylist = docs.currentPlaylistId();
        const int homeRows = showPageItem->property("cueRows").toList().size();
        const QSize cs7Size = window->size();
        window->resize(1380, 800);
        settings.setShowThumbLevel(4);   // the 300-pixel card: four columns here
        QTest::qWait(140);
        auto makeCs7Song = [&docs](const QString& name, int slides) {
            docs.createSong(name);
            for (int i = 0; i < slides; ++i) docs.addSlide();
            docs.flushText();
            return docs.currentSongId();
        };
        const QString alphaId = makeCs7Song(QStringLiteral("CS7 Alpha"), 3);
        const QString betaId = makeCs7Song(QStringLiteral("CS7 Beta"), 3);
        const QString emptyId = makeCs7Song(QStringLiteral("CS7 Empty"), 0);
        const QString deltaId = makeCs7Song(QStringLiteral("CS7 Delta"), 9);
        check(docs.createPlaylist(QStringLiteral("CS.7 Order")), "the CS.7 order playlist exists");
        QString orderPlaylist;
        for (const auto& playlist : docs.playlists()) {
            const QVariantMap row = playlist.toMap();
            if (row.value(QStringLiteral("name")).toString() == QLatin1String("CS.7 Order"))
                orderPlaylist = row.value(QStringLiteral("id")).toString();
        }
        check(!orderPlaylist.isEmpty(), "the CS.7 order is findable to select");
        docs.selectPlaylist(orderPlaylist);
        check(docs.addShowToPlaylist(alphaId) && docs.addShowToPlaylist(betaId)
                  && docs.addShowToPlaylist(alphaId) && docs.addShowToPlaylist(deltaId)
                  && docs.addShowToPlaylist(emptyId),
              "the order takes Alpha, Beta, Alpha again, Delta and an empty show");
        QTest::qWait(160);
        const QVariantList order = showPageItem->property("cueRows").toList();
        check(order.size() == 18, "the empty show adds no phantom cue to the grid");
        const int cs7cols = cueGrid->property("columns").toInt();
        check(cs7cols >= 2, "the order lays out over multiple columns at this size");
        const auto rowsFor = [cs7cols](int cues) { return (cues + cs7cols - 1) / cs7cols; };
        const QVariantMap layout = cueGrid->property("layout").toMap();
        const QVariantList cells = layout.value(QStringLiteral("cells")).toList();
        const QList<int> groupCues{3, 3, 3, 9};
        // Cells = every cue, plus each NON-FINAL group's tail fill up to its
        // row end. The last group's trailing columns stay simply unused.
        int expectedCells = 18;
        for (int g = 0; g < groupCues.size() - 1; ++g)
            expectedCells += (cs7cols - groupCues[g] % cs7cols) % cs7cols;
        int realCues = 0;
        int firstFiller = -1;
        for (int i = 0; i < cells.size(); ++i) {
            if (cells[i].toMap().contains(QStringLiteral("cueIndex"))) ++realCues;
            else if (firstFiller < 0) firstFiller = i;
        }
        check(cells.size() == expectedCells
                  && realCues == 18 && expectedCells > 18,
              "every group pads its row out and the fill is real, not cues");
        const QVariantList cellOfCue = layout.value(QStringLiteral("cellOfCue")).toList();
        const int alphaEnd = cellOfCue.at(2).toInt();
        check(alphaEnd % cs7cols != cs7cols - 1
                  ? cells.at(alphaEnd + 1).toMap().value(QStringLiteral("filler")).toBool()
                  : true,
              "Alpha ends mid-row and the rest of that row is inert fill");
        auto cueRect = [cueGrid](int cue) {
            QVariant answer;
            QMetaObject::invokeMethod(cueGrid, "cueRect", Q_RETURN_ARG(QVariant, answer),
                                      Q_ARG(QVariant, cue));
            return answer.toMap();
        };
        auto cueRow = [cueGrid](int cue) {
            QVariant answer;
            QMetaObject::invokeMethod(cueGrid, "cueRow", Q_RETURN_ARG(QVariant, answer),
                                      Q_ARG(QVariant, cue));
            return answer.toInt();
        };
        const qreal cs7cellH = cueGrid->property("cellHeight").toReal();
        const qreal cs7cellW = cueGrid->property("cellWidth").toReal();
        cueGrid->setProperty("contentY", 0);
        QTest::qWait(60);
        const int betaRow = rowsFor(3);
        const int alphaAgainRow = betaRow + rowsFor(3);
        const int deltaRow = alphaAgainRow + rowsFor(3);
        check(cueRect(0).value(QStringLiteral("x")).toReal() == 0.0
                  && cueRow(3) == betaRow && cueRow(6) == alphaAgainRow
                  && cueRow(9) == deltaRow,
              "Beta, the repeated Alpha and Delta each open a fresh row at column zero");
        check(qAbs(cueRect(6).value(QStringLiteral("x")).toReal()) < 1.0,
              "the second Alpha really starts at column zero, not after a gap");
        check(cueRow(17) == deltaRow + rowsFor(9) - 1
                  && qAbs(cueRect(17).value(QStringLiteral("y")).toReal()
                          - (deltaRow + rowsFor(9) - 1) * cs7cellH) < 1.0,
              "the order's last cue sits on the last row where the plan put it");
        // Group heads sit where their mapped rect says they sit — the tile
        // and the geometry report ONE position.
        QList<QQuickItem*> cs7Tiles;
        collectNamed(cueGrid, "operatorCueTile", cs7Tiles);
        QQuickItem* betaTile = nullptr;
        for (auto* tile : cs7Tiles)
            if (tile->isVisible() && tile->property("index").toInt() == 3) betaTile = tile;
        check(betaTile != nullptr, "the Beta head tile is on screen");
        if (betaTile != nullptr) {
            const QPointF inGrid = betaTile->mapToItem(cueGrid, QPointF(0, 0));
            check(qAbs(inGrid.x() - cueRect(3).value(QStringLiteral("x")).toReal()) < 1.0
                      && qAbs(inGrid.y() + cueGrid->property("contentY").toReal()
                              - cueRect(3).value(QStringLiteral("y")).toReal()) < 1.0,
                  "the head tile and cueRect agree on where Beta starts");
        }
        // The padding is inert: clicking the fill beside Alpha costs nothing.
        showPageItem->setProperty("cueIndex", -1);
        check(firstFiller >= 0, "the fixture leaves some padding to test");
        const QPointF fillPoint = cueGrid->mapToScene(
            QPointF((firstFiller % cs7cols) * cs7cellW + cs7cellW / 2.0,
                    (firstFiller / cs7cols) * cs7cellH + cs7cellH / 2.0));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, fillPoint.toPoint());
        QTest::qWait(40);
        check(showPageItem->property("cueIndex").toInt() == -1,
              "a click on group padding selects and presents nothing");
        // Repeated occurrences stay separate: same slide, different cue.
        const QString firstAlphaKey = order[0].toMap().value(QStringLiteral("cueKey")).toString();
        const QString againAlphaKey = order[6].toMap().value(QStringLiteral("cueKey")).toString();
        check(order[0].toMap().value(QStringLiteral("slideId")).toString()
                      == order[6].toMap().value(QStringLiteral("slideId")).toString()
                  && firstAlphaKey != againAlphaKey
                  && order[0].toMap().value(QStringLiteral("entryId")).toString()
                         != order[6].toMap().value(QStringLiteral("entryId")).toString(),
              "the two Alphas share pixels but are two cues on two entries");
        // Flat cue order survives the grouping: the keyboard crosses the
        // Alpha/Beta boundary in one step, and a wrapped Delta cue clicks
        // through to its own cue index.
        showPageItem->setProperty("cueIndex", 2);
        QTest::qWait(30);
        QTest::keyClick(window, Qt::Key_PageDown);
        QTest::qWait(40);
        check(showPageItem->property("cueIndex").toInt() == 3
                  && order[3].toMap().value(QStringLiteral("showName")).toString()
                         == QLatin1String("CS7 Beta"),
              "Next walks straight from Alpha into Beta across the row break");
        QQuickItem* deltaTile = nullptr;
        for (auto* tile : cs7Tiles)
            if (tile->isVisible() && tile->property("index").toInt() == 13) deltaTile = tile;
        if (deltaTile == nullptr) {
            cueGrid->setProperty("contentY",
                                 cueRect(13).value(QStringLiteral("y")).toReal());
            QTest::qWait(80);
            cs7Tiles.clear();
            collectNamed(cueGrid, "operatorCueTile", cs7Tiles);
            for (auto* tile : cs7Tiles)
                if (tile->isVisible() && tile->property("index").toInt() == 13) deltaTile = tile;
        }
        check(deltaTile != nullptr, "a wrapped Delta cue is reachable by scrolling");
        if (deltaTile != nullptr) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              deltaTile->mapToScene(QPointF(25, 25)).toPoint());
            QTest::qWait(40);
            check(showPageItem->property("cueIndex").toInt() == 13,
                  "clicking a wrapped cue picks that cue");
        }
        // Browsing one presentation from the library is ONE group: a
        // uniform flow with no forced row breaks anywhere.
        showPageItem->setProperty("sourceShowId", deltaId);
        QTest::qWait(140);
        const QVariantList browsedOrder = showPageItem->property("cueRows").toList();
        const QVariantMap browsedLayout = cueGrid->property("layout").toMap();
        const QVariantList browsedCells = browsedLayout.value(QStringLiteral("cells")).toList();
        check(browsedOrder.size() == 9 && browsedCells.size() == 9,
              "the browsed show lays out as one uninterrupted group");
        bool browsedFlows = true;
        for (int i = 0; i < 9; ++i) {
            const QVariantMap rect = cueRect(i);
            if (qAbs(rect.value(QStringLiteral("x")).toReal() - (i % cs7cols) * cs7cellW) > 1.0
                    || qAbs(rect.value(QStringLiteral("y")).toReal()
                            - (i / cs7cols) * cs7cellH) > 1.0)
                browsedFlows = false;
        }
        check(browsedFlows, "cues inside a group flow left to right and wrap normally");
        // Hand the rest of the suite its service back.
        showPageItem->setProperty("sourceShowId", QString());
        for (const auto& entry : docs.playlistEntries()) {
            docs.removePlaylistEntry(entry.toMap().value(QStringLiteral("entryId")).toString());
        }
        docs.selectPlaylist(homePlaylist);
        docs.selectSong(songId);
        docs.selectSlide(0);
        settings.setShowThumbLevel(2);
        window->resize(cs7Size);
        QTest::qWait(160);
        check(showPageItem->property("cueRows").toList().size() == homeRows,
              "the running order is back home after the grouped fixture");
    }
    // --- task CS.8: the ask and the scroll follow GROUPED geometry --------
    // The requested band is what the grouped view ACTUALLY shows — visible
    // rows plus one bounded look-ahead row, filler gaps costing nothing —
    // with the 32-scene cap, per-slide dedupe and burst coalescing alive on
    // top of that geometry. The two hard cases get named coverage: the very
    // bottom of the order (where contentY clamps and the cap anchors off
    // the bottom edge) and every zoom step (where the window is re-derived
    // from the new mapped cells, not remembered from the old ones).
    {
        const QString homePlaylist = docs.currentPlaylistId();
        const int homeRows = showPageItem->property("cueRows").toList().size();
        const QSize cs8Size = window->size();
        window->resize(1380, 800);
        settings.setShowThumbLevel(4);
        QTest::qWait(160);
        auto makeCs8Song = [&docs](const QString& name, int slides) {
            docs.createSong(name);
            for (int i = 0; i < slides; ++i) docs.addSlide();
            docs.flushText();
            return docs.currentSongId();
        };
        const QString cs8AlphaId = makeCs8Song(QStringLiteral("CS8 Alpha"), 3);
        const QString cs8BetaId = makeCs8Song(QStringLiteral("CS8 Beta"), 3);
        const QString cs8DeltaId = makeCs8Song(QStringLiteral("CS8 Delta"), 9);
        const QString cs8EpsId = makeCs8Song(QStringLiteral("CS8 Epsilon"), 20);
        check(docs.createPlaylist(QStringLiteral("CS.8 Order")), "the CS.8 order exists");
        QString cs8Playlist;
        for (const auto& playlist : docs.playlists()) {
            const QVariantMap row = playlist.toMap();
            if (row.value(QStringLiteral("name")).toString() == QLatin1String("CS.8 Order"))
                cs8Playlist = row.value(QStringLiteral("id")).toString();
        }
        check(!cs8Playlist.isEmpty(), "the CS.8 order is findable to select");
        docs.selectPlaylist(cs8Playlist);
        check(docs.addShowToPlaylist(cs8AlphaId) && docs.addShowToPlaylist(cs8BetaId)
                  && docs.addShowToPlaylist(cs8AlphaId) && docs.addShowToPlaylist(cs8DeltaId)
                  && docs.addShowToPlaylist(cs8EpsId),
              "the order takes Alpha, Beta, Alpha again, Delta and Epsilon");
        QTest::qWait(170);
        const QVariantList rows8 = showPageItem->property("cueRows").toList();
        check(rows8.size() == 38, "the CS.8 order walks 38 cues");
        const QString cs8LastSlide = rows8.last().toMap().value(QStringLiteral("slideId")).toString();
        auto pageCall8 = [showPageItem](const char* name) {
            QVariant answer;
            QMetaObject::invokeMethod(showPageItem, name, Q_RETURN_ARG(QVariant, answer));
            return answer;
        };
        auto cue8Rect = [cueGrid](int cue) {
            QVariant answer;
            QMetaObject::invokeMethod(cueGrid, "cueRect", Q_RETURN_ARG(QVariant, answer),
                                      Q_ARG(QVariant, cue));
            return answer.toMap();
        };
        const auto realY = [cueGrid]() { return cueGrid->property("contentY").toReal(); };
        const auto viewH = [cueGrid]() { return cueGrid->property("height").toReal(); };
        const auto scrollMax = [cueGrid]() {
            return cueGrid->property("contentHeight").toReal()
                 - cueGrid->property("height").toReal();
        };
        auto band8 = [&]() {
            QStringList keys;
            for (const auto& entry : pageCall8("slideBand").toList())
                keys << entry.toMap().value(QStringLiteral("key")).toString();
            return keys;
        };
        // The contract, mirrored from the PUBLIC grouped properties only:
        // the window is floor(contentY/cellHeight) through the viewport rows
        // plus ONE look-ahead row; filler cells cost nothing; the window is
        // capped at 32 cues (bottom-anchored when it reaches the order's
        // end) and then deduped to distinct pictures.
        auto wantedRaw = [&]() {
            const QVariantMap layout = cueGrid->property("layout").toMap();
            const QVariantList cells = layout.value(QStringLiteral("cells")).toList();
            const int cols = cueGrid->property("columns").toInt();
            const qreal cellH = cueGrid->property("cellHeight").toReal();
            const qreal lookAheadBottom = qCeil((realY() + viewH()) / cellH) * cellH + cellH;
            QList<int> wanted;
            for (int i = 0; i < cells.size(); ++i) {
                const qreal top = (i / cols) * cellH;
                if (top + cellH <= realY() || top >= lookAheadBottom) continue;
                const QVariantMap cell = cells[i].toMap();
                if (cell.contains(QStringLiteral("cueIndex")))
                    wanted << cell.value(QStringLiteral("cueIndex")).toInt();
            }
            return wanted;
        };
        auto expectedBand = [&]() {
            QStringList keys;
            QSet<QString> seen;
            QList<int> wanted = wantedRaw();
            const bool bottom = realY() + viewH() >= cueGrid->property("contentHeight").toReal() - 1;
            if (bottom) std::reverse(wanted.begin(), wanted.end());
            for (int cue : wanted) {
                const QString id = rows8[cue].toMap().value(QStringLiteral("slideId")).toString();
                if (!seen.contains(id)) { seen.insert(id); keys << id; }
            }
            if (bottom) std::reverse(keys.begin(), keys.end());
            if (keys.size() > 32)
                return bottom ? keys.mid(keys.size() - 32) : keys.mid(0, 32);
            return keys;
        };
        const auto uniqueCount = [](const QStringList& keys) {
            return QSet<QString>(keys.cbegin(), keys.cend()).size();
        };
        const auto contained = [&](int cue) {
            const QVariantMap rect = cue8Rect(cue);
            return rect.value(QStringLiteral("y")).toReal() >= realY() - 1.0
                && rect.value(QStringLiteral("y")).toReal()
                       + rect.value(QStringLiteral("height")).toReal()
                       <= realY() + viewH() + 1.0;
        };

        // The top of the order: the ask IS the mapped window, once each.
        cueGrid->setProperty("contentY", 0);
        QTest::qWait(200);
        QStringList bandKeys = band8();
        check(bandKeys == expectedBand() && !bandKeys.isEmpty()
                  && uniqueCount(bandKeys) == bandKeys.size()
                  && bandKeys.first() == rows8[0].toMap().value(QStringLiteral("slideId")).toString(),
              "at the top the ask equals the grouped window, deduped and in order");

        // Scrolling itself drives the ask on grouped geometry: a burst to
        // the bottom stays silent inside the timer window, lands ONE ask at
        // the destination, and that ask is the clamped bottom window — the
        // bottom case, with the last cue inside it.
        const int asks8 = showPageItem->property("bandRequests").toInt();
        for (int i = 1; i <= 5; ++i) {
            cueGrid->setProperty("contentY", scrollMax() * i / 5.0);
            QTest::qWait(6);
        }
        check(showPageItem->property("bandRequests").toInt() == asks8,
              "five grouped jumps inside the timer window still ask for nothing");
        QTest::qWait(200);
        check(showPageItem->property("bandRequests").toInt() - asks8 == 1,
              "the grouped burst coalesces into exactly one ask");
        bandKeys = band8();
        check(bandKeys == expectedBand() && bandKeys.size() <= 32
                  && uniqueCount(bandKeys) == bandKeys.size()
                  && bandKeys.contains(cs8LastSlide)
                  && bandKeys.last() == cs8LastSlide,
              "the bottom ask describes the clamped window and ends on the order's last cue");

        // A window across group boundaries pays nothing for the padding:
        // the band stays exactly the mapped window's distinct pictures.
        cueGrid->setProperty("contentY", qMin(cue8Rect(3).value(QStringLiteral("y")).toReal(),
                                              scrollMax()));
        QTest::qWait(200);
        check(band8() == expectedBand(),
              "group padding costs the band nothing at a boundary window");

        // Zoom re-derives the window from the NEW mapped cells: the view
        // restarts its layout at the top, the band equals the window where
        // the view actually is, and scrolling back to the bottom re-anchors
        // the cap there with the order end inside. Small cards at a wide
        // grid put far more cues in the window than the cap allows — the
        // raw window says so while the band stops at 32.
        settings.setShowThumbLevel(2);
        QTest::qWait(200);
        const int asksAtZoom2 = showPageItem->property("bandRequests").toInt();
        bandKeys = band8();
        check(asksAtZoom2 > asks8 && !bandKeys.isEmpty()
                  && bandKeys == expectedBand() && uniqueCount(bandKeys) == bandKeys.size(),
              "zooming re-derives the ask from the new mapped cells");
        const qreal pxAtZoom2 = pageCall8("bandPixels").toMap().value(QStringLiteral("w")).toReal();
        settings.setShowThumbLevel(0);
        QTest::qWait(200);
        check(band8() == expectedBand(),
              "the window follows a zoom-out to the view's position, not the old cells");
        cueGrid->setProperty("contentY", scrollMax());
        QTest::qWait(200);
        bandKeys = band8();
        const qreal pxAtZoom0 = pageCall8("bandPixels").toMap().value(QStringLiteral("w")).toReal();
        check(bandKeys.size() <= 32
                  && uniqueCount(bandKeys) == bandKeys.size()
                  && bandKeys.contains(cs8LastSlide) && bandKeys.last() == cs8LastSlide
                  && bandKeys == expectedBand(),
              "the zoomed-out bottom ask stays capped and deduped and ends on the last cue");
        check(pxAtZoom0 < pxAtZoom2,
              "the requested preview size follows the zoom step, not the old cells");

        // Scroll-to-cue, bottom case: the last cue is contained by its
        // MAPPED cell and the view ends flush at the bottom.
        settings.setShowThumbLevel(3);
        QTest::qWait(160);
        cueGrid->setProperty("contentY", 0);
        QTest::qWait(80);
        showPageItem->setProperty("cueIndex", 37);
        QTest::qWait(100);
        check(contained(37)
                  && qAbs(realY() - qMax(0.0, scrollMax())) < 1.0,
              "the last cue's cell scrolls flush into the bottom of the view");
        QTest::qWait(170);
        check(band8().contains(cs8LastSlide),
              "the ask re-anchors to where the cue step left the view");

        // Scroll-to-cue at every thumb size: a group head and a cue deep
        // inside the last group both land contained in their mapped cells,
        // whatever column count the zoom step produced.
        bool everyZoomScrolls = true;
        for (int level = 0; level <= 4; ++level) {
            settings.setShowThumbLevel(level);
            QTest::qWait(170);
            cueGrid->setProperty("contentY", 0);
            QTest::qWait(60);
            showPageItem->setProperty("cueIndex", 9);   // Delta opens its own row
            QTest::qWait(60);
            if (!contained(9)) everyZoomScrolls = false;
            showPageItem->setProperty("cueIndex", 24);  // deep inside the last group
            QTest::qWait(60);
            if (!contained(24)) everyZoomScrolls = false;
        }
        check(everyZoomScrolls,
              "every thumb size scrolls a group head and a deep cue into their mapped cells");

        // Hand the suite its service back.
        showPageItem->setProperty("cueIndex", -1);
        for (const auto& entry : docs.playlistEntries())
            docs.removePlaylistEntry(entry.toMap().value(QStringLiteral("entryId")).toString());
        docs.selectPlaylist(homePlaylist);
        docs.selectSong(songId);
        docs.selectSlide(0);
        settings.setShowThumbLevel(2);
        window->resize(cs8Size);
        QTest::qWait(160);
        check(showPageItem->property("cueRows").toList().size() == homeRows,
              "the running order is back home after the CS.8 fixture");
    }
    // --- task CS.9: integrated acceptance of the shared control set -------
    // The leaves pinned their own parts; this walk takes the seams the plan
    // names: a theme write on the Themes host lands in the Edit host's
    // resolved field, a slide customization stays the slide's own (the theme
    // row never moves), browsing moves no document, history or output byte,
    // a hidden Show page never asks the worker for the band while a shown
    // one catches up, the shared rail is keyboard-live (Tab moves, Escape
    // releases, leaving the page takes the field with it) with its first and
    // last controls reachable at a bounded window, and the tiles and chips
    // name themselves for assistive tech. Everything after it is undone —
    // the tour leaves no fingerprint on the service.
    {
        auto itemNamed = [&](const char* n) { return findNamed(window->contentItem(), n); };
        auto clickAt = [window](QQuickItem* item, qreal x, qreal y) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              item->mapToScene(QPointF(x, y)).toPoint());
        };
        auto railFlickable = [](QQuickItem* from) {
            QQuickItem* flick = from ? from->parentItem() : nullptr;
            while (flick != nullptr
                   && !(flick->property("contentY").isValid()
                        && flick->property("contentHeight").isValid()))
                flick = flick->parentItem();
            return flick;
        };
        auto scrollIntoRail = [&](QQuickItem* t) {
            QQuickItem* flick = railFlickable(t);
            if (flick == nullptr) return;
            const QPointF p = t->mapToItem(flick, QPointF(0, 0));
            const qreal cy = flick->property("contentY").toReal();
            if (p.y() < 4.0)
                flick->setProperty("contentY", cy + p.y() - 4.0);
            else if (p.y() + t->height() > flick->height() - 4.0)
                flick->setProperty("contentY",
                                   cy + p.y() + t->height() - flick->height() + 4.0);
        };
        auto clickField = [&](QQuickItem* t) {
            scrollIntoRail(t);
            QTest::qWait(50);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              t->mapToScene(QPointF(t->width() / 2.0, t->height() / 2.0))
                                  .toPoint());
            QTest::qWait(60);
        };
        auto themeRowById = [&docs](const QString& id) {
            for (const QVariant& theme : docs.themes())
                if (theme.toMap().value(QStringLiteral("id")).toString() == id)
                    return theme.toMap();
            return QVariantMap();
        };
        const QSize cs9Size = window->size();
        window->resize(1380, 800);
        QTest::qWait(140);
        // The tour runs before the TH block forks named themes, so it walks
        // the fixture's Default floor — the chain every slide resolves
        // through here, which is exactly the layer a cross-host write must
        // reach the Edit rail through.
        const QString cs9Theme = docs.themes().isEmpty()
            ? QString()
            : docs.themes().at(0).toMap().value(QStringLiteral("id")).toString();
        check(!cs9Theme.isEmpty(), "the Default floor is there to carry the tour");
        const QVariantMap houseBefore = themeRowById(cs9Theme);
        const QVariantMap appearanceBefore
            = docs.slideAppearance().value(QStringLiteral("fontSize")).toMap();
        const int sentHidden0 = showPageItem->property("bandSent").toInt();

        // Write the Songs size ON THE THEMES HOST with real keys into a
        // field scrolled visibly into the rail. (No theme-wearing step: the
        // slide resolves straight through the Default floor at this point.)
        window->setProperty("page", "themes");
        QTest::qWait(160);
        auto* cs9Editor = itemNamed("themeEditorPage");
        int houseRow = -1;
        int rowIndex = 0;
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("id")).toString() == cs9Theme)
                houseRow = rowIndex;
            ++rowIndex;
        }
        QList<QQuickItem*> navRows;
        collectNamed(window->contentItem(), "themeEditorRow", navRows);
        check(cs9Editor != nullptr && houseRow >= 0 && houseRow < navRows.size(),
              "the theme has a nav row to click");
        if (cs9Editor == nullptr || houseRow < 0 || houseRow >= navRows.size())
            return 1;
        clickAt(navRows.at(houseRow), 30, navRows.at(houseRow)->height() / 2);
        QTest::qWait(120);
        check(cs9Editor->property("selectedId").toString() == cs9Theme,
              "clicking the row selects the theme on its own host");
        QMetaObject::invokeMethod(cs9Editor, "scrollInspectorTo",
                                  Q_ARG(QVariant, QStringLiteral("themeEditorSizeField")));
        QTest::qWait(80);
        auto* themeSize = itemNamed("themeEditorSizeField");
        check(themeSize && themeSize->isVisible(), "the Songs size field is on screen");
        clickAt(themeSize, themeSize->width() / 2, themeSize->height() / 2);
        QTest::qWait(60);
        themeSize->setProperty("text", "48");
        QMetaObject::invokeMethod(themeSize, "editingFinished");
        QTest::qWait(120);
        check(themeRowById(cs9Theme).value(QStringLiteral("fontSize")).toInt() == 48,
              "the Themes host commits its theme's size through the visible field");

        // Cross-host refresh: the Edit page's resolved field reads the write.
        window->setProperty("page", "edit");
        QTest::qWait(200);
        auto* editSize = itemNamed("themeSizeField");
        auto* sizeBadge = itemNamed("customizeBadgeFontSize");
        check(editSize && sizeBadge && !sizeBadge->property("visible").toBool()
                  && editSize->property("text").toString() == QLatin1String("48"),
              "the theme write on one host lands in the Edit host's resolved field");

        // Customization on the Edit host: 96 by keys, chip wakes, and the
        // THEME row stays 48 — customization is the slide's own business.
        // The theme write re-vivified the rail: re-resolve the field, the
        // stale pointer would click wherever its corpse last lay.
        // The rail's right edge can sit under the media bin's anchored
        // extent at a narrow window, so a mouse press near the field would
        // land on the bin's toggle bar. The tour drives the field the way
        // keys do: scroll it into the rail, focus it, commit.
        editSize = itemNamed("themeSizeField");
        scrollIntoRail(editSize);
        editSize->forceActiveFocus();
        QTest::qWait(60);
        editSize->setProperty("text", "96");
        QMetaObject::invokeMethod(editSize, "editingFinished");
        QTest::qWait(120);
        const QVariantMap customized = docs.slideAppearance()
            .value(QStringLiteral("fontSize")).toMap();
        check(customized.value(QStringLiteral("customized")).toBool()
                  && customized.value(QStringLiteral("value")).toInt() == 96
                  && itemNamed("customizeBadgeFontSize")->property("visible").toBool()
                  && themeRowById(cs9Theme).value(QStringLiteral("fontSize")).toInt() == 48,
              "the slide customizes to 96 while the theme keeps its 48");
        // The chip names itself for assistive tech while it is up.
        QString chipName;
        {
            auto* liveBadge = itemNamed("customizeBadgeFontSize");
            QQmlExpression expr(QQmlEngine::contextForObject(liveBadge), liveBadge,
                                QStringLiteral("Accessible.name"));
            bool exprError = false;
            chipName = expr.evaluate(&exprError).toString();
        }
        check(chipName == QLatin1String("Revert fontSize customization"),
              "the revert chip announces itself");
        // Remove-customizations returns the rail to the theme's answer.
        auto* removeButton = itemNamed("removeCustomizationsButton");
        check(removeButton && removeButton->property("enabled").toBool(),
              "Remove customizations wakes for the one customized field");
        QMetaObject::invokeMethod(removeButton, "clicked");
        QTest::qWait(150);
        check(!docs.slideAppearance().value(QStringLiteral("fontSize")).toMap()
                       .value(QStringLiteral("customized")).toBool()
                  && !itemNamed("customizeBadgeFontSize")->property("visible").toBool()
                  && itemNamed("themeSizeField")->property("text").toString() == QLatin1String("48"),
              "Remove customizations lands back on the theme's 48");

        // Keyboard discipline on the shared rail: Tab MOVES focus inside the
        // page, Escape releases a typed field, and leaving the page takes
        // the cursor with it.
        editSize = itemNamed("themeSizeField");
        scrollIntoRail(editSize);
        editSize->forceActiveFocus();
        QTest::qWait(60);
        auto* focusedIn = window->property("activeFocusItem").value<QQuickItem*>();
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::qWait(60);
        auto* focusedOut = window->property("activeFocusItem").value<QQuickItem*>();
        check(focusedIn != nullptr && focusedOut != nullptr && focusedOut != focusedIn,
              "Tab walks on from the size field instead of squatting");
        editSize = itemNamed("themeSizeField");
        scrollIntoRail(editSize);
        editSize->forceActiveFocus();
        QTest::qWait(60);
        QTest::keyClick(window, Qt::Key_7);
        QTest::keyClick(window, Qt::Key_7);
        QTest::qWait(40);
        check(window->property("typingIntoField").toBool(), "typing into the rail is typing");
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(60);
        check(!docs.slideAppearance().value(QStringLiteral("fontSize")).toMap()
                      .value(QStringLiteral("customized")).toBool()
                  && itemNamed("themeSizeField")->property("text").toString()
                         != QLatin1String("48"),
              "Escape leaves the typed text in place and commits nothing");
        // Same unfinished type, released by LEAVING the page: the field
        // goes with the page (no squatting cursor) and its edit lands as
        // the ordinary focus-out commit the whole app follows — one undo
        // step, which the tour's walk-back below takes with it.
        window->setProperty("page", "show");
        QTest::qWait(160);
        check(!window->property("typingIntoField").toBool()
                  && !window->property("squattedFocus").toBool(),
              "leaving Edit releases the inspector field with the page");
        // The field re-reads the record on return — the focused type came
        // through as exactly one commit, not a phantom or a half value.
        window->setProperty("page", "edit");
        QTest::qWait(140);
        check(itemNamed("themeSizeField")->property("text").toString()
                      == QLatin1String("4877")
                  && docs.slideAppearance().value(QStringLiteral("fontSize")).toMap()
                         .value(QStringLiteral("customized")).toBool(),
              "the field's type committed once at the page boundary, as an ordinary edit");

        // First and last controls of the shared rail are reachable at the
        // SMALLEST ledger window: scroll the rail and both mapped rects
        // land inside it.
        auto railReachable = [&]() {
            QQuickItem* probe = findNamed(window->contentItem(), "themeSizeField");
            if (probe == nullptr) return false;
            QQuickItem* flick = probe->parentItem();
            while (flick != nullptr
                   && !(flick->property("contentY").isValid()
                        && flick->property("contentHeight").isValid()))
                flick = flick->parentItem();
            auto inside = [&](QQuickItem* t, QQuickItem* host) {
                const QPointF p = t->mapToScene(QPointF(t->width() / 2.0, t->height() / 2.0));
                const QPointF f = host->mapToScene(QPointF(0, 0));
                return p.x() >= f.x() - 1.0 && p.x() <= f.x() + host->width() + 1.0
                    && p.y() >= f.y() - 1.0 && p.y() <= f.y() + host->height() + 1.0;
            };
            if (flick == nullptr) {
                QQuickItem* first = findNamed(window->contentItem(), "themeColourField");
                QQuickItem* last = findNamed(window->contentItem(), "editShadowBlur");
                return first && last && first->isVisible() && last->isVisible();
            }
            flick->setProperty("contentY", 0);
            QTest::qWait(80);
            auto* first = findNamed(window->contentItem(), "themeColourField");
            const bool top = first != nullptr && inside(first, flick);
            flick->setProperty("contentY", flick->property("contentHeight").toReal()
                                             - flick->property("height").toReal());
            QTest::qWait(80);
            auto* last = findNamed(window->contentItem(), "editShadowBlur");
            const bool end = last != nullptr && inside(last, flick);
            return top && end;
        };
        window->resize(1000, 680);
        QTest::qWait(160);
        window->setProperty("page", "edit");
        QTest::qWait(140);
        check(railReachable(),
              "the rail's first and last inspector controls both reach the viewport");
        window->resize(1380, 800);
        QTest::qWait(140);

        // The hidden Show page stayed silent through the whole tour: every
        // churn on other pages ran past its visible guard. (The paired
        // LIVE-transport check — silent hidden, catching up when shown —
        // runs with the real preview transport further down this suite.)
        check(showPageItem->property("bandSent").toInt() == sentHidden0,
              "a hidden Show page asked the worker nothing across the tour");
        window->setProperty("page", "show");
        QTest::qWait(160);

        // Undo the tour back to its starting record: every gesture was one
        // undo step, so backing up to the captured shapes costs at most the
        // handful of writes the tour made — and stops exactly there.
        for (int i = 0; i < 8; ++i) {
            const bool home = themeRowById(cs9Theme) == houseBefore
                && docs.slideAppearance().value(QStringLiteral("fontSize")).toMap()
                       == appearanceBefore;
            if (home || !docs.canUndo()) break;
            docs.undo();
            QTest::qWait(60);
        }
        QTest::qWait(120);
        check(themeRowById(cs9Theme) == houseBefore
                  && docs.slideAppearance().value(QStringLiteral("fontSize")).toMap()
                         == appearanceBefore,
              "one undo per gesture brings the service home");

        // Browsing purity: theme selection, view tiles, page switches and a
        // cue click move no document, no history and no output byte.
        {
        const bool undoBefore = docs.canUndo();
        const bool redoBefore = docs.canRedo();
        const QVariantList themesSnapshot = docs.themes();
        const QVariantMap appearanceSnapshot = docs.slideAppearance();
        const int entriesSnapshot = docs.playlistEntries().size();
        window->setProperty("page", "themes");
        QTest::qWait(140);
        // The nav Repeater re-vivifies on every themes() change — the tour
        // wrote themes, so the rows are freshly re-born. Re-collect, never
        // click a pointer from before a document change.
        navRows.clear();
        collectNamed(window->contentItem(), "themeEditorRow", navRows);
        if (!navRows.isEmpty()) clickAt(navRows.at(0), 30, navRows.at(0)->height() / 2);
        QTest::qWait(80);
        auto* bibleTile = itemNamed("themeViewBible");
        auto* songsTile = itemNamed("themeViewSongs");
        if (bibleTile && bibleTile->isVisible())
            clickAt(bibleTile, bibleTile->width() / 2, bibleTile->height() / 2);
        QTest::qWait(100);
        if (songsTile && songsTile->isVisible())
            clickAt(songsTile, songsTile->width() / 2, songsTile->height() / 2);
        QTest::qWait(100);
        window->setProperty("page", "bible");
        QTest::qWait(140);
        check(!worker.outputLive(), "browsing never puts anything on the output");
        window->setProperty("page", "show");
        QTest::qWait(160);
        QList<QQuickItem*> tourTiles;
        collectNamed(window->contentItem(), "operatorCueTile", tourTiles);
        for (auto* tile : tourTiles) {
            if (tile->isVisible()) { clickAt(tile, 25, 25); break; }
        }
        QTest::qWait(120);
        check(docs.canUndo() == undoBefore && docs.canRedo() == redoBefore
                  && docs.themes() == themesSnapshot
                  && docs.slideAppearance() == appearanceSnapshot
                  && docs.playlistEntries().size() == entriesSnapshot
                  && !worker.outputLive(),
              "browsing moved no document, no history and no output byte");
        }

        // CS.8 left the grouped view scrolled and zoomed; CS.7's view tiles
        // have moved the geometry since. Whatever the state, the slide's own
        // customizations survive the tour untouched: the last word of the
        // rail is still the slide's, not the theme's.
        window->setProperty("page", "edit");
        QTest::qWait(140);
        const QVariantMap appearanceNow = docs.slideAppearance();
        check(appearanceNow.value(QStringLiteral("fontSize")).toMap() == appearanceBefore,
              "the slide customization is untouched by the theme tour");
        // The tour selected the themes it wrote; put the selection back on
        // the record's own theme so every later section reads what it was
        // written against. Selection is not a document edit: nothing new
        // lands on the undo stack. The rows only exist on the Themes host,
        // so the re-selection happens there.
        window->setProperty("page", "themes");
        QTest::qWait(160);
        navRows.clear();
        collectNamed(window->contentItem(), "themeEditorRow", navRows);
        for (auto* row : navRows) {
            if (row->property("themeName").toString() == QStringLiteral("Default")) {
                clickAt(row, 30, row->height() / 2);
                break;
            }
        }
        QTest::qWait(80);
        // The tour scrolled the shared rail to reach its controls; leave it
        // the way every later section was written against: at the top.
        if (auto* insp = itemNamed("editInspector"); insp != nullptr) {
            for (QQuickItem* f = insp; f != nullptr; f = f->parentItem()) {
                if (f->property("contentY").isValid()
                        && f->property("contentHeight").isValid()) {
                    f->setProperty("contentY", 0);
                    break;
                }
            }
        }
        window->setProperty("page", "show");
        window->resize(1680, 1020);
        QTest::qWait(160);
    }
    // --- task UI.5: monitor, clear strip, transport and audio area --------
    // The rail now arranges itself as the plan draws it, and every control
    // on it reports acknowledged state only: with no worker there is no
    // output to clear, nothing on the transport, and a cue with nowhere to
    // go. The interim file chooser is gone; the audio area knows the
    // service's prepared cues, by kind, and browsing one of them sends
    // nothing anywhere.
    {
        auto* preview = findNamed(window->contentItem(), "audiencePreview");
        auto* clears = findNamed(window->contentItem(), "clearControls");
        check(preview && clears && clears->isVisible(),
              "the rail carries the audience monitor and its clear strip");
        const QPointF previewTopLeft = preview->mapToScene(QPointF(0, 0));
        const QPointF previewRight = preview->mapToScene(QPointF(preview->width(), 0));
        const QPointF stripTopLeft = clears->mapToScene(QPointF(0, 0));
        check(stripTopLeft.x() >= previewRight.x()
                  && qAbs(stripTopLeft.y() - previewTopLeft.y()) < 1.0,
              "the compact clears sit beside the preview, not beneath it");
        check(findNamed(window->contentItem(), "playAudioButton") == nullptr,
              "the interim file chooser is gone from the rail");

        auto* clearSlide = findNamed(window->contentItem(), "clearSlideControl");
        auto* clearBackground = findNamed(window->contentItem(), "clearBackgroundControl");
        auto* clearAll = findNamed(window->contentItem(), "clearAllButton");
        check(clearSlide && clearBackground && clearAll
                  && !clearSlide->property("enabled").toBool()
                  && !clearBackground->property("enabled").toBool()
                  && !clearAll->property("enabled").toBool(),
              "clears and restore wait for a live output instead of eating clicks");
        check(clearAll->property("text").toString() == QLatin1String("Clear all"),
              "with nothing cleared there is no restore to offer");

        auto* transportPlay = findNamed(window->contentItem(), "transportPlayButton");
        auto* transportIdle = findNamed(window->contentItem(), "transportIdleNote");
        check(transportPlay && !transportPlay->isVisible()
                  && transportIdle && transportIdle->isVisible(),
              "the transport names its emptiness and offers no controls for nothing");

        auto* combo = findNamed(window->contentItem(), "audioCueCombo");
        auto* play = findNamed(window->contentItem(), "audioPlayButton");
        auto* stop = findNamed(window->contentItem(), "audioStopButton");
        auto* audioNote = findNamed(window->contentItem(), "audioStatusLine");
        check(combo && combo->isVisible() && play && play->isVisible() && stop && audioNote,
              "the audio area offers its cues, an explicit Play and a Stop");
        QStringList cueNames;
        for (const QVariant& cue : combo->property("model").toList())
            cueNames << cue.toMap().value(QStringLiteral("name")).toString();
        check(cueNames == (QStringList{"hymn.mp3", "ghost.mp3"}),
              "the area is populated by prepared AUDIO only — no video, no file paths");
        // Picking from the list is browsing. The bin's cue lesson, at the
        // rail: nothing until an explicit press, and even the press only
        // asks — here there is not even an output to receive the ask.
        combo->setProperty("currentIndex", 1);
        QTest::qWait(30);
        check(worker.standalonePath().isEmpty() && !worker.standaloneOnAir()
                  && worker.standaloneRefusal().isEmpty() && worker.sceneActive() == 0,
              "picking a cue from the list sends nothing to any output");
        check(!play->property("enabled").toBool() && !stop->property("enabled").toBool(),
              "Play waits for an output and Stop for a track actually on air");
        check(audioNote->property("text").toString().contains("no output connected"),
              "the area names what stands between the cue and sound");
        auto* offlineNote = findNamed(window->contentItem(), "previewOfflineNote");
        check(offlineNote && offlineNote->isVisible() && !worker.outputLive(),
              "no cue and no combo choice makes the monitor claim air it does not have");
        {
            // CS.2: the rail's monitor is the shared surface bound explicitly
            // to the Audience target — the one painter on the whole screen
            // that may show the output — and it is the offline note's frame,
            // still reachable under its old automation name.
            auto* audiencePainter = preview ? findByClass(preview, "PreviewView") : nullptr;
            check(audiencePainter && audiencePainter->property("target").toInt() == 0
                      && !audiencePainter->property("hasFrame").toBool()
                      && !preview->property("ready").toBool(),
                  "the rail's monitor paints the Audience target and claims no frame it lacks");
            check(preview && preview->property("placeholder").toString()
                                == QLatin1String("Output disconnected")
                      && qAbs(preview->width() * 9 - preview->height() * 16) < 1.0,
                  "the monitor says it is disconnected inside a 16:9 frame");
        }
        combo->setProperty("currentIndex", 0);
        QTest::qWait(20);
    }
    // --- task UI.6: toolbar controls, keyboard handback, panel restore ----
    // The header's two working controls, and the layout promises the leaf
    // makes about them: the bin toggle reads the bin's state (it never
    // remembers its own press), Search is exactly Ctrl+F — the field owns
    // the keyboard, Escape hands it back, the presenter's walk is live
    // again — the page controls work from the bar on every page, and a
    // collapsed dock restores from its strip.
    {
        auto clickAt = [window](QQuickItem* item, qreal x, qreal y) {
            if (item) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(x, y)).toPoint());
            }
        };
        auto* search = findNamed(window->contentItem(), "toolbarSearchButton");
        auto* binToggle = findNamed(window->contentItem(), "toolbarBinToggle");
        auto* tabShow = findNamed(window->contentItem(), "toolbarTabShow");
        auto* tabEdit = findNamed(window->contentItem(), "toolbarTabEdit");
        auto* tabSettings = findNamed(window->contentItem(), "toolbarTabSettings");
        check(search && binToggle && tabShow && tabEdit && tabSettings
                  && search->isVisible() && binToggle->isVisible()
                  && tabEdit->isVisible() && tabSettings->isVisible(),
              "the header offers Search, both operator pages, media visibility and Settings");
        check(binToggle->property("checked").toBool() && mediaBin->property("open").toBool(),
              "the visibility control reads the bin's own state");
        clickAt(binToggle, 30, 20);
        QTest::qWait(60);
        check(!mediaBin->property("open").toBool() && !binToggle->property("checked").toBool(),
              "the toolbar closes the bin and the control follows the state, not the click");
        clickAt(mediaBin, mediaBin->width() / 2, 20);
        QTest::qWait(60);
        check(mediaBin->property("open").toBool() && binToggle->property("checked").toBool(),
              "and the bin's own bar reopens it with the toolbar agreeing");

        clickAt(search, 30, 20);
        QTest::qWait(80);
        auto* field = findNamed(window->contentItem(), "librarySearchField");
        check(field && field->property("activeFocus").toBool()
                  && window->property("typingIntoField").toBool(),
              "the toolbar Search takes the keyboard to the sidebar field");
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(40);
        check(!window->property("typingIntoField").toBool(),
              "Escape leaves the field and the presenter's keys come back");
        showPageItem->setProperty("cueIndex", 0);
        QTest::qWait(20);
        QTest::keyClick(window, Qt::Key_PageDown);
        QTest::qWait(40);
        check(showPageItem->property("cueIndex").toInt() == 1,
              "the running-order walk is live again after a search");
        showPageItem->setProperty("cueIndex", 0);
        QTest::qWait(20);

        clickAt(tabSettings, 33, 26);
        QTest::qWait(60);
        check(window->property("page").toString() == QLatin1String("settings"),
              "the right-hand Settings tab switches modes");
        check(search->isVisible() && binToggle->isVisible() && tabShow->isVisible(),
              "no header control hides or collapses because a different page is up");
        clickAt(tabShow, 33, 26);
        QTest::qWait(60);
        check(window->property("page").toString() == QLatin1String("show")
                  && tabShow->property("selected").toBool()
                  && !tabSettings->property("selected").toBool(),
              "the page marker follows the toolbar's own choice");

        auto* right = window->findChild<QQuickItem*>("outputDock");
        const qreal rightBefore = panels.rightWidth();
        const auto gripR = right->mapToScene(QPointF(2, 60)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, gripR);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, gripR);
        check(panels.rightWidth() == rightBefore,
              "a press released in place on the handle resizes nothing");
        panels.resizeRight(4);
        QTest::qWait(60);
        check(panels.rightWidth() <= 4.5, "the right dock can collapse to its strip");
        clickAt(right, 2, right->height() / 2);
        QTest::qWait(60);
        check(panels.rightWidth() > 200.0, "a click on the collapsed strip restores it");
    }

    // --- task TH.4: the More ⋯ theme menu and the Theme Editor mode --------
    // The header submenu is the operator's one-click access to the current
    // theme from every page (worn row checked, click sets or clears), and
    // its editor entry opens a page whose New/Duplicate/Rename/Delete and
    // inspector are ordinary document edits — one entry each, undoable, with
    // refusals landing on the status line instead of in the data. Browsing
    // the editor must never move the audience: this fixture has no worker
    // started, so any output at all would be a presentation.
    {
        auto clickAt = [window](QQuickItem* item, qreal x, qreal y) {
            if (item) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(x, y)).toPoint());
            }
        };
        // The menu opens and closes through Material transitions, so its
        // state is waited for, never assumed: a fixed sleep is how the next
        // click lands on an item that is fading out instead of one that is
        // up. Rows collected from the overlay only exist while it is open.
        auto waitFor = [](const std::function<bool()>& predicate, int budgetMs = 2000) {
            QElapsedTimer timer;
            timer.start();
            while (!predicate() && timer.elapsed() < budgetMs) QTest::qWait(25);
            return predicate();
        };
        auto menuRowsWith = [&](int expected) {
            QList<QQuickItem*> rows;
            QPointF lastScene;
            bool stableOnce = false;
            // Settle means two consecutive samples of the SAME scene
            // position: the Material menu slides in through an enter
            // transition, and an item collected mid-slide is a click away
            // from nothing.
            const bool settled = waitFor([&] {
                rows.clear();
                collectNamed(window->contentItem(), "themeMenuItem", rows);
                if (rows.size() != expected) {
                    stableOnce = false;
                    return false;
                }
                const QPointF scene = rows.first()->mapToScene(QPointF(0, 0));
                if (scene != lastScene) {
                    lastScene = scene;
                    stableOnce = false;
                    return false;
                }
                if (!stableOnce) {
                    stableOnce = true;
                    return false;
                }
                return true;
            });
            return settled ? rows : QList<QQuickItem*>{};
        };
        auto* more = findNamed(window->contentItem(), "toolbarMore");
        auto* menu = window->findChild<QObject*>("themeMenu");
        check(more && more->isVisible() && menu && !menu->property("visible").toBool(),
              "the header carries the themes entry and its menu starts closed");

        clickAt(more, 33, 26);
        QTest::qWait(120);
        // The service is never without its Default theme (amendment
        // 2026-11-02), so the menu's empty state needs a document that was
        // emptied by hand — what a fresh app always offers is the Default
        // row and the editor entry.
        auto* editorEntry = findNamed(window->contentItem(), "themeMenuEditor");
        check(menu->property("visible").toBool() && editorEntry && editorEntry->isVisible()
                  && menuRowsWith(1).size() == 1
                  && docs.themes().size() == 1
                  && docs.themes().at(0).toMap().value(QStringLiteral("builtin")).toBool(),
              "More opens on the seeded Default theme and still offers the editor");

        check(docs.createBlankTheme("Alpha") && docs.createBlankTheme("Beta"),
              "two themes exist for the menu to name");
        QList<QQuickItem*> menuRows = menuRowsWith(3);
        check(!menuRows.isEmpty(), "every theme is a row in the open menu");
        const QString alphaId = docs.themes().at(1).toMap().value(QStringLiteral("id")).toString();
        clickAt(menuRows.at(1), 40, menuRows.at(1)->height() / 2);
        check(waitFor([&] { return docs.wornThemeId() == alphaId; })
                  && waitFor([&] { return !menu->property("visible").toBool(); }),
              "clicking a theme row wears it and closes the menu");
        clickAt(more, 33, 26);
        QList<QQuickItem*> menuRows2 = menuRowsWith(3);
        check(!menuRows2.isEmpty()
                  && menuRows2.at(1)->property("text").toString().startsWith(QChar(0x2713)),
              "the worn theme is the checked one in the list");
        clickAt(menuRows2.at(1), 40, menuRows2.at(1)->height() / 2);
        check(waitFor([&] { return docs.wornThemeId().isEmpty(); }),
              "clicking the worn row again takes it off");
        waitFor([&] { return !menu->property("visible").toBool(); });

        clickAt(more, 33, 26);
        QTest::qWait(120);
        auto* editorEntry2 = findNamed(window->contentItem(), "themeMenuEditor");
        clickAt(editorEntry2, 40, editorEntry2->height() / 2);
        QTest::qWait(150);
        auto* editorPage = findNamed(window->contentItem(), "themeEditorPage");
        check(window->property("page").toString() == QLatin1String("themes")
                  && editorPage && editorPage->isVisible(),
              "Theme Editor… opens the editor mode");
        auto* newButton = findNamed(window->contentItem(), "themeEditorNew");
        auto* dupButton = findNamed(window->contentItem(), "themeEditorDuplicate");
        auto* deleteButton = findNamed(window->contentItem(), "themeEditorDelete");
        auto* useToggle = findNamed(window->contentItem(), "themeEditorUseToggle");
        auto* previewFrame = findNamed(window->contentItem(), "themeEditorPreview");
        check(newButton && dupButton && deleteButton && useToggle && previewFrame
                  && newButton->isVisible() && previewFrame->isVisible(),
              "the editor lists themes beside a preview and offers New, Duplicate, Delete and Use");

        const int themesBefore = docs.themes().size();
        clickAt(newButton, newButton->width() / 2, newButton->height() / 2);
        QTest::qWait(80);
        check(docs.themes().size() == themesBefore + 1, "New adds a theme");
        docs.undo();
        QTest::qWait(80);
        check(docs.themes().size() == themesBefore, "one undo removes the new theme again");

        // Default holds row 0 and selection opens there, but Delete stays
        // asleep while it is selected — the floor cannot be deleted
        // (amendment 2026-11-02). Every other row wakes every control.
        const QString defaultId =
            docs.themes().at(0).toMap().value(QStringLiteral("id")).toString();
        const QString betaId = docs.themes().at(2).toMap().value(QStringLiteral("id")).toString();
        QList<QQuickItem*> listRows;
        collectNamed(window->contentItem(), "themeEditorRow", listRows);
        check(listRows.size() == 3 && editorPage->property("selectedId").toString() == defaultId,
              "the list shows one row per theme and selection opens on the first row");
        check(!deleteButton->property("enabled").toBool(),
              "the Default row leaves Delete disabled");
        clickAt(listRows.at(1), 30, listRows.at(1)->height() / 2);
        QTest::qWait(80);
        check(deleteButton->property("enabled").toBool()
                  && editorPage->property("selectedId").toString() == alphaId,
              "selecting an ordinary row wakes Delete");
        clickAt(listRows.at(2), 30, listRows.at(2)->height() / 2);
        QTest::qWait(80);
        check(editorPage->property("selectedId").toString() == betaId,
              "clicking a row selects that theme");
        clickAt(dupButton, dupButton->width() / 2, dupButton->height() / 2);
        QTest::qWait(80);
        bool copyNamed = false;
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("name")).toString()
                    == QStringLiteral("Beta copy")) {
                copyNamed = true;
            }
        }
        check(docs.themes().size() == themesBefore + 1 && copyNamed,
              "Duplicate copies the selection under a copy name");
        clickAt(deleteButton, deleteButton->width() / 2, deleteButton->height() / 2);
        QTest::qWait(80);
        check(docs.themes().size() == themesBefore, "Delete removes the selected theme");
        docs.undo();
        QTest::qWait(80);
        check(docs.themes().size() == themesBefore + 1,
              "one undo brings the theme and its references back");

        // Deleting Beta moved the selection (reconcile picks a living row);
        // undo restores the theme, not the click. Ask for Beta by name so
        // the toggle below wears exactly what this check selects.
        QList<QQuickItem*> preToggleRows;
        collectNamed(window->contentItem(), "themeEditorRow", preToggleRows);
        for (int i = 0; i < docs.themes().size(); ++i) {
            if (docs.themes().at(i).toMap().value(QStringLiteral("id")).toString() == betaId) {
                clickAt(preToggleRows.at(i), 30, preToggleRows.at(i)->height() / 2);
            }
        }
        QTest::qWait(80);
        check(editorPage->property("selectedId").toString() == betaId,
              "the restored theme can be selected again by row");

        clickAt(useToggle, useToggle->width() / 2, useToggle->height() / 2);
        QTest::qWait(80);
        check(docs.wornThemeId() == betaId && useToggle->property("checked").toBool(),
              "Use as current theme wears the selected theme");
        clickAt(useToggle, useToggle->width() / 2, useToggle->height() / 2);
        QTest::qWait(80);
        check(docs.wornThemeId().isEmpty() && !useToggle->property("checked").toBool(),
              "unchecking takes the current theme off again");

        // Double-click renames in place; the commit writes through the model.
        QList<QQuickItem*> listRows2;
        collectNamed(window->contentItem(), "themeEditorRow", listRows2);
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier,
                           listRows2.at(1)->mapToScene(QPointF(30, 12)).toPoint());
        QTest::qWait(80);
        // Every delegate carries a rename field; only the edited row's is
        // up, so the visible one is the interesting one.
        QList<QQuickItem*> renameFields;
        collectNamed(window->contentItem(), "themeEditorRenameField", renameFields);
        QQuickItem* renameField = nullptr;
        for (auto* f : renameFields) {
            if (f->property("visible").toBool()) renameField = f;
        }
        check(renameField && renameField->isVisible(),
              "double-clicking a row puts its name into an inline field");
        renameField->setProperty("text", "Renamed");
        QMetaObject::invokeMethod(renameField, "editingFinished");
        QTest::qWait(80);
        check(docs.themes().at(1).toMap().value(QStringLiteral("name")).toString()
                      == QStringLiteral("Renamed"),
              "committing the field renames the theme");

        auto* colourField = findNamed(window->contentItem(), "themeEditorColourField");
        check(colourField && colourField->property("text").toString().startsWith("#"),
              "the inspector opens holding the selection's own values");
        colourField->setProperty("text", "#abcdef");
        QMetaObject::invokeMethod(colourField, "editingFinished");
        QTest::qWait(80);
        check(docs.themes().at(1).toMap().value(QStringLiteral("textColor")).toString()
                      == QStringLiteral("#abcdef"),
              "typing a colour into the inspector edits the theme in place");
        colourField->setProperty("text", "green");
        QMetaObject::invokeMethod(colourField, "editingFinished");
        QTest::qWait(80);
        check(docs.themes().at(1).toMap().value(QStringLiteral("textColor")).toString()
                      == QStringLiteral("#abcdef")
                  && docs.status().contains(QStringLiteral("colour")),
              "a refused colour lands on the status line, not in the theme");

        // Browsing never touches the audience: rows clicked through with no
        // worker started — a presentation would need one, and nothing here
        // started or was refused one.
        QList<QQuickItem*> listRows3;
        collectNamed(window->contentItem(), "themeEditorRow", listRows3);
        clickAt(listRows3.at(0), 30, 12);
        QTest::qWait(60);
        clickAt(listRows3.last(), 30, 12);
        QTest::qWait(60);
        check(!worker.outputLive() && worker.frames() == 0,
              "clicking through themes asks no output and moves no audience");

        // The lyric box (themes plan amendment 2026-11-01): the overlay on
        // the sample carries the slide editor's own gestures, and the four
        // fields are the exact-number path to the SAME rectangle. The sample
        // is a fixture, so nothing here can reach the audience — what is
        // pinned is the record, one history entry per gesture, and the
        // honest disabled states of "no opinion".
        const auto placementRow = [&docs](const QString& id) {
            for (const QVariant& theme : docs.themes()) {
                if (theme.toMap().value(QStringLiteral("id")).toString() == id) {
                    return theme.toMap();
                }
            }
            return QVariantMap{};
        };
        // Drive everything below against what the editor actually has
        // selected RIGHT NOW: the browsing above ends on the last row of
        // the list, and a remembered id from before duplicate/delete/undo
        // is how an assertion tests one theme while the gestures edit the
        // theme under the operator's cursor.
        const QString placedId = editorPage->property("selectedId").toString();
        check(!placedId.isEmpty(), "the editor has a selected theme to place with");
        auto* noPlacementButton = findNamed(window->contentItem(), "themeEditorNoPlacement");
        auto* placeXField = findNamed(window->contentItem(), "themeEditorPlaceX");
        auto* placeYField = findNamed(window->contentItem(), "themeEditorPlaceY");
        auto* placeWField = findNamed(window->contentItem(), "themeEditorPlaceW");
        auto* placeHField = findNamed(window->contentItem(), "themeEditorPlaceH");
        check(noPlacementButton && placeXField && placeHField
                  && !noPlacementButton->property("enabled").toBool()
                  && placementRow(placedId).value(QStringLiteral("placementSet")).toBool() == false,
              "a theme with no placement shows the fields asleep: No placement is disabled "
              "and the row says nothing is placed");
        // Drag the box the overlay is OFFERING (the dashed built-in lyric
        // rect 160,100,1600,880): press inside it, move up-left (the wide
        // box clamps against the canvas's right edge before it clamps at
        // zero), release once.
        const qreal zoom = previewFrame->width() / 1920.0;
        const QPointF hold = previewFrame->mapToScene(QPointF(960 * zoom, 540 * zoom));
        const QPointF dragged = hold + QPointF(-20, -15);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, hold.toPoint());
        QTest::mouseMove(window, dragged.toPoint(), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dragged.toPoint());
        QTest::qWait(80);
        const QVariantMap moved = placementRow(placedId).value(QStringLiteral("placement")).toMap();
        check(placementRow(placedId).value(QStringLiteral("placementSet")).toBool()
                  && qAbs(moved.value(QStringLiteral("x")).toDouble()
                          - std::round(160.0 - 20.0 / zoom)) <= 1.0
                  && qAbs(moved.value(QStringLiteral("y")).toDouble()
                          - std::round(100.0 - 15.0 / zoom)) <= 1.0
                  && moved.value(QStringLiteral("width")).toDouble() == 1600.0,
              "dragging the offered box makes a placement out of the gesture — "
              "moved by exactly the pointer's travel, sized as it was");
        docs.undo();
        QTest::qWait(80);
        check(!placementRow(placedId).value(QStringLiteral("placementSet")).toBool(),
              "one undo removes the whole drag, not one pixel of it");
        // The typed path: four fields, one write.
        placeXField->setProperty("text", "200");
        placeYField->setProperty("text", "150");
        placeWField->setProperty("text", "1400");
        placeHField->setProperty("text", "500");
        QMetaObject::invokeMethod(placeHField, "editingFinished");
        QTest::qWait(80);
        const QVariantMap typed = placementRow(placedId).value(QStringLiteral("placement")).toMap();
        check(typed.value(QStringLiteral("x")).toDouble() == 200.0
                  && typed.value(QStringLiteral("y")).toDouble() == 150.0
                  && typed.value(QStringLiteral("width")).toDouble() == 1400.0
                  && typed.value(QStringLiteral("height")).toDouble() == 500.0
                  && noPlacementButton->property("enabled").toBool(),
              "typing the four numbers writes the placement as one unit and No placement wakes");
        // A box the page itself will not place (w < 1) never reaches the
        // model, and every field snaps back to where the record says it is.
        placeWField->setProperty("text", "0");
        QMetaObject::invokeMethod(placeWField, "editingFinished");
        QTest::qWait(80);
        check(placementRow(placedId).value(QStringLiteral("placement")).toMap()
                      .value(QStringLiteral("width")).toDouble() == 1400.0
                  && placeWField->property("text").toString() == QLatin1String("1400"),
              "a refused rectangle changes nothing and the fields snap back");
        // "No placement" is the theme's own revert: colours stay, the
        // rectangle goes, and the overlay falls back to the dashed offer.
        clickAt(noPlacementButton, noPlacementButton->width() / 2,
                noPlacementButton->height() / 2);
        QTest::qWait(80);
        check(!placementRow(placedId).value(QStringLiteral("placementSet")).toBool()
                  && !noPlacementButton->property("enabled").toBool()
                  && placeXField->property("text").toString() == QLatin1String("160")
                  && !placementRow(placedId).value(QStringLiteral("name")).toString().isEmpty(),
              "No placement takes the theme's opinion away, puts the offered box back in "
              "the fields, and leaves the theme itself intact");

        // --- typography and effects (task TS.4) -------------------------
        // The style block is SHARED, so every control here owns exactly one
        // field of it: the weight combo cannot disturb the outline, a value
        // the model refuses snaps its control back, and one gesture costs
        // one undo — including the gestures that change nothing (tabbing
        // through a field is not an edit). No worker runs in this fixture,
        // which is also the story the family picker tells.
        const auto styleRow = [&docs](const QString& id) {
            for (const QVariant& theme : docs.themes()) {
                if (theme.toMap().value(QStringLiteral("id")).toString() == id) {
                    return theme.toMap();
                }
            }
            return QVariantMap{};
        };
        auto* weightCombo = findNamed(window->contentItem(), "themeEditorWeight");
        auto* weightInherit = findNamed(window->contentItem(), "themeEditorWeightInherit");
        auto* minSizeField = findNamed(window->contentItem(), "themeEditorMinSize");
        auto* strokeEnable = findNamed(window->contentItem(), "themeEditorStrokeEnable");
        auto* strokeOff = findNamed(window->contentItem(), "themeEditorStrokeInherit");
        auto* blurSlider = findNamed(window->contentItem(), "themeEditorShadowBlur");
        auto* browseFonts = findNamed(window->contentItem(), "themeEditorBrowseFonts");
        // A closed Popup is not in the visible item tree at all — in Qt 6 it
        // is not even an Item — so the QObject tree is where the picker is
        // looked for, and "no picker" is read the same way.
        auto* fontPopup = window->findChild<QObject*>(QStringLiteral("themeEditorFontPopup"));
        auto* familyField = findNamed(window->contentItem(), "themeEditorFontField");
        auto* fontStatus = findNamed(window->contentItem(), "themeEditorFontStatus");
        check(weightCombo && weightInherit && minSizeField && strokeEnable && strokeOff
                  && blurSlider && browseFonts && fontPopup && familyField && fontStatus,
              "the Songs inspector carries the typography, effect and family controls");
        check(!weightInherit->property("enabled").toBool()
                  && !strokeOff->property("enabled").toBool()
                  && styleRow(placedId).value(QStringLiteral("fontWeightSet")).toBool() == false
                  && styleRow(placedId).value(QStringLiteral("strokeSet")).toBool() == false,
              "a silent theme's style controls start asleep and its row says nothing is set");

        // One combo choice is ONE field: the neighbours keep their silence.
        weightCombo->setProperty("currentIndex", 6);  // the model's 700
        QMetaObject::invokeMethod(weightCombo, "activated", Q_ARG(int, 6));
        QTest::qWait(80);
        check(styleRow(placedId).value(QStringLiteral("fontWeightSet")).toBool()
                  && styleRow(placedId).value(QStringLiteral("fontWeight")).toInt() == 700
                  && styleRow(placedId).value(QStringLiteral("strokeSet")).toBool() == false
                  && styleRow(placedId).value(QStringLiteral("minFontSizeSet")).toBool() == false,
              "the weight combo writes its own field and no other");
        check(weightInherit->property("enabled").toBool(),
              "the Inherit button wakes when the theme has something to give back");
        docs.undo();
        QTest::qWait(80);
        check(!styleRow(placedId).value(QStringLiteral("fontWeightSet")).toBool()
                  && !weightInherit->property("enabled").toBool()
                  && weightCombo->property("currentIndex").toInt() == 3,
              "one undo takes the weight back to silence, and the combo repaints from the record");

        // A field committed UNCHANGED writes nothing — and so it costs no
        // undo: the step back after it is still the weight, not a phantom.
        weightCombo->setProperty("currentIndex", 6);
        QMetaObject::invokeMethod(weightCombo, "activated", Q_ARG(int, 6));
        QTest::qWait(80);
        minSizeField->setProperty("text", minSizeField->property("text").toString());
        QMetaObject::invokeMethod(minSizeField, "editingFinished");
        QTest::qWait(80);
        check(!styleRow(placedId).value(QStringLiteral("minFontSizeSet")).toBool(),
              "committing the minimum size the same as shown writes nothing");
        docs.undo();
        QTest::qWait(80);
        check(!styleRow(placedId).value(QStringLiteral("fontWeightSet")).toBool(),
              "and the unchanged commit cost no undo: the step back is still the weight");

        // A value the model turns down never lands, and the field snaps
        // back to where the record says it is.
        minSizeField->setProperty("text", "18");
        QMetaObject::invokeMethod(minSizeField, "editingFinished");
        QTest::qWait(80);
        check(styleRow(placedId).value(QStringLiteral("minFontSizeSet")).toBool()
                  && styleRow(placedId).value(QStringLiteral("minFontSize")).toDouble() == 18.0,
              "typing a minimum size commits it");
        minSizeField->setProperty("text", "0.5");
        QMetaObject::invokeMethod(minSizeField, "editingFinished");
        QTest::qWait(80);
        check(styleRow(placedId).value(QStringLiteral("minFontSize")).toDouble() == 18.0
                  && minSizeField->property("text").toString() == QLatin1String("18"),
              "an off-band minimum size is refused and the field snaps back to the record");

        // Enabling an outline on a theme that carries NO stroke record
        // materialises the contract's defaults around the one member — the
        // audience already draws black width 2, so flipping the switch
        // changes the switch and nothing else.
        QMetaObject::invokeMethod(editorPage, "scrollInspectorTo",
                                  Q_ARG(QVariant, QStringLiteral("themeEditorStrokeEnable")));
        QTest::qWait(80);
        clickAt(strokeEnable, 8, strokeEnable->height() / 2);
        QTest::qWait(80);
        QVariantMap stroke = styleRow(placedId).value(QStringLiteral("stroke")).toMap();
        check(styleRow(placedId).value(QStringLiteral("strokeSet")).toBool()
                  && stroke.value(QStringLiteral("enabled")).toBool()
                  && stroke.value(QStringLiteral("color")).toString() == QLatin1String("#000000")
                  && stroke.value(QStringLiteral("width")).toDouble() == 2.0,
              "toggling the outline materialises the default record around it: black, width 2");
        clickAt(strokeEnable, 8, strokeEnable->height() / 2);
        QTest::qWait(80);
        stroke = styleRow(placedId).value(QStringLiteral("stroke")).toMap();
        check(styleRow(placedId).value(QStringLiteral("strokeSet")).toBool()
                  && !stroke.value(QStringLiteral("enabled")).toBool()
                  && !strokeEnable->property("checked").toBool(),
              "un-ticking keeps the record with enabled false — a real answer, not silence");
        clickAt(strokeOff, strokeOff->width() / 2, strokeOff->height() / 2);
        QTest::qWait(80);
        check(!styleRow(placedId).value(QStringLiteral("strokeSet")).toBool()
                  && styleRow(placedId).value(QStringLiteral("stroke")).toMap().isEmpty()
                  && !strokeOff->property("enabled").toBool(),
              "\"Off\" hands the outline back to inherit and then sleeps again");

        // A slider is ONE gesture: nothing is written while the handle is
        // held (a drag that wrote per pixel would bury the undo stack), the
        // release commits the handle's answer once, and one undo takes the
        // whole drag back.
        QMetaObject::invokeMethod(editorPage, "scrollInspectorTo",
                                  Q_ARG(QVariant, QStringLiteral("themeEditorShadowBlur")));
        QTest::qWait(60);
        const QPointF grip = blurSlider->mapToScene(
            QPointF(blurSlider->width() * 0.25, blurSlider->height() / 2));
        const QPointF carried = blurSlider->mapToScene(
            QPointF(blurSlider->width() * 0.75, blurSlider->height() / 2));
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, grip.toPoint());
        QTest::qWait(40);
        QTest::mouseMove(window, carried.toPoint(), 20);
        QTest::qWait(60);
        check(!styleRow(placedId).value(QStringLiteral("shadowSet")).toBool(),
              "dragging the blur handle writes nothing while it is held");
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, carried.toPoint());
        QTest::qWait(80);
        const double blurred = styleRow(placedId).value(QStringLiteral("shadow")).toMap()
                                   .value(QStringLiteral("blurSigma")).toDouble();
        check(styleRow(placedId).value(QStringLiteral("shadowSet")).toBool()
                  && blurred > 0.0
                  && qAbs(blurred - blurSlider->property("value").toDouble()) <= 0.51,
              "the release commits the handle's own answer, and only that member");
        docs.undo();
        QTest::qWait(80);
        check(!styleRow(placedId).value(QStringLiteral("shadowSet")).toBool(),
              "one undo backs out the whole drag — not one pixel of it");

        // The family field is the way in that needs NOTHING of the worker,
        // and the note refuses to guess: with no worker connected there is
        // no verdict on whether "Lora" exists, so it says exactly that. The
        // picker, which is nothing but the worker's answer, sleeps.
        familyField->setProperty("text", "Lora");
        QMetaObject::invokeMethod(familyField, "editingFinished");
        QTest::qWait(80);
        check(styleRow(placedId).value(QStringLiteral("fontFamily")).toString()
                  == QLatin1String("Lora")
                  && fontStatus->property("text").toString().contains(
                          QStringLiteral("cannot list")),
              "a typed family saves while the worker is away, and the note will not invent a verdict");
        check(!browseFonts->property("enabled").toBool(),
              "Browse sleeps until a worker that can enumerate its fonts is connected");
        clickAt(browseFonts, browseFonts->width() / 2, browseFonts->height() / 2);
        QTest::qWait(60);
        check(fontPopup && !fontPopup->property("visible").toBool(),
              "clicking the sleeping Browse opens no empty picker");

        // The scripture reading context (task TF.4): the toggle CREATES the
        // entry out of the theme's current look and box, the fields commit
        // one property at a time, refusals snap back, and the line note
        // says it cannot measure rather than invent a number — no worker
        // runs in this fixture. What the section writes is the SAME entry
        // the Bible page paginates against; the e2e leg pins that.
        auto* ctxToggle = findNamed(window->contentItem(), "themeEditorContextToggle");
        auto* ctxSizeField = findNamed(window->contentItem(), "themeEditorContextSize");
        auto* ctxHField = findNamed(window->contentItem(), "themeEditorContextH");
        auto* ctxLinesLabel = findNamed(window->contentItem(), "themeEditorContextLines");
        const auto contextRow = [&docs](const QString& id) {
            return docs.themeContext(id, QStringLiteral("scripture"));
        };
        // TV.1: this section is the theme's BIBLE VIEW now — reached by the
        // navigator beside the list, and browsing to it is pure navigation:
        // the selection holds, the theme record is untouched, and an
        // absent context still says nothing.
        auto* viewSongs = findNamed(window->contentItem(), "themeViewSongs");
        auto* viewBible = findNamed(window->contentItem(), "themeViewBible");
        const QString viewedId = editorPage->property("selectedId").toString();
        check(viewSongs && viewBible
                  && editorPage->property("selectedView").toString().isEmpty()
                  && editorPage->property("selectedId").toString() == viewedId,
              "the editor browses in the Songs view, with a view navigator beside the list");
        {
            // CS.2: the navigator tiles are shared surfaces on the keyed
            // thumbnail target — each tile paints only ITS view's fixture,
            // and with the lane closed an honest glyph, not borrowed pixels.
            auto* songsPainter = viewSongs ? findByClass(viewSongs, "PreviewView") : nullptr;
            check(songsPainter
                      && songsPainter->property("target").toInt() == 2
                      && songsPainter->property("key").toString()
                             == QStringLiteral("theme-view-songs-") + viewedId,
                  "the view tiles paint their own keyed thumbnails, never borrowed frames");
        }
        clickAt(viewBible, viewBible->width() / 2, viewBible->height() / 2);
        QTest::qWait(80);
        check(editorPage->property("selectedView").toString() == QLatin1String("scripture")
                  && editorPage->property("selectedId").toString() == viewedId
                  && editorPage->property("contextOn").toBool() == false,
              "the Bible button opens the reading-context view, selection and all, still silent");
        check(ctxToggle && !ctxToggle->property("checked").toBool()
                  && contextRow(placedId).value(QStringLiteral("set")).toBool() == false,
              "a theme with no reading context opens the section asleep");
        clickAt(ctxToggle, ctxToggle->width() / 2, ctxToggle->height() / 2);
        QTest::qWait(80);
        const QVariantMap created = contextRow(placedId);
        check(created.value(QStringLiteral("set")).toBool()
                  && ctxToggle->property("checked").toBool()
                  && created.value(QStringLiteral("boxSet")).toBool()
                  && created.value(QStringLiteral("height")).toDouble() == 880.0
                  && created.value(QStringLiteral("alignSet")).toBool()
                  && !created.value(QStringLiteral("fontSizeSet")).toBool()
                  // a context speaks type and a box — never colours; the
                  // record has no colour fields to speak in the first place
                  && !created.contains(QStringLiteral("textColor")),
              "toggling creates the scripture context from the theme's current look: "
              "the offered box is stated, a size of 0 stays silent");
        ctxSizeField->setProperty("text", "32");
        QMetaObject::invokeMethod(ctxSizeField, "editingFinished");
        QTest::qWait(80);
        check(contextRow(placedId).value(QStringLiteral("fontSizeSet")).toBool()
                  && contextRow(placedId).value(QStringLiteral("fontSize")).toDouble() == 32.0,
              "committing the reading size writes 32 into the context");
        docs.undo();
        QTest::qWait(80);
        check(contextRow(placedId).value(QStringLiteral("set")).toBool()
                  && !contextRow(placedId).value(QStringLiteral("fontSizeSet")).toBool(),
              "one undo takes the size commit back and leaves the context standing");
        ctxSizeField->setProperty("text", "-4");
        QMetaObject::invokeMethod(ctxSizeField, "editingFinished");
        QTest::qWait(80);
        check(!contextRow(placedId).value(QStringLiteral("fontSizeSet")).toBool()
                  && ctxSizeField->property("text").toString().isEmpty(),
              "a negative size never reaches the context and the field snaps back to silence");
        // The height field is the box as one unit: one typed number commits
        // all four.
        ctxHField->setProperty("text", "600");
        QMetaObject::invokeMethod(ctxHField, "editingFinished");
        QTest::qWait(80);
        const QVariantMap resized = contextRow(placedId);
        check(resized.value(QStringLiteral("boxSet")).toBool()
                  && resized.value(QStringLiteral("height")).toDouble() == 600.0
                  && resized.value(QStringLiteral("x")).toDouble() == 160.0,
              "typing the box's height commits the rectangle as one unit");
        check(waitFor([&] {
                  return ctxLinesLabel->property("text").toString().contains(
                          QStringLiteral("not measured"));
              }),
              "with no worker answering the line note says so instead of inventing a count");
        clickAt(ctxToggle, ctxToggle->width() / 2, ctxToggle->height() / 2);
        QTest::qWait(80);
        check(!contextRow(placedId).value(QStringLiteral("set")).toBool()
                  && !ctxToggle->property("checked").toBool(),
              "toggling off removes the reading context and the fields sleep");
        docs.undo();
        QTest::qWait(80);
        check(contextRow(placedId).value(QStringLiteral("set")).toBool()
                  && contextRow(placedId).value(QStringLiteral("height")).toDouble() == 600.0
                  && ctxToggle->property("checked").toBool(),
              "one undo brings the whole context back — the toggle follows the record");

        // TV.1: the two views keep two records. Every context edit above is
        // the BIBLE view's; the theme's own placement stayed absent and a
        // drag on the Bible box now will write the context and leave the
        // theme alone — the other way round from a Songs-view drag.
        const QVariantMap themeNow = placementRow(placedId);
        check(!themeNow.value(QStringLiteral("placementSet")).toBool()
                  && themeNow.value(QStringLiteral("fontSize")).toDouble() == 0.0,
              "the context's size and box never touched the theme's own record");
        const qreal lecternZoom = previewFrame->width() / 1920.0;
        // Press the middle of the CONTEXT's box (160,100,1600,600) and
        // carry it: the gesture commits to the view it happened in.
        const QPointF bibleHold = previewFrame->mapToScene(
            QPointF(960 * lecternZoom, 300 * lecternZoom));
        const QPointF bibleMoved = bibleHold + QPointF(10, 12);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, bibleHold.toPoint());
        QTest::mouseMove(window, bibleMoved.toPoint(), 20);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, bibleMoved.toPoint());
        QTest::qWait(80);
        const QVariantMap draggedContext = contextRow(placedId);
        check(draggedContext.value(QStringLiteral("boxSet")).toBool()
                  && qAbs(draggedContext.value(QStringLiteral("x")).toDouble()
                          - std::round(160.0 + 10.0 / lecternZoom)) <= 1.0
                  && qAbs(draggedContext.value(QStringLiteral("y")).toDouble()
                          - std::round(100.0 + 12.0 / lecternZoom)) <= 1.0
                  && !placementRow(placedId).value(QStringLiteral("placementSet")).toBool(),
              "a drag in the Bible view moves the CONTEXT's box and leaves the theme's placement absent");
        docs.undo();
        QTest::qWait(80);
        check(contextRow(placedId).value(QStringLiteral("x")).toDouble() == 160.0
                  && !placementRow(placedId).value(QStringLiteral("placementSet")).toBool(),
              "one undo rolls the view drag back to the context's own record — one entry, as the Songs view keeps it");
        // Back to Songs: the base fields wake, the context sleeps, and the
        // selection never moved.
        clickAt(viewSongs, viewSongs->width() / 2, viewSongs->height() / 2);
        QTest::qWait(80);
        auto* songsSection = findNamed(window->contentItem(), "themeEditorSongsSection");
        auto* bibleSection = findNamed(window->contentItem(), "themeEditorBibleSection");
        check(songsSection && songsSection->property("visible").toBool()
                  && bibleSection && !bibleSection->property("visible").toBool()
                  && editorPage->property("selectedId").toString() == viewedId,
              "the Songs button puts the theme's own look back and sleeps the context — the selection held");
        // And browsing stays browsing inside the Bible view: rows clicked
        // through with no worker started move no audience and create no
        // context for the themes that do not carry one.
        clickAt(viewBible, viewBible->width() / 2, viewBible->height() / 2);
        QTest::qWait(60);
        QList<QQuickItem*> viewedRows;
        collectNamed(window->contentItem(), "themeEditorRow", viewedRows);
        if (viewedRows.size() > 1) {
            clickAt(viewedRows.at(0), 30, 12);
            QTest::qWait(60);
        }
        auto* viewNote = findNamed(window->contentItem(), "themeEditorViewNote");
        check(editorPage->property("selectedView").toString() == QLatin1String("scripture")
                  && viewNote
                      && viewNote->property("text").toString().contains(QStringLiteral("BIBLE"))
                  && !worker.outputLive() && worker.frames() == 0,
              "browsing themes inside the Bible view says so and still asks the output for nothing");

        // The honest floor (amendment 2026-11-02): delete every theme the
        // operator can delete, and the service still stands on its Default —
        // every targeted control disables, New stays live, and the Default
        // row cannot be emptied away.
        if (!docs.wornThemeId().isEmpty()) {
            docs.useThemeAsCurrent(QString());
        }
        while (docs.themes().size() > 1) {
            docs.deleteTheme(docs.themes().last().toMap().value(QStringLiteral("id")).toString());
        }
        QTest::qWait(80);
        const bool onlyDefault = docs.themes().size() == 1
                                 && docs.themes().at(0).toMap()
                                            .value(QStringLiteral("builtin")).toBool();
        check(onlyDefault && !deleteButton->property("enabled").toBool()
                  && dupButton->property("enabled").toBool()
                  && useToggle->property("enabled").toBool()
                  && newButton->property("enabled").toBool(),
              "with only the Default theme left Delete sleeps and every other "
              "control stays live — the floor cannot be deleted");
        check(!docs.deleteTheme(defaultId)
                  && docs.themes().size() == 1
                  && docs.status().contains(QStringLiteral("default theme")),
              "even asked directly, the Default theme refuses to be removed");
        window->setProperty("page", "show");
        QTest::qWait(60);
    }

    // --- task TV.2: Themes reclaims the workspace, everything reachable ---
    // The docks and the bin are HIDDEN for Themes and written never; the
    // page spans the window under the header; the canvas is a real 16:9
    // picture that FITS at every supported size; the inspector's final
    // control is a real click-and-type target (scrolled into view first —
    // finding a QML object proves nothing); the view tiles ask their
    // thumbnails as one bounded two-scene batch; and leaving puts back the
    // exact workspace the operator had arranged. Mouse and key events only.
    {
        auto clickAt = [window](QQuickItem* item, qreal x, qreal y) {
            if (item) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(x, y)).toPoint());
            }
        };
        const QSize sizeBefore = window->size();
        // The Show state as the operator arranged it, recorded BEFORE
        // entering — the stored values AND the geometry they drive.
        const qreal leftBefore = panels.leftWidth();
        const qreal rightBefore = panels.rightWidth();
        const qreal drawerBefore = panels.drawerHeight();
        const qreal leftGeomBefore = left->width();
        const qreal rightGeomBefore = right->width();
        const bool binOpenBefore = mediaBin->property("open").toBool();
        window->setProperty("page", "themes");
        QTest::qWait(100);
        auto* editorPage = findNamed(window->contentItem(), "themeEditorPage");
        auto* topBar = findNamed(window->contentItem(), "topBar");
        check(editorPage && editorPage->isVisible() && topBar,
              "the Themes page opens with the header it lives under");
        check(!left->isVisible() && !right->isVisible() && !mediaBin->isVisible(),
              "Themes hides the library dock, the output dock and the media bin");
        check(panels.leftWidth() == leftBefore && panels.rightWidth() == rightBefore
                  && panels.drawerHeight() == drawerBefore
                  && mediaBin->property("open").toBool() == binOpenBefore,
              "hiding writes nothing: the stored widths and drawer are the operator's own");
        // The emergency clear now rides with the audience state, because
        // the panel that usually carries it is one of the things hidden.
        auto* headerClear = findNamed(window->contentItem(), "toolbarAudienceClear");
        check(headerClear && headerClear->isVisible(),
              "the header carries the audience clear while Themes hides the panel");
        auto* nav = findNamed(window->contentItem(), "themeEditorNav");
        auto* scroll = findNamed(window->contentItem(), "themeEditorInspectorScroll");
        auto* canvas = findNamed(window->contentItem(), "themeEditorPreview");
        auto* actions = findNamed(window->contentItem(), "themeEditorActions");
        auto* viewSongs = findNamed(window->contentItem(), "themeViewSongs");
        auto* viewBible = findNamed(window->contentItem(), "themeViewBible");
        check(nav && scroll && canvas && actions && viewSongs && viewBible,
              "the page is navigation, a fitted canvas and a scrolling inspector");
        const qreal pageLeft = editorPage->mapToScene(QPointF(0, 0)).x();
        const qreal pageTop = editorPage->mapToScene(QPointF(0, 0)).y();
        check(qAbs(pageLeft - 12) < 0.5
                  && qAbs(pageTop - (topBar->height() + 12)) < 0.5
                  && qAbs(editorPage->width() - (window->width() - 24)) <= 1.0
                  && qAbs(editorPage->height() - (window->height() - topBar->height() - 24)) <= 1.0,
              "the Themes page spans the window under the header, not the dock gutter");
        // Every supported size: the 16:9 canvas fits the workspace whole,
        // the final inspector control — scrolled to, not assumed — sits
        // inside the viewport, and both view tiles are clickable where
        // they actually are.
        auto* lastField = findNamed(window->contentItem(), "themeEditorSpacingField");
        check(lastField != nullptr, "the field the keyboard leg types into exists");
        // TS.4's effects and TR.3's transition section both lengthened the
        // Songs column, and the reachability promise is a promise about the
        // END of the rail — so the control that gets "scrolled to, not
        // assumed" is the one that is last now: the view's background duration.
        auto* lastControl = findNamed(window->contentItem(), "themeEditorTransitionMedia");
        check(lastControl != nullptr,
              "the transition section's last field is the Songs section's last control");
        // The rail being measured is the Songs one, whatever view the browsing
        // above left the page in.
        clickAt(viewSongs, viewSongs->width() / 2, viewSongs->height() / 2);
        QTest::qWait(60);
        for (const auto size : {QSize(1000, 680), QSize(1380, 800), QSize(1680, 1020)}) {
            window->resize(size);
            QTest::qWait(80);
            const QPointF topLeft = canvas->mapToScene(QPointF(0, 0));
            const QPointF bottomRight =
                canvas->mapToScene(QPointF(canvas->width(), canvas->height()));
            check(canvas->width() > 0.0 && canvas->height() > 0.0
                      && qAbs(canvas->width() / canvas->height() - 16.0 / 9.0) < 0.02,
                  "the canvas is a positive 16:9 picture at every supported size");
            check(topLeft.x() >= 0.0 && topLeft.y() >= topBar->height() - 0.5
                      && bottomRight.x() <= window->width() + 0.5
                      && bottomRight.y() <= window->height() + 0.5,
                  "the canvas fits below the header and inside the window, never clipped");
            QMetaObject::invokeMethod(editorPage, "scrollInspectorToEnd");
            QTest::qWait(50);
            const QPointF tail = lastControl->mapToScene(
                QPointF(lastControl->width() / 2, lastControl->height() / 2));
            const QPointF bibleCentre = viewBible->mapToScene(
                QPointF(viewBible->width() / 2, viewBible->height() / 2));
            check(lastControl->isVisible()
                      && tail.x() >= 0.0 && tail.x() <= window->width()
                      && tail.y() >= 0.0 && tail.y() <= window->height()
                      && bibleCentre.x() >= 0.0 && bibleCentre.y() >= 0.0
                      && bibleCentre.y() <= window->height(),
                  "the rail's last control and the view tiles are all inside the window");
            clickAt(viewBible, viewBible->width() / 2, viewBible->height() / 2);
            QTest::qWait(60);
            check(editorPage->property("selectedView").toString() == QLatin1String("scripture"),
                  "the Bible tile switches the view with one real click at this size");
            clickAt(viewSongs, viewSongs->width() / 2, viewSongs->height() / 2);
            QTest::qWait(60);
            check(editorPage->property("selectedView").toString().isEmpty(),
                  "the Songs tile switches back at this size too");
        }
        // Reachable is also a TYPING story at the minimum size: bring the
        // named field into the rail, click it, and commit it with real keys.
        // The click must be what takes the focus, the keys what writes the
        // value, and one undo what takes it back.
        window->resize(1000, 680);
        QTest::qWait(80);
        QMetaObject::invokeMethod(editorPage, "scrollInspectorTo",
                                  Q_ARG(QVariant, QStringLiteral("themeEditorSpacingField")));
        QTest::qWait(50);
        lastField->setProperty("text", "");
        clickAt(lastField, lastField->width() / 2, lastField->height() / 2);
        QTest::qWait(60);
        auto* focused = window->property("activeFocusItem").value<QQuickItem*>();
        bool focusInsideField = focused == lastField;
        for (auto* up = focused; up && !focusInsideField; up = up->parentItem())
            focusInsideField = up == lastField;
        check(focusInsideField,
              "the clicked field inside the scrolled rail holds the keyboard");
        for (const char c : std::string("1.5")) QTest::keyClick(window, c);
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(80);
        const QString typedId = editorPage->property("selectedId").toString();
        auto lineSpacingOf = [&docs](const QString& id) {
            for (const QVariant& theme : docs.themes())
                if (theme.toMap().value(QStringLiteral("id")).toString() == id)
                    return theme.toMap().value(QStringLiteral("lineSpacing")).toDouble();
            return 0.0;
        };
        check(!typedId.isEmpty() && lineSpacingOf(typedId) == 1.5,
              "the final control writes through the real keyboard at the minimum window");
        docs.undo();
        QTest::qWait(80);
        check(lineSpacingOf(typedId) != 1.5, "one undo takes the typed spacing back");
        // The view tiles' ask, pinned off the wire: ONE batch of two keyed
        // scenes for the selected theme, at a size the stream accepts, and
        // a fresh ask every time the selection moves under them.
        QVariant batchAnswer;
        QMetaObject::invokeMethod(editorPage, "viewThumbBatch",
                                  Q_RETURN_ARG(QVariant, batchAnswer));
        const QVariantList batch = batchAnswer.toList();
        const QVariantMap first = batch.value(0).toMap();
        const QVariantMap second = batch.value(1).toMap();
        check(batch.size() == 2
                  && first.value(QStringLiteral("key")).toString()
                         == QStringLiteral("theme-view-songs-") + typedId
                  && second.value(QStringLiteral("key")).toString()
                         == QStringLiteral("theme-view-bible-") + typedId
                  && !first.value(QStringLiteral("scene")).toString().isEmpty()
                  && first.value(QStringLiteral("scene")).toString()
                         != second.value(QStringLiteral("scene")).toString(),
              "the tiles ask one batch: two keys naming the selection, two distinct fixture scenes");
        QVariant askPixels;
        QMetaObject::invokeMethod(editorPage, "viewThumbPixels",
                                  Q_RETURN_ARG(QVariant, askPixels));
        const int askW = askPixels.toMap().value(QStringLiteral("w")).toInt();
        const int askH = askPixels.toMap().value(QStringLiteral("h")).toInt();
        check(askW >= 32 && askW <= 512 && askH >= 32 && askH <= 512
                  && askH == qRound(askW * 9.0 / 16.0)
                  && openshow::protocol::thumbnailExtentFits(
                         static_cast<std::uint32_t>(askW), static_cast<std::uint32_t>(askH),
                         openshow::protocol::previewSlotStrideBytes(640, 360)),
              "the tiles' ask size is 16:9 inside the envelope this app's slot accepts");
        const int asksBefore = editorPage->property("viewThumbRequests").toInt();
        check(docs.createBlankTheme("TV.2 browse"), "the browse fixture adds a theme");
        QTest::qWait(160);  // past the tiles' debounce either way
        check(editorPage->property("viewThumbRequests").toInt() > asksBefore,
              "moving the selection re-asks the tiles — the batch names the selected theme");
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("name")).toString()
                    == QStringLiteral("TV.2 browse"))
                docs.deleteTheme(theme.toMap().value(QStringLiteral("id")).toString());
        }
        QTest::qWait(80);
        // Leaving restores the operator's workspace exactly — visible, and
        // with the stored values they arranged — and hands the keyboard
        // back to the shell rather than leaving it in a field on a page
        // nobody can see any more. Geometry first gets the window back to
        // the size the widths were recorded at.
        window->setProperty("page", "show");
        window->resize(sizeBefore);
        QTest::qWait(120);
        check(left->isVisible() && right->isVisible() && mediaBin->isVisible(),
              "leaving Themes brings the docks and the bin back");
        check(panels.leftWidth() == leftBefore && panels.rightWidth() == rightBefore
                  && panels.drawerHeight() == drawerBefore
                  && qAbs(left->width() - leftGeomBefore) < 0.5
                  && qAbs(right->width() - rightGeomBefore) < 0.5
                  && mediaBin->property("open").toBool() == binOpenBefore,
              "the workspace the operator arranged comes back whole");
        check(!headerClear->isVisible(), "the header's Themes-only clear sleeps again");
        auto* stillFocused = window->property("activeFocusItem").value<QQuickItem*>();
        bool focusTrappedInEditor = false;
        for (auto* up = stillFocused; up && !focusTrappedInEditor; up = up->parentItem())
            focusTrappedInEditor = up == editorPage;
        check(!focusTrappedInEditor && !window->property("typingIntoField").toBool(),
              "exiting Themes leaves no field of the editor holding the keyboard");
    }

    // --- task TR.3: the transition controls, in both doors ------------------
    // The two seats TR.1's contract gives an operator: the service's standing
    // answer in Settings, and a view's own override in the Theme Editor. The
    // claims this fixture can make honestly are about the CONTROL and the
    // RECORD — a real choice writes the record the model holds, a refused
    // value lands nowhere and repaints the control from the model, an
    // unchanged commit costs no undo, and "disabled for a Cut" means disabled
    // WITHOUT the numbers being erased. What a widget test could never show is
    // that the number on the screen is the number the audience takes, so that
    // half lives with a real worker: frontend_journey_tests measures a landed
    // fade's length, and document_present_tests puts the same record on the
    // wire for a real scene.
    {
        auto clickAt = [window](QQuickItem* item, qreal x, qreal y) {
            if (item) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(x, y)).toPoint());
            }
        };
        const QSize sizeBeforeTransition = window->size();
        window->resize(1380, 800);
        QTest::qWait(80);
        const auto service = [&docs] { return docs.serviceDefaultTransition(); };
        const auto ms = [](const QVariantMap& record, const char* key) {
            return record.value(QLatin1String(key)).toInt();
        };
        window->setProperty("page", "settings");
        QTest::qWait(80);
        auto* kindCombo = findNamed(window->contentItem(), "serviceTransitionKind");
        auto* wordsField = findNamed(window->contentItem(), "serviceTransitionWords");
        auto* backField = findNamed(window->contentItem(), "serviceTransitionBack");
        auto* serviceNote = findNamed(window->contentItem(), "serviceTransitionNote");
        check(kindCombo && wordsField && backField && serviceNote,
              "Settings carries the service default's kind and both durations");
        check(ms(service(), "textDurationMs") == 500 && ms(service(), "mediaDurationMs") == 800
                  && kindCombo->property("currentIndex").toInt() == 0
                  && wordsField->property("text").toString() == QLatin1String("500")
                  && backField->property("text").toString() == QLatin1String("800"),
              "the controls start on the concrete 500 / 800 crossfade this service always "
                  "performed — there is no unset service default to explain");

        // One field commits the WHOLE record: the kind travels with the
        // numbers, because half a transition is not a thing the file, the wire
        // or the domain will hold.
        wordsField->setProperty("text", "120");
        QMetaObject::invokeMethod(wordsField, "editingFinished");
        QTest::qWait(80);
        check(service().value(QStringLiteral("kind")).toString() == QLatin1String("fade")
                  && ms(service(), "textDurationMs") == 120
                  && ms(service(), "mediaDurationMs") == 800,
              "typing the words duration moves that number inside the same whole record");

        // Two refusals, both from the domain's one sentence, and the field goes
        // back to what the service actually holds: a number the model turned
        // down must not stay on the screen looking stored.
        backField->setProperty("text", "10001");
        QMetaObject::invokeMethod(backField, "editingFinished");
        QTest::qWait(80);
        check(ms(service(), "mediaDurationMs") == 800
                  && backField->property("text").toString() == QLatin1String("800"),
              "an over-band background duration is refused and the field snaps back to the model");
        backField->setProperty("text", "soon");
        QMetaObject::invokeMethod(backField, "editingFinished");
        QTest::qWait(80);
        check(ms(service(), "mediaDurationMs") == 800
                  && backField->property("text").toString() == QLatin1String("800"),
              "and so is a value that is not a count of milliseconds at all — 0 is a duration "
                  "an operator might mean, so a page may not quietly type one");

        // One gesture, one undo — and the refusals above cost none of their own.
        docs.undo();
        QTest::qWait(80);
        check(ms(service(), "textDurationMs") == 500
                  && wordsField->property("text").toString() == QLatin1String("500"),
              "one undo takes the one real commit back and the field repaints from the model");

        // Cut: the kind moves, the numbers stay. Disabled, not deleted — the
        // record underneath still says what it said, so coming back to
        // Crossfade comes back to the times that were there.
        kindCombo->setProperty("currentIndex", 1);
        QMetaObject::invokeMethod(kindCombo, "activated", Q_ARG(int, 1));
        QTest::qWait(80);
        check(service().value(QStringLiteral("kind")).toString() == QLatin1String("cut")
                  && ms(service(), "textDurationMs") == 500
                  && ms(service(), "mediaDurationMs") == 800,
              "choosing Cut commits the kind and keeps both durations stored");
        check(!wordsField->property("enabled").toBool() && !backField->property("enabled").toBool()
                  && wordsField->property("text").toString() == QLatin1String("500")
                  && backField->property("text").toString() == QLatin1String("800"),
              "a Cut disables the duration fields WITHOUT erasing them");
        kindCombo->setProperty("currentIndex", 0);
        QMetaObject::invokeMethod(kindCombo, "activated", Q_ARG(int, 0));
        QTest::qWait(80);
        check(service().value(QStringLiteral("kind")).toString() == QLatin1String("fade")
                  && ms(service(), "textDurationMs") == 500
                  && wordsField->property("enabled").toBool()
                  && backField->property("enabled").toBool(),
              "and Crossfade finds the same numbers exactly where they were left");
        check(serviceNote->property("text").toString().contains(QStringLiteral("VIDEO"))
                  && serviceNote->property("text").toString().contains(
                          QStringLiteral("NEXT cue")),
              "the video-background limitation and the next-cue rule are stated beside the "
                  "controls that set them");
        docs.undo();
        QTest::qWait(80);
        check(service().value(QStringLiteral("kind")).toString() == QLatin1String("cut"),
              "and the picker's two moves were two gestures: one undo back is the cut");
        docs.redo();
        QTest::qWait(80);

        // --- the view's own record, in the Theme Editor ---------------------
        window->setProperty("page", "themes");
        QTest::qWait(160);
        auto* viewEditor = findNamed(window->contentItem(), "themeEditorPage");
        const auto themeRow = [&docs](const QString& id) {
            for (const QVariant& theme : docs.themes()) {
                if (theme.toMap().value(QStringLiteral("id")).toString() == id) {
                    return theme.toMap();
                }
            }
            return QVariantMap{};
        };
        const QString movedId = viewEditor ? viewEditor->property("selectedId").toString()
                                           : QString();
        auto* moveCombo = findNamed(window->contentItem(), "themeEditorTransitionKind");
        auto* moveInherit = findNamed(window->contentItem(), "themeEditorTransitionInherit");
        auto* moveWords = findNamed(window->contentItem(), "themeEditorTransitionWords");
        auto* moveBack = findNamed(window->contentItem(), "themeEditorTransitionMedia");
        auto* moveSource = findNamed(window->contentItem(), "themeEditorTransitionSource");
        check(viewEditor && viewEditor->isVisible() && !movedId.isEmpty() && moveCombo
                  && moveInherit && moveWords && moveBack && moveSource,
              "the Songs inspector carries the view's Transition controls on a selected theme");
        check(!themeRow(movedId).value(QStringLiteral("transitionSet")).toBool()
                  && !moveInherit->property("enabled").toBool()
                  && !moveWords->property("enabled").toBool()
                  && moveWords->property("text").toString() == QLatin1String("500")
                  && moveSource->property("text").toString().contains(QStringLiteral("inherits")),
              "a theme with no record shows the numbers it is BORROWING, says which seat they "
                  "come from, and gives Inherit nothing to take off");

        // Crossfade gives the view a record of its own out of the numbers on
        // show: a click states the answer, and nobody gets a silent 0.
        moveCombo->setProperty("currentIndex", 2);
        QMetaObject::invokeMethod(moveCombo, "activated", Q_ARG(int, 2));
        QTest::qWait(80);
        QVariantMap own = themeRow(movedId).value(QStringLiteral("transition")).toMap();
        check(themeRow(movedId).value(QStringLiteral("transitionSet")).toBool()
                  && own.value(QStringLiteral("kind")).toString() == QLatin1String("fade")
                  && own.value(QStringLiteral("textDurationMs")).toInt() == 500
                  && own.value(QStringLiteral("mediaDurationMs")).toInt() == 800,
              "Crossfade writes a whole record for the view, seeded from what was on show");
        check(moveInherit->property("enabled").toBool() && moveWords->property("enabled").toBool()
                  && moveBack->property("enabled").toBool()
                  && !moveSource->property("text").toString().contains(
                          QStringLiteral("inherits")),
              "and the section wakes: this theme answers for its own cues now");
        moveWords->setProperty("text", "90");
        QMetaObject::invokeMethod(moveWords, "editingFinished");
        QTest::qWait(80);
        own = themeRow(movedId).value(QStringLiteral("transition")).toMap();
        check(own.value(QStringLiteral("textDurationMs")).toInt() == 90
                  && own.value(QStringLiteral("mediaDurationMs")).toInt() == 800,
              "typing the view's words duration moves that number and keeps its neighbour");
        moveWords->setProperty("text", "90");
        QMetaObject::invokeMethod(moveWords, "editingFinished");
        QTest::qWait(80);
        docs.undo();
        QTest::qWait(80);
        own = themeRow(movedId).value(QStringLiteral("transition")).toMap();
        check(own.value(QStringLiteral("textDurationMs")).toInt() == 500,
              "committing the same number again wrote nothing: the step back is the "
              "Crossfade, not a phantom edit");
        docs.redo();
        QTest::qWait(80);
        moveWords->setProperty("text", "10001");
        QMetaObject::invokeMethod(moveWords, "editingFinished");
        QTest::qWait(80);
        own = themeRow(movedId).value(QStringLiteral("transition")).toMap();
        check(own.value(QStringLiteral("textDurationMs")).toInt() == 90
                  && moveWords->property("text").toString() == QLatin1String("90"),
              "an over-band view duration is refused and the field snaps back to the record");
        moveCombo->setProperty("currentIndex", 1);
        QMetaObject::invokeMethod(moveCombo, "activated", Q_ARG(int, 1));
        QTest::qWait(80);
        own = themeRow(movedId).value(QStringLiteral("transition")).toMap();
        check(own.value(QStringLiteral("kind")).toString() == QLatin1String("cut")
                  && own.value(QStringLiteral("textDurationMs")).toInt() == 90
                  && !moveWords->property("enabled").toBool()
                  && moveWords->property("text").toString() == QLatin1String("90")
                  && moveSource->property("text").toString().contains(QStringLiteral("Cut")),
              "a Cut for the view keeps its 90 disabled and says, in the source line, that "
                  "the cue is simply on");
        // The Inherit button lives at the bottom of a column longer than the
        // window: it is scrolled to, then clicked where it actually is.
        QMetaObject::invokeMethod(viewEditor, "scrollInspectorTo",
                                  Q_ARG(QVariant, QStringLiteral("themeEditorTransitionInherit")));
        QTest::qWait(80);
        clickAt(moveInherit, moveInherit->width() / 2, moveInherit->height() / 2);
        QTest::qWait(80);
        check(!themeRow(movedId).value(QStringLiteral("transitionSet")).toBool()
                  && themeRow(movedId).value(QStringLiteral("transition")).toMap().isEmpty()
                  && moveCombo->property("currentIndex").toInt() == 0
                  && moveSource->property("text").toString().contains(QStringLiteral("inherits")),
              "Inherit removes the record — the view is silent again, not storing a copy of "
                  "what silence would resolve to");
        check(worker.outputLive() == false && worker.sceneActive() == 0,
              "and none of it moved an audience: every write on this page is to a document, "
              "and a theme the operator is editing is often nowhere near the output");

        // The undo half of Inherit: the removal is a write like any other, so
        // the record the operator just gave back comes back with one step.
        docs.undo();
        QTest::qWait(80);
        check(themeRow(movedId).value(QStringLiteral("transitionSet")).toBool()
                  && themeRow(movedId).value(QStringLiteral("transition")).toMap().value(
                             QStringLiteral("kind")).toString()
                         == QLatin1String("cut")
                  && moveCombo->property("currentIndex").toInt() == 1,
              "one undo brings the surrendered record back, picker and all");

        // Give the theme a Crossfade of its own before changing seats, at a
        // number the service does not use: what the silent Bible view shows
        // next is the theme's answer, and 90/800 tells it apart from the
        // service's 500/800 in a way a Cut's ignored numbers would not. The
        // fields had gone back to the service's numbers with Inherit — silence
        // shows what silence resolves to — so the view is typed rather than
        // assumed.
        moveCombo->setProperty("currentIndex", 2);
        QMetaObject::invokeMethod(moveCombo, "activated", Q_ARG(int, 2));
        QTest::qWait(80);
        moveWords->setProperty("text", "90");
        QMetaObject::invokeMethod(moveWords, "editingFinished");
        QTest::qWait(80);
        check(themeRow(movedId).value(QStringLiteral("transition")).toMap().value(
                          QStringLiteral("kind")).toString()
                      == QLatin1String("fade")
                  && themeRow(movedId).value(QStringLiteral("transition")).toMap().value(
                             QStringLiteral("textDurationMs")).toInt()
                 == 90,
              "the theme now answers its own cues with a 90 ms words crossfade");

        // --- the BIBLE view's own record ------------------------------------
        // The context is the NEARER seat: a passage moves the way its view
        // says, and says nothing until the operator says something. This
        // theme carries no reading context yet, so the toggle makes one — the
        // same real click an operator makes — and from here every claim is
        // about which of the two records a Go would read.
        auto* viewBibleMove = findNamed(window->contentItem(), "themeViewBible");
        clickAt(viewBibleMove, viewBibleMove->width() / 2, viewBibleMove->height() / 2);
        QTest::qWait(80);
        QMetaObject::invokeMethod(viewEditor, "scrollInspectorToTop");
        QTest::qWait(60);
        const auto contextRow = [&docs](const QString& id) {
            return docs.themeContext(id, QStringLiteral("scripture"));
        };
        auto* ctxToggleMove = findNamed(window->contentItem(), "themeEditorContextToggle");
        auto* ctxMoveCombo = findNamed(window->contentItem(), "themeEditorContextTransitionKind");
        auto* ctxMoveInherit = findNamed(window->contentItem(),
                                         "themeEditorContextTransitionInherit");
        auto* ctxMoveWords = findNamed(window->contentItem(), "themeEditorContextTransitionWords");
        auto* ctxMoveBack = findNamed(window->contentItem(), "themeEditorContextTransitionMedia");
        auto* ctxMoveSource = findNamed(window->contentItem(),
                                        "themeEditorContextTransitionSource");
        auto* ctxMoveNote = findNamed(window->contentItem(), "themeEditorContextTransitionNote");
        auto* ctxSizeField = findNamed(window->contentItem(), "themeEditorContextSize");
        check(ctxToggleMove && ctxMoveCombo && ctxMoveInherit && ctxMoveWords && ctxMoveBack
                  && ctxMoveSource && ctxMoveNote && ctxSizeField,
              "the Bible inspector carries the view's own Transition controls");
        if (!contextRow(movedId).value(QStringLiteral("set")).toBool()) {
            clickAt(ctxToggleMove, ctxToggleMove->width() / 2, ctxToggleMove->height() / 2);
            QTest::qWait(80);
        }
        check(contextRow(movedId).value(QStringLiteral("set")).toBool(),
              "the Bible view has a record of its own to say something with");
        check(!contextRow(movedId).value(QStringLiteral("transitionSet")).toBool()
                  && contextRow(movedId).value(QStringLiteral("transition")).toMap().isEmpty()
                  && ctxMoveCombo->property("currentIndex").toInt() == 0
                  && !ctxMoveInherit->property("enabled").toBool(),
              "a new reading context says nothing about movement: no record to inherit away, "
              "and the picker sits on Inherit");
        check(ctxMoveWords->property("text").toString() == QLatin1String("90")
                  && ctxMoveBack->property("text").toString() == QLatin1String("800")
                  && ctxMoveSource->property("text").toString().contains(
                             QStringLiteral("inherits this theme"))
                  && !ctxMoveSource->property("text").toString().contains(
                             QStringLiteral("inherits the service")),
              "the silent view shows the THEME's numbers rather than the service's and names the "
                  "nearer seat — the same order a Go walks");

        // A Cut for the view is a DECISION recorded in its own entry: the same
        // audience result as inheriting the theme's Cut, and a different
        // document, because inheriting means the next theme edit changes this
        // view and a recorded Cut does not.
        QMetaObject::invokeMethod(viewEditor, "scrollInspectorTo",
                                  Q_ARG(QVariant,
                                        QStringLiteral("themeEditorContextTransitionKind")));
        QTest::qWait(80);
        ctxMoveCombo->setProperty("currentIndex", 1);
        QMetaObject::invokeMethod(ctxMoveCombo, "activated", Q_ARG(int, 1));
        QTest::qWait(80);
        QVariantMap ctxMove = contextRow(movedId).value(QStringLiteral("transition")).toMap();
        check(contextRow(movedId).value(QStringLiteral("transitionSet")).toBool()
                  && ctxMove.value(QStringLiteral("kind")).toString() == QLatin1String("cut")
                  && ctxMove.value(QStringLiteral("textDurationMs")).toInt() == 90
                  && ctxMove.value(QStringLiteral("mediaDurationMs")).toInt() == 800,
              "Cut writes the view a whole record of its own, seeded from what was on show");
        check(!ctxMoveWords->property("enabled").toBool()
                  && !ctxMoveBack->property("enabled").toBool()
                  && ctxMoveWords->property("text").toString() == QLatin1String("90")
                  && ctxMoveSource->property("text").toString().contains(
                             QStringLiteral("this view"))
                  && ctxMoveNote->property("text").toString().contains(QStringLiteral("VIDEO")),
              "the view's durations are disabled without being erased, the source line names the "
                  "view itself, and the video-background limit is stated here too");

        // The rule TR.1 could not express as a widget and TR.3 must not lose:
        // this entry is REPLACED by a typography commit, so the movement has
        // to travel with it. Changing the reading size is the ordinary edit
        // that would otherwise take the view's Cut off in silence.
        ctxSizeField->setProperty("text", "34");
        QMetaObject::invokeMethod(ctxSizeField, "editingFinished");
        QTest::qWait(80);
        const QVariantMap afterType = contextRow(movedId);
        ctxMove = afterType.value(QStringLiteral("transition")).toMap();
        check(afterType.value(QStringLiteral("fontSizeSet")).toBool()
                  && afterType.value(QStringLiteral("fontSize")).toDouble() == 34.0
                  && afterType.value(QStringLiteral("transitionSet")).toBool()
                  && ctxMove.value(QStringLiteral("kind")).toString() == QLatin1String("cut")
                  && ctxMove.value(QStringLiteral("textDurationMs")).toInt() == 90,
              "changing the reading size keeps the view's movement: the context verb replaces "
                  "the entry, and this is the one field its absent keys do not silence");

        // Inherit hands the movement back to the theme and touches nothing
        // else — the typography it was not about stays exactly where it was.
        QMetaObject::invokeMethod(viewEditor, "scrollInspectorTo",
                                  Q_ARG(QVariant,
                                        QStringLiteral("themeEditorContextTransitionInherit")));
        QTest::qWait(80);
        clickAt(ctxMoveInherit, ctxMoveInherit->width() / 2, ctxMoveInherit->height() / 2);
        QTest::qWait(80);
        const QVariantMap afterInherit = contextRow(movedId);
        check(!afterInherit.value(QStringLiteral("transitionSet")).toBool()
                  && afterInherit.value(QStringLiteral("transition")).toMap().isEmpty()
                  && afterInherit.value(QStringLiteral("fontSizeSet")).toBool()
                  && ctxMoveCombo->property("currentIndex").toInt() == 0
                  && ctxMoveSource->property("text").toString()
                             .contains(QStringLiteral("this theme")),
              "Inherit gives the movement back to the theme, leaves the reading size it was not "
                  "about, and points the source line down the chain again");

        // Leave the fixture as the operator left it: the reading context the
        // toggle made goes back off and the theme's own record goes back to
        // silence, so no later leg reads a movement this fixture invented. The
        // undo stack keeps the steps, which is what an operator would have.
        QMetaObject::invokeMethod(viewEditor, "scrollInspectorToTop");
        QTest::qWait(60);
        clickAt(ctxToggleMove, ctxToggleMove->width() / 2, ctxToggleMove->height() / 2);
        QTest::qWait(80);
        // Back to the Songs seat for the legs that follow: the rail they read
        // is the theme's own column, and a page left in the Bible view would
        // have them edit a context nobody meant to open.
        auto* viewSongsMove = findNamed(window->contentItem(), "themeViewSongs");
        clickAt(viewSongsMove, viewSongsMove->width() / 2, viewSongsMove->height() / 2);
        QTest::qWait(80);
        check(viewEditor->property("selectedView").toString().isEmpty(),
              "the fixture hands the page back in the Songs view");
        docs.setThemeTransition(movedId, QVariantMap{});
        QTest::qWait(80);
        check(!themeRow(movedId).value(QStringLiteral("transitionSet")).toBool()
                  && !contextRow(movedId).value(QStringLiteral("set")).toBool()
                  && ms(service(), "textDurationMs") == 500
                  && service().value(QStringLiteral("kind")).toString()
                             == QLatin1String("fade"),
              "the transition fixture leaves nothing behind for the legs that follow");
        window->setProperty("page", "show");
        window->resize(sizeBeforeTransition);
        QTest::qWait(100);
    }

    // --- task BI.5: the import page in the shell (off-air surface only) ----
    // The More menu carries one entry; it opens the page, and the page
    // opens on the online notice with nothing to add. No worker runs and
    // no network answers here — the point asserted is the SHAPE of the
    // surface: an Add that sleeps until slides measure, a Cancel, and no
    // control anywhere on the page that could reach the Library or air.
    {
        auto clickAt = [window](QQuickItem* item, qreal x, qreal y) {
            if (item) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(x, y)).toPoint());
            }
        };
        auto* more = findNamed(window->contentItem(), "toolbarMore");
        clickAt(more, 33, 26);
        QTest::qWait(80);
        auto* bibleEntry = findNamed(window->contentItem(), "bibleImportMenuItem");
        check(bibleEntry != nullptr, "the More menu carries the Bible import entry");
        clickAt(bibleEntry, 30, 12);
        QTest::qWait(80);
        auto* page = window->findChild<QQuickItem*>("bibleImportPage");
        check(window->property("page").toString() == QLatin1String("bible") && page
                  && page->isVisible(),
              "the entry opens the Bible import page");
        auto* notice = window->findChild<QQuickItem*>("bibleNoticeLabel");
        check(notice && notice->property("text").toString().contains(QStringLiteral("internet")),
              "the page opens on the online notice");
        // CS.3: the page's shape — a bounded verse scroll, a worker-card
        // collection, and an OFF-AIR canvas on the Selection target: the
        // one picture lane this page may use, never the Audience monitor's.
        auto* verseScroll = findNamed(window->contentItem(), "bibleVerseList");
        auto* cardGrid = findNamed(window->contentItem(), "bibleSlideCollection");
        auto* bibleCanvas = findNamed(window->contentItem(), "biblePreviewCanvas");
        auto* biblePainter = bibleCanvas ? findByClass(bibleCanvas, "PreviewView") : nullptr;
        check(verseScroll && cardGrid && bibleCanvas && biblePainter
                  && biblePainter->property("target").toInt() == 1
                  && bibleCanvas->property("target").toString() == QLatin1String("selection"),
              "the import page lists verses, cards the plan and paints only its off-air canvas");
        // BIF.2: the picker must OPEN on the session's translation — the
        // operator's NIV default appeared only after they clicked the menu
        // by hand because the old currentIndex binding never recomputed.
        if (auto* picker = window->findChild<QObject*>("bibleTranslationPicker")) {
            const int index = picker->property("currentIndex").toInt();
            check(index >= 0, "the translation picker opens WITH a row selected");
            check(bible.translation() == QStringLiteral("NIV") &&
                      picker->property("displayText").toString() ==
                          QStringLiteral("NIV — New International Version"),
                  "and the row it opens on is the session's NIV, name and all");
            bible.setTranslation(QStringLiteral("ESV"));  // a session-side move
            QTest::qWait(60);
            check(picker->property("currentIndex").toInt() != index &&
                      picker->property("displayText").toString().startsWith(
                          QStringLiteral("ESV — ")),
                  "changing the session's translation moves the picker");
            bible.setTranslation(QStringLiteral("NIV"));
            QTest::qWait(60);
        } else {
            check(false, "the page carries the translation picker");
        }
        // BIF.3: naming a book while typing raises the built-in canon as a
        // dropdown; Enter completes the book; unmatched text shows nothing
        // and is still sendable as typed.
        if (auto* field = window->findChild<QQuickItem*>("bibleReferenceField")) {
            // A closed Popup keeps its (hidden) delegates materialised, so
            // the dropdown's state is read from the popup itself, not from
            // the mere presence of row items in the tree.
            auto dropdownOpen = [field]() {
                // The Popup is a child object of the field, so scope the
                // lookup there: a window-wide findChild can land on any
                // same-named control the workspace happens to hold.
                auto* popup = field->findChild<QObject*>("bibleBookPopup");
                return popup && popup->property("visible").toBool();
            };
            clickAt(field, field->width() / 2, field->height() / 2);
            field->forceActiveFocus();
            QTest::qWait(60);
            for (const char c : std::string("1 cor")) QTest::keyClick(window, c);
            QTest::qWait(150);
            check(dropdownOpen(), "typing a book opens the completer's dropdown");
            QList<QQuickItem*> rows;
            collectNamed(window->contentItem(), "bibleBookSuggestionRow", rows);
            check(rows.isEmpty() || rows.first()->property("text").toString() ==
                                        QStringLiteral("1 Corinthians"),
                  "the first row names the book the ordinal spelled three ways means");
            if (!rows.empty()) {
                // The completion is taken with ENTER — the keyboard path
                // into the completer. (Synthetic mouse routing into a
                // non-modal overlay Popup is platform-dependent and not
                // what this fixture exists to accept; Enter completes and
                // retires through exactly the wiring the click uses.)
                QTest::keyClick(window, Qt::Key_Return);
                QTest::qWait(60);
                check(field->property("text").toString().startsWith(
                          QStringLiteral("1 Corinthians")),
                      "taking the completion finishes the book in the field");
                check(bible.reference().startsWith(QStringLiteral("1 Corinthians")),
                      "and the completed book is what the session holds");
                check(!dropdownOpen(), "the dropdown retires once the completion is taken");
            }
            // Free text the table knows nothing about: no dropdown, and the
            // session keeps it verbatim — the helper never gates.
            for (const char c : std::string(" zzz of nowhere")) QTest::keyClick(window, c);
            QTest::qWait(150);
            check(!dropdownOpen(), "unmatched text suggests nothing");
            QTest::keyClick(window, Qt::Key_Return);
            QTest::qWait(60);
            check(bible.reference().contains(QStringLiteral("zzz of nowhere")),
                  "and Enter still sends the text exactly as typed");
        } else {
            check(false, "the page carries the reference field");
        }
        auto* add = window->findChild<QQuickItem*>("bibleAddButton");
        auto* cancel = window->findChild<QQuickItem*>("bibleCancelButton");
        check(add && !add->property("enabled").toBool() && cancel,
              "with nothing fetched, Add sleeps and Cancel is there");
        check(add && add->property("text").toString().contains(QStringLiteral("schedule")),
              "the Add names its only destination: today's schedule");
        check(!worker.outputLive() && worker.frames() == 0,
              "opening and reading the import page asks for no output at all");
        // Even clicking Add while it sleeps (programmatic ask) writes nothing.
        const qsizetype entriesBeforeAdd = docs.playlistEntries().size();
        check(!scripture.addToSchedule() && docs.playlistEntries().size() == entriesBeforeAdd
                  && scripture.slides().isEmpty(),
              "Add with no measured slides refuses and writes nothing");
        window->setProperty("page", "show");
        QTest::qWait(60);
    }

    // --- task TH.5: apply surfaces and the customize-not-restyle inspector --
    // The show page header owns the DEFAULT of the show on screen; the cue
    // tile's right-click themes ONE slide and puts its customizations back;
    // the sidebar row's right-click bulks over a whole presentation as ONE
    // undo step; and the Edit panel's fields write per-slide customizations
    // — a shared theme entity is never restyled from a slide. Refusals land
    // on the status line (a refusal edits nothing, so nothing overwrites
    // it); successes are read off the views, never off a remembered click.
    {
        auto clickAt = [window](QQuickItem* item, qreal x, qreal y) {
            if (item) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(x, y)).toPoint());
            }
        };
        auto rightClickAt = [window](QQuickItem* item, qreal x, qreal y) {
            if (item) {
                QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                                  item->mapToScene(QPointF(x, y)).toPoint());
            }
        };
        auto waitFor = [](const std::function<bool()>& predicate, int budgetMs = 2000) {
            QElapsedTimer timer;
            timer.start();
            while (!predicate() && timer.elapsed() < budgetMs) QTest::qWait(25);
            return predicate();
        };
        // Menu rows slide in through the Material enter transition; settled
        // means the expected rows exist AND hold the same scene position
        // twice running, so the next click cannot land on nothing.
        auto settledRows = [&](const char* name, int expected) {
            QList<QQuickItem*> rows;
            QPointF lastScene;
            bool stableOnce = false;
            const bool settled = waitFor([&] {
                rows.clear();
                collectNamed(window->contentItem(), name, rows);
                if (rows.size() != expected) {
                    stableOnce = false;
                    return false;
                }
                const QPointF scene = rows.first()->mapToScene(QPointF(0, 0));
                if (scene != lastScene) {
                    lastScene = scene;
                    stableOnce = false;
                    return false;
                }
                if (!stableOnce) {
                    stableOnce = true;
                    return false;
                }
                return true;
            });
            return settled ? rows : QList<QQuickItem*>{};
        };
        auto closeMenu = [window](const char* name) {
            if (auto* menu = window->findChild<QObject*>(name))
                QMetaObject::invokeMethod(menu, "close");
        };
        auto themeIdNamed = [&docs](const QString& name) {
            for (const QVariant& theme : docs.themes()) {
                if (theme.toMap().value(QStringLiteral("name")).toString() == name)
                    return theme.toMap().value(QStringLiteral("id")).toString();
            }
            return QString();
        };
        auto songRow = [&docs](const QString& id) {
            for (const QVariant& song : docs.songs()) {
                if (song.toMap().value(QStringLiteral("id")).toString() == id)
                    return song.toMap();
            }
            return QVariantMap();
        };

        check(docs.createBlankTheme("House") && docs.createBlankTheme("Loud"),
              "two themes serve the apply surfaces");
        const QString house = themeIdNamed(QStringLiteral("House"));
        const QString loud = themeIdNamed(QStringLiteral("Loud"));
        check(docs.editTheme(loud, QStringLiteral("Loud"), QStringLiteral("#ff0000"),
                             QStringLiteral("#100000"), QString(), QString(), 0.0,
                             QStringLiteral("center"), 1.25),
              "Loud is restyled so which slide wears it is visible in the views");

        // --- the header selector sets the DEFAULT of the show on screen ---
        const QString viewed = docs.currentSongId();
        auto* selector = findNamed(window->contentItem(), "showThemeSelector");
        check(selector && selector->isVisible()
                  && selector->property("text").toString()
                         .contains(QStringLiteral("no show default")),
              "the show header names the viewed show with no default set");
        clickAt(selector, selector->width() / 2, selector->height() / 2);
        auto* defaultMenu = window->findChild<QObject*>("showDefaultThemeMenu");
        QList<QQuickItem*> defaultRows;
        const bool opened = defaultMenu && waitFor([&] {
                          return defaultMenu->property("visible").toBool();
                      });
        defaultRows = settledRows("showDefaultThemeItem", 3);
        check(opened && !defaultRows.isEmpty(),
              "the header opens a menu naming every theme");
        QQuickItem* loudRow = nullptr;
        for (auto* row : defaultRows) {
            if (row->property("text").toString() == QStringLiteral("Loud"))
                loudRow = row;
        }
        check(loudRow != nullptr, "the header menu names Loud");
        clickAt(loudRow, 40, loudRow->height() / 2);
        check(waitFor([&] {
                  return songRow(viewed).value(QStringLiteral("defaultThemeId"))
                             .toString() == loud;
              })
                  && waitFor([&] {
                         return selector->property("text").toString()
                                == QStringLiteral("Loud");
                     }),
              "picking Loud in the header makes it the viewed show's default");
        // Reopening needs the exit transition finished: a click that
        // arrives while the closing menu still holds the grab is eaten.
        waitFor([&] { return !defaultMenu->property("visible").toBool(); });
        clickAt(selector, selector->width() / 2, selector->height() / 2);
        QList<QQuickItem*> defaultRows2 = settledRows("showDefaultThemeItem", 3);
        QQuickItem* loudRow2 = nullptr;
        for (auto* row : defaultRows2) {
            if (row->property("text").toString().contains(QStringLiteral("Loud")))
                loudRow2 = row;
        }
        check(loudRow2 != nullptr
                  && loudRow2->property("text").toString().startsWith(QChar(0x2713)),
              "the worn default is the checked row when the header reopens");
        closeMenu("showDefaultThemeMenu");
        waitFor([&] { return !defaultMenu->property("visible").toBool(); });
        docs.undo();
        check(waitFor([&] {
                  return songRow(viewed).value(QStringLiteral("defaultThemeId"))
                             .toString().isEmpty();
              }),
              "one undo takes the show default back off");

        // --- cue tiles: the right-click themes ONE slide ---
        QList<QQuickItem*> tiles;
        collectNamed(window->contentItem(), "operatorCueTile", tiles);
        check(tiles.size() >= 2, "the cue grid has tiles to theme");
        rightClickAt(tiles.at(1), 40, 20);
        auto* cueMenu = window->findChild<QObject*>("cueThemeMenu");
        check(cueMenu && waitFor([&] { return cueMenu->property("visible").toBool(); })
                  && !settledRows("cueApplyThemeItem", 3).isEmpty()
                  && !settledRows("cueRemoveCustomizationItem", 1).isEmpty(),
              "right-clicking a cue opens the theme menu on it");
        QList<QQuickItem*> themeRows = settledRows("cueApplyThemeItem", 3);
        QQuickItem* loudCueRow = nullptr;
        for (auto* row : themeRows) {
            if (row->property("text").toString() == QStringLiteral("Loud"))
                loudCueRow = row;
        }
        check(loudCueRow != nullptr, "the menu names Loud");
        clickAt(loudCueRow, 40, loudCueRow->height() / 2);
        check(waitFor([&] {
                  return docs.slides().at(1).toMap().value(QStringLiteral("themeId"))
                             .toString() == loud;
              }),
              "the picked slide wears Loud");
        check(docs.slides().at(0).toMap().value(QStringLiteral("themeId")).toString()
                      .isEmpty(),
              "and only the clicked tile's slide — its neighbour follows the show still");
        closeMenu("cueThemeMenu");
        waitFor([&] { return !cueMenu->property("visible").toBool(); });

        // Remove from a clean slide is a wrong click, honestly refused.
        waitFor([&] { return !cueMenu->property("visible").toBool(); });
        rightClickAt(tiles.at(0), 40, 20);
        check(waitFor([&] { return cueMenu->property("visible").toBool(); }),
              "the menu opens on a second tile too");
        QList<QQuickItem*> removeRows = settledRows("cueRemoveCustomizationItem", 1);
        check(!removeRows.isEmpty(), "the menu carries the restore row");
        clickAt(removeRows.first(), 40, removeRows.first()->height() / 2);
        check(waitFor([&] { return docs.status().contains(QStringLiteral("restore")); }),
              "removing customizations a clean slide never made is refused on the status line");
        closeMenu("cueThemeMenu");
        waitFor([&] { return !cueMenu->property("visible").toBool(); });

        // And from a customized slide it restores — while the worn theme stays.
        docs.selectSlide(1);
        check(docs.customizeCurrentSlide(QStringLiteral("textColor"),
                                         QStringLiteral("#123456")),
              "the slide customizes before the menu restores it");
        waitFor([&] { return !cueMenu->property("visible").toBool(); });
        rightClickAt(tiles.at(1), 40, 20);
        check(waitFor([&] { return cueMenu->property("visible").toBool(); }),
              "the menu opens over the customized tile");
        QList<QQuickItem*> removeRows2 = settledRows("cueRemoveCustomizationItem", 1);
        clickAt(removeRows2.value(0), 40,
                removeRows2.value(0)->height() / 2);
        check(waitFor([&] {
                  const QVariantMap row = docs.slides().at(1).toMap();
                  return !row.value(QStringLiteral("customized")).toBool()
                         && row.value(QStringLiteral("themeId")).toString() == loud;
              }),
              "the menu restore clears the customizations and keeps the worn theme");
        closeMenu("cueThemeMenu");
        waitFor([&] { return !cueMenu->property("visible").toBool(); });

        // Follow show gives the slide's own theme back.
        waitFor([&] { return !cueMenu->property("visible").toBool(); });
        rightClickAt(tiles.at(1), 40, 20);
        check(waitFor([&] { return cueMenu->property("visible").toBool(); }),
              "the menu is up for the follow row");
        QList<QQuickItem*> followRows = settledRows("cueFollowShowItem", 1);
        clickAt(followRows.value(0), 40, followRows.value(0)->height() / 2);
        check(waitFor([&] {
                  return docs.slides().at(1).toMap().value(QStringLiteral("themeId"))
                             .toString().isEmpty();
              }),
              "follow show takes the slide's theme off again");
        closeMenu("cueThemeMenu");
        waitFor([&] { return !cueMenu->property("visible").toBool(); });

        // --- sidebar row: the bulk over a whole presentation, one undo ---
        QList<QQuickItem*> outlineRows;
        collectNamed(outline, "collectionOutlineRow", outlineRows);
        if (left->property("outlineKind").toString() != QLatin1String("library")) {
            clickAt(outlineRows.first(), 20, 14);
            QTest::qWait(60);
        }
        QString welcomeId;
        for (const QVariant& song : docs.songs()) {
            if (song.toMap().value(QStringLiteral("name")).toString()
                    == QStringLiteral("Welcome"))
                welcomeId = song.toMap().value(QStringLiteral("id")).toString();
        }
        QQuickItem* welcomeRow = nullptr;
        QList<QQuickItem*> libraryRows;
        collectNamed(window->contentItem(), "collectionLibraryRow", libraryRows);
        for (auto* row : libraryRows) {
            if (row->property("modelData").toMap().value(QStringLiteral("id")).toString()
                    == welcomeId)
                welcomeRow = row;
        }
        check(welcomeRow != nullptr, "the library lists Welcome for the bulk menu");
        rightClickAt(welcomeRow, 30, 14);
        auto* showMenu = window->findChild<QObject*>("showThemeMenu");
        check(showMenu && waitFor([&] { return showMenu->property("visible").toBool(); })
                  && !settledRows("showApplyThemeItem", 3).isEmpty()
                  && !settledRows("showRemoveAllItem", 1).isEmpty(),
              "right-clicking the library row opens the show-wide theme menu");
        QList<QQuickItem*> bulkRows = settledRows("showApplyThemeItem", 3);
        QQuickItem* bulkLoudRow = nullptr;
        for (auto* row : bulkRows) {
            if (row->property("text").toString() == QStringLiteral("Loud"))
                bulkLoudRow = row;
        }
        check(bulkLoudRow != nullptr, "the bulk menu names Loud");
        clickAt(bulkLoudRow, 40, bulkLoudRow->height() / 2);
        check(waitFor([&] {
                  for (const QVariant& slide : docs.slidesOf(welcomeId)) {
                      if (slide.toMap().value(QStringLiteral("themeId")).toString() != loud)
                          return false;
                  }
                  return true;
              }),
              "Apply theme to show puts Loud on EVERY slide of the presentation");
        docs.undo();
        check(waitFor([&] {
                  for (const QVariant& slide : docs.slidesOf(welcomeId)) {
                      if (!slide.toMap().value(QStringLiteral("themeId")).toString().isEmpty())
                          return false;
                  }
                  return true;
              }),
              "one undo removes the WHOLE bulk, not just the last slide");
        waitFor([&] { return !showMenu->property("visible").toBool(); });
        rightClickAt(welcomeRow, 30, 14);
        check(waitFor([&] { return showMenu->property("visible").toBool(); }),
              "the menu opens for the restore row");
        QList<QQuickItem*> removeAllRows = settledRows("showRemoveAllItem", 1);
        clickAt(removeAllRows.value(0), 60, removeAllRows.value(0)->height() / 2);
        check(waitFor([&] { return docs.status().contains(QStringLiteral("restore")); }),
              "restoring a show nobody customized is refused on the status line");
        closeMenu("showThemeMenu");
        waitFor([&] { return !showMenu->property("visible").toBool(); });

        // --- the Edit inspector: customize, badge, restore; never restyle ---
        docs.selectSong(viewed);
        docs.selectSlide(0);
        window->setProperty("page", "edit");
        QTest::qWait(120);
        check(docs.applyThemeToCurrentSlide(house),
              "the slide under edit wears House");
        auto* colourField = findNamed(window->contentItem(), "themeColourField");
        auto* gradientField = findNamed(window->contentItem(), "themeBackGradientField");
        auto* removeButton = findNamed(window->contentItem(), "removeCustomizationsButton");
        auto* badge = findNamed(window->contentItem(), "customizeBadgeTextColor");
        auto* wornName = findNamed(window->contentItem(), "themeWornName");
        check(colourField && gradientField && removeButton && badge && wornName,
              "the inspector carries its fields, badges and the restore");
        check(colourField->property("text").toString() == QLatin1String("#ffffff")
                  && !removeButton->property("enabled").toBool(),
              "the fields open showing the worn theme's resolved values and nothing is customized");
        check(!badge->property("visible").toBool(), "a clean field carries no badge");

        // House gains a gradient, so the slide's "" has a theme to override.
        check(docs.editTheme(house, QStringLiteral("House"), QStringLiteral("#ffffff"),
                             QStringLiteral("#000000"), QStringLiteral("#333333"),
                             QString(), 0.0, QStringLiteral("center"), 1.25),
              "House is restyled graded in the Theme Editor's way");
        check(waitFor([&] {
                  return gradientField->property("text").toString()
                         == QLatin1String("#333333");
              }),
              "the gradient field follows the worn theme through the binding");

        // Typing a colour customizes the SLIDE; the entity is untouched.
        colourField->setProperty("text", "#00ff00");
        QMetaObject::invokeMethod(colourField, "editingFinished");
        check(waitFor([&] {
                  const QVariantMap entry
                      = docs.slideAppearance().value(QStringLiteral("textColor")).toMap();
                  return entry.value(QStringLiteral("customized")).toBool()
                         && entry.value(QStringLiteral("value")).toString()
                                == QStringLiteral("#00ff00");
              }),
              "committing a colour customizes the slide's own appearance");
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("id")).toString() == house) {
                check(theme.toMap().value(QStringLiteral("textColor")).toString()
                          == QLatin1String("#ffffff"),
                      "the shared House entity keeps its white — no silent restyle");
            }
        }
        check(waitFor([&] { return badge->property("visible").toBool(); }),
              "the customized field grows its badge");

        // "" on the gradient is the explicit flat, badge and all.
        gradientField->setProperty("text", "");
        QMetaObject::invokeMethod(gradientField, "editingFinished");
        check(waitFor([&] {
                  return docs.slideAppearance().value(QStringLiteral("backgroundGradient"))
                             .toMap().value(QStringLiteral("customized")).toBool();
              }),
              "committing the empty gradient customizes the slide explicitly flat");

        // The badge reverts its own field, and only its own field. The
        // click starts where an operator's does — in the field they were
        // just typing in — so the panel holds focus when the badge is hit.
        clickAt(colourField, 20, colourField->height() / 2);
        QTest::qWait(60);
        clickAt(badge, badge->width() / 2, badge->height() / 2);
        QTest::qWait(120);
        check(waitFor([&] {
                  return !docs.slideAppearance().value(QStringLiteral("textColor")).toMap()
                              .value(QStringLiteral("customized")).toBool();
              })
                  && waitFor([&] { return !badge->property("visible").toBool(); }),
              "clicking the badge puts textColor back to the themes, badge and all");
        check(docs.slideAppearance().value(QStringLiteral("backgroundGradient")).toMap()
                      .value(QStringLiteral("customized")).toBool(),
              "the other customization survived the badge — per-field really means per-field");

        // A row below the panel's fold needs what an operator would do:
        // scroll it into the viewport before the click. (The geometry band
        // below already does the same for the page's own Flickable.)
        auto scrollIntoView = [](QQuickItem* item) {
            for (QQuickItem* p = item->parentItem(); p; p = p->parentItem()) {
                if (QString::fromLatin1(p->metaObject()->className())
                        .startsWith(QLatin1String("QQuickFlickable"))) {
                    const qreal top = item->mapToItem(p, QPointF(0, 0)).y();
                    if (top < 0 || top + item->height() > p->height())
                        p->setProperty("contentY",
                                       p->property("contentY").toReal() + top + 1.0);
                    return;
                }
            }
        };

        // A bad value is refused with the status line's reason, and the
        // field snaps back to what the model says.
        colourField->setProperty("text", "green");
        QMetaObject::invokeMethod(colourField, "editingFinished");
        check(waitFor([&] { return docs.status().contains(QStringLiteral("colour")); })
                  && !docs.slideAppearance().value(QStringLiteral("textColor")).toMap()
                            .value(QStringLiteral("customized")).toBool()
                  && colourField->property("text").toString() == QLatin1String("#ffffff"),
              "a bad colour is refused, the field snaps back, and the theme speaks again");

        // The whole-slide restore, one step, undone one step.
        check(removeButton->property("enabled").toBool(),
              "the restore button wakes while the slide is customized");
        scrollIntoView(removeButton);
        QTest::qWait(60);
        clickAt(removeButton, removeButton->width() / 2, removeButton->height() / 2);
        check(waitFor([&] {
                  return !docs.slideAppearance().value(QStringLiteral("backgroundGradient"))
                              .toMap().value(QStringLiteral("customized")).toBool();
              }),
              "Remove customizations sends the slide back to its themes");
        check(!removeButton->property("enabled").toBool(),
              "the button sleeps again on a clean slide");
        docs.undo();
        check(waitFor([&] {
                  return docs.slideAppearance().value(QStringLiteral("backgroundGradient"))
                             .toMap().value(QStringLiteral("customized")).toBool();
              }),
              "one undo brings the whole restore back");

        // The worn name is a read-out, and New forks without disturbing it.
        check(wornName->property("text").toString().contains(QStringLiteral("House")),
              "the panel names the theme the slide wears");
        const int themesBefore = docs.themes().size();
        auto* newButton = findNamed(window->contentItem(), "themeNewButton");
        scrollIntoView(newButton);
        QTest::qWait(60);
        clickAt(newButton, newButton->width() / 2, newButton->height() / 2);
        check(waitFor([&] { return docs.themes().size() == themesBefore + 1; }),
              "New forks the worn look into a fresh theme");
        check(themeIdNamed(QStringLiteral("House fork")) != QLatin1String("")
                  && docs.slides().at(0).toMap().value(QStringLiteral("themeId"))
                             .toString() == themeIdNamed(QStringLiteral("House fork")),
              "only this slide moves onto the fork");
        docs.undo();
        check(waitFor([&] { return docs.themes().size() == themesBefore; }),
              "one undo removes the fork too");

        // CS.6: the shared parts now serve these rows. The panel's names
        // survive — themeFontField, themeSizeField, themeSpacingField —
        // while the align trio became one combo (themeAlignCombo) and the
        // revert chips kept the customizeBadge names. The parts also bring
        // the rest of the Songs vocabulary to this rail: the worker's font
        // picker, weight/slant/fit/min-size, stroke and shadow — the same
        // rules, speaking about the current slide.
        {
            auto* sizeField = findNamed(window->contentItem(), "themeSizeField");
            auto* sizeBadge = findNamed(window->contentItem(), "customizeBadgeFontSize");
            auto* alignCombo = findNamed(window->contentItem(), "themeAlignCombo");
            auto* alignBadge = findNamed(window->contentItem(), "customizeBadgeAlign");
            auto* weightCombo = findNamed(window->contentItem(), "editWeight");
            auto* weightInherit = findNamed(window->contentItem(), "editWeightInherit");
            auto* strokeCheck = findNamed(window->contentItem(), "editStrokeEnable");
            auto* strokeOff = findNamed(window->contentItem(), "editStrokeOff");
            check(sizeField && sizeBadge && alignCombo && alignBadge && weightCombo
                      && weightInherit && strokeCheck && strokeOff,
                  "the migrated rail carries the family, size, alignment and effect "
                  "rows with their revert affordances");
            // An unchanged commit is not an edit: a value the chain already
            // resolves to customizes nothing (editingFinished also fires on
            // a bare focus-out — this is the guard every field relies on).
            sizeField->setProperty("text", sizeField->property("text").toString());
            QMetaObject::invokeMethod(sizeField, "editingFinished");
            QTest::qWait(80);
            check(!docs.slideAppearance().value(QStringLiteral("fontSize")).toMap()
                         .value(QStringLiteral("customized")).toBool()
                      && !sizeBadge->property("visible").toBool(),
                  "a size committed unchanged writes no customization and grows no chip");
            sizeField->setProperty("text", "72");
            QMetaObject::invokeMethod(sizeField, "editingFinished");
            QTest::qWait(80);
            check(waitFor([&] { return docs.slideAppearance().value(QStringLiteral("fontSize"))
                                        .toMap().value(QStringLiteral("customized")).toBool(); })
                      && waitFor([&] { return sizeBadge->property("visible").toBool(); }),
                  "typing a size customizes the slide and grows its chip");
            scrollIntoView(sizeBadge);
            QTest::qWait(60);
            clickAt(sizeBadge, sizeBadge->width() / 2, sizeBadge->height() / 2);
            QTest::qWait(120);
            check(!docs.slideAppearance().value(QStringLiteral("fontSize")).toMap()
                          .value(QStringLiteral("customized")).toBool()
                      && !sizeBadge->property("visible").toBool(),
                  "the chip hands the size back to the themes");
            // One combo choice is one field, reverted by its own chip.
            alignCombo->setProperty("currentIndex", 2);
            QMetaObject::invokeMethod(alignCombo, "activated", Q_ARG(int, 2));
            QTest::qWait(80);
            check(docs.slideAppearance().value(QStringLiteral("align")).toMap()
                          .value(QStringLiteral("customized")).toBool()
                      && alignBadge->property("visible").toBool(),
                  "the alignment combo customizes its own field and nothing else");
            scrollIntoView(alignBadge);
            QTest::qWait(60);
            clickAt(alignBadge, alignBadge->width() / 2, alignBadge->height() / 2);
            QTest::qWait(120);
            check(!docs.slideAppearance().value(QStringLiteral("align")).toMap()
                          .value(QStringLiteral("customized")).toBool(),
                  "the chip hands the alignment back");
            // The advanced styling the slide supports now reaches through:
            // weight speaks, Inherit quiets it, exactly as on the Songs rail.
            // The weight choices ride the same font-list reply as the family
            // list — a worker round-trip — so wait for it like the Themes
            // test does before driving the combo.
            for (int i = 0; i < 100 && weightCombo->property("count").toInt() < 9; ++i)
                QTest::qWait(50);
            check(weightCombo->property("count").toInt() == 9,
                  "the weight choices arrive in Edit");
            weightCombo->setProperty("currentIndex", 6);  // the model's 700
            QMetaObject::invokeMethod(weightCombo, "activated", Q_ARG(int, 6));
            QTest::qWait(80);
                        check(docs.slideAppearance().value(QStringLiteral("fontWeight")).toMap()
                          .value(QStringLiteral("customized")).toBool()
                      && weightInherit->property("enabled").toBool(),
                  "a weight typed in Edit customizes the slide and wakes its Inherit");
            scrollIntoView(weightInherit);
            QTest::qWait(60);
            clickAt(weightInherit, weightInherit->width() / 2, weightInherit->height() / 2);
            QTest::qWait(120);
            check(!docs.slideAppearance().value(QStringLiteral("fontWeight")).toMap()
                          .value(QStringLiteral("customized")).toBool(),
                  "Inherit hands the weight back to the theme");
            // An outline materialises, owns, and inherits back WHOLE.
            scrollIntoView(strokeCheck);
            QTest::qWait(60);
            clickAt(strokeCheck, strokeCheck->width() / 2, strokeCheck->height() / 2);
            QTest::qWait(120);
            check(waitFor([&] { return docs.slideAppearance().value(QStringLiteral("stroke"))
                                        .toMap().value(QStringLiteral("customized")).toBool(); })
                      && strokeOff->property("enabled").toBool(),
                  "the outline checkbox materialises the slide's own stroke record");
            scrollIntoView(strokeOff);
            QTest::qWait(60);
            clickAt(strokeOff, strokeOff->width() / 2, strokeOff->height() / 2);
            QTest::qWait(120);
            check(waitFor([&] { return !docs.slideAppearance().value(QStringLiteral("stroke"))
                                         .toMap().value(QStringLiteral("customized")).toBool(); }),
                  "Off hands the whole outline back to inheritance");
        }

        // --- the lyric box rides the chain (themes plan amendment) --------
        // The four geometry fields edit the box WHERE IT DRAWS: on a slide
        // whose theme places, a typed value records a placement
        // CUSTOMIZATION — one unit, with its own owner label and revert —
        // while an extra box keeps editing its authored rectangle exactly
        // as it always did.
        check(docs.setThemePlacement(house, 250.0, 150.0, 1300.0, 700.0),
              "House gains a lyric-box placement");
        auto* owner = findNamed(window->contentItem(), "placementOwner");
        auto* revert = findNamed(window->contentItem(), "revertPlacementButton");
        check(owner && revert
                  && waitFor([&] { return owner->property("visible").toBool()
                                        && owner->property("text").toString()
                                               == QLatin1String("THEME PLACES"); })
                  && !revert->property("enabled").toBool()
                  && docs.currentSlideBoxes().at(0).toMap().value(QStringLiteral("x"))
                             .toDouble() == 250.0,
              "the geometry row names the THEME as the box's owner and the canvas shows "
              "the themed rectangle with nothing to revert");
        QList<QQuickItem*> geomFields;
        collectNamed(window->contentItem(), "geometryField", geomFields);
        check(geomFields.size() == 4
                  && geomFields.first()->property("text").toString() == QLatin1String("250"),
              "the X/Y/W/H fields read the placed box, not the authored one underneath");
        geomFields.first()->setProperty("text", "500");
        QMetaObject::invokeMethod(geomFields.first(), "editingFinished");
        check(waitFor([&] { return docs.slideAppearance().value(QStringLiteral("placement"))
                                       .toMap().value(QStringLiteral("customized")).toBool(); })
                  && owner->property("text").toString() == QLatin1String("THIS SLIDE")
                  && waitFor([&] { return revert->property("enabled").toBool(); }),
              "typing on a placed box customizes the SLIDE's own spot — one unit, "
              "owner label and revert and all");
        // The geometry band lives at the bottom of the Edit page's
        // Flickable, and at this window height its last row sits under the
        // viewport edge. An operator flicks the page up to reach it, so the
        // test does exactly that before clicking.
        {
            QQuickItem* page = revert->parentItem()->parentItem()->parentItem()->parentItem();
            page->setProperty("contentY", page->property("contentHeight").toReal()
                                              - page->property("height").toReal());
            QTest::qWait(60);
        }
        clickAt(revert, revert->width() / 2, revert->height() / 2);
        QTest::qWait(120);
        check(waitFor([&] { return !docs.slideAppearance().value(QStringLiteral("placement"))
                                          .toMap().value(QStringLiteral("customized")).toBool(); })
                  && docs.currentSlideBoxes().at(0).toMap().value(QStringLiteral("x"))
                             .toDouble() == 250.0
                  && owner->property("text").toString() == QLatin1String("THEME PLACES"),
              "the revert hands the whole rectangle back to the theme in one step");
        // An extra box is geometry, never a placement: the theme drives
        // exactly one box, and everything else keeps authored behaviour.
        // The stack already carries whatever earlier fixtures left, so
        // everything here is relative to the box COUNT and the NEW id —
        // never an assumed index.
        auto* newBox = findNamed(window->contentItem(), "newBoxButton");
        const QStringList before = [&docs]() {
            QStringList ids;
            for (const QVariant& box : docs.currentSlideBoxes())
                ids.append(box.toMap().value(QStringLiteral("id")).toString());
            return ids;
        }();
        clickAt(newBox, newBox->width() / 2, newBox->height() / 2);
        QTest::qWait(80);
        QString newId;
        int newIndex = -1;
        {
            const QVariantList boxes = docs.currentSlideBoxes();
            for (int i = 0; i < boxes.size(); ++i) {
                const QString id = boxes.at(i).toMap().value(QStringLiteral("id")).toString();
                if (!before.contains(id)) {
                    newId = id;
                    newIndex = i;
                }
            }
        }
        check( docs.currentSlideBoxes().size() == before.size() + 1 && !newId.isEmpty()
                   && docs.currentSlideBoxes().at(newIndex).toMap()
                             .value(QStringLiteral("placed")).toBool() == false,
               "the new box was added and the chain does not place it");
        check(docs.currentBox() == newIndex, "the new box is the selection");
        geomFields.first()->setProperty("text", "90");
        QMetaObject::invokeMethod(geomFields.first(), "editingFinished");
        QTest::qWait(80);
        check(docs.currentSlideBoxes().at(newIndex).toMap().value(QStringLiteral("x"))
                      .toDouble()
                  == 90.0
                  && !docs.slideAppearance().value(QStringLiteral("placement")).toMap()
                            .value(QStringLiteral("customized")).toBool(),
              "typing on an unplaced box edits its AUTHORED rectangle and customizes "
              "no placement");
        // Leave the fixture exactly the story it told before this block.
        auto* deleteBox = findNamed(window->contentItem(), "deleteBoxButton");
        clickAt(deleteBox, deleteBox->width() / 2, deleteBox->height() / 2);
        QTest::qWait(80);
        check(docs.currentSlideBoxes().size() == before.size() && docs.clearThemePlacement(house),
              "the extra box goes and House stops placing — nothing leaks to the captures");
        window->setProperty("page", "show");
        QTest::qWait(60);
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
        // TV.2: the theme view tiles ride the same lane as a two-scene
        // batch — enter the page and the selected theme's Songs key is
        // owed a picture just as the grid's rows are.
        window->setProperty("page", "themes");
        QElapsedTimer tileTimeout;
        tileTimeout.start();
        QString tileKey;
        if (auto* themePage = window->findChild<QQuickItem*>("themeEditorPage")) {
            const QString tileSelection = themePage->property("selectedId").toString();
            if (!tileSelection.isEmpty())
                tileKey = QStringLiteral("theme-view-songs-") + tileSelection;
        }
        while (!tileKey.isEmpty() && tileTimeout.elapsed() < 10000
               && worker.thumbnail(tileKey).isNull())
            QTest::qWait(50);
        check(!tileKey.isEmpty() && !worker.thumbnail(tileKey).isNull(),
              "the Songs view tile receives its picture from the production worker");
        // CS.9 seam on the LIVE transport: the hidden Show page asks the
        // worker for nothing across document churn, and catches the band
        // up the moment the operator looks at it again.
        {
            const int sentLive0 = showPageItem->property("bandSent").toInt();
            window->setProperty("page", "themes");
            QTest::qWait(140);
            check(docs.addShowToPlaylist(songId), "the churn adds one entry under Show's nose");
            QTest::qWait(60);
            const QVariantList liveEntries = docs.playlistEntries();
            docs.removePlaylistEntry(liveEntries.last().toMap()
                                         .value(QStringLiteral("entryId")).toString());
            QTest::qWait(300);
            check(showPageItem->property("bandSent").toInt() == sentLive0,
                  "with a live transport the hidden Show page still asks for nothing");
            window->setProperty("page", "show");
            QTest::qWait(300);
            check(showPageItem->property("bandSent").toInt() > sentLive0,
                  "the shown Show page catches the band up at once");
        }
        window->setProperty("page", "show");
        QTest::qWait(80);
    }
    const auto capture = qEnvironmentVariable("OPENSHOW_WORKSPACE_CAPTURE");
    if (!capture.isEmpty()) {
        // UI.6's deterministic populated state, TH.6's themed state. The
        // long-named cue is imported HERE so no earlier assertion sees it:
        // it is the bin row's and rail combo's long-name story. This run is
        // a disconnected session, which is the disconnected capture; the
        // faulted sentences are pinned in the controller tests, which an
        // offscreen window cannot drive.
        const QString files = QString::fromStdString(scratch.path().toStdString());
        const QString longName = QStringLiteral(
            "A very long hymn title that has to elide inside the row column.mp3");
        {
            std::ofstream(std::filesystem::path(scratch.path().toStdString())
                          / longName.toStdString()) << "BYTES";
        }
        check(docs.importMediaFile(files + "/" + longName),
              "the capture library carries a long-named cue");
        // TH.6: every capture is taken WITH the chain in play, so the
        // pictures show what themed and customized output actually looks
        // like rather than a themeless fixture. The names come from the
        // TH.5 run's themes; the state is re-established here, not
        // inherited, so the captures stay deterministic however the
        // assertions above shuffled the documents. Show default Loud,
        // the slide under edit wears House over it, and the slide
        // customizes the gradient flat and the size — the Edit capture
        // therefore carries the worn name, two revert badges and a live
        // "Remove customizations", and the Show header names the default.
        QString houseId;
        QString loudId;
        for (const QVariant& theme : docs.themes()) {
            const QString name = theme.toMap().value(QStringLiteral("name")).toString();
            if (name == QStringLiteral("House"))
                houseId = theme.toMap().value(QStringLiteral("id")).toString();
            else if (name == QStringLiteral("Loud"))
                loudId = theme.toMap().value(QStringLiteral("id")).toString();
        }
        check(!houseId.isEmpty() && !loudId.isEmpty()
                  && docs.setShowDefaultThemeFor(docs.currentSongId(), loudId),
              "the captures' show follows a default theme");
        // The amendment's story in one rectangle: House places the lyric
        // box low and wide, so the Edit capture shows the canvas chrome
        // drawn where the AUDIENCE sees the box, under the THEME PLACES
        // label — and the Themes capture shows that rectangle being edited
        // on the sample.
        check(docs.setThemePlacement(houseId, 240.0, 620.0, 1440.0, 320.0),
              "House gains a lyric-box placement for the captures");
        docs.selectSlide(0);
        check(docs.applyThemeToCurrentSlide(houseId)
                  && docs.customizeCurrentSlide(QStringLiteral("backgroundGradient"), QString())
                  && docs.customizeCurrentSlide(QStringLiteral("fontSize"), 44.0),
              "the captured slide wears House over the default and customizes two fields");
        // The badges the edit capture shows are exactly these two: leaving
        // a field untouched must never customize it (editingFinished also
        // fires on focus-out — the guard in commit() is what pins this).
        {
            const QVariantMap appearance = docs.slideAppearance();
            int customizedCount = 0;
            for (const QString& field : {QStringLiteral("textColor"),
                                         QStringLiteral("backgroundColor"),
                                         QStringLiteral("backgroundGradient"),
                                         QStringLiteral("fontFamily"),
                                         QStringLiteral("fontSize"),
                                         QStringLiteral("align"),
                                         QStringLiteral("lineSpacing")})
                customizedCount += appearance.value(field).toMap()
                    .value(QStringLiteral("customized")).toBool() ? 1 : 0;
            check(customizedCount == 2
                      && appearance.value(QStringLiteral("backgroundGradient")).toMap()
                             .value(QStringLiteral("customized")).toBool()
                      && appearance.value(QStringLiteral("fontSize")).toMap()
                             .value(QStringLiteral("customized")).toBool(),
                  "exactly the gradient and the size are customized — nothing else");
        }
        QTest::qWait(60);
        window->grabWindow().save(capture);
        panels.toggleDrawer();
        QTest::qWait(60);
        window->grabWindow().save(capture + ".closed.png");
        panels.toggleDrawer();
        QTest::qWait(60);
        window->resize(QSize(1380, 800));
        QTest::qWait(50);
        window->grabWindow().save(capture + ".1380.png");
        panels.toggleDrawer();
        QTest::qWait(50);
        window->grabWindow().save(capture + ".1380.closed.png");
        panels.toggleDrawer();
        QTest::qWait(50);
        window->resize(QSize(1000, 680));
        QTest::qWait(50);
        window->grabWindow().save(capture + ".1000.png");
        panels.toggleDrawer();
        QTest::qWait(50);
        window->grabWindow().save(capture + ".1000.closed.png");
        panels.toggleDrawer();
        QTest::qWait(50);
        // The empty-collection word: no pictures exist, so the Pictures
        // collection must say what is missing instead of showing a void.
        mediaBin->setProperty("filter", "picture");
        QTest::qWait(50);
        window->grabWindow().save(capture + ".emptyfilter.png");
        mediaBin->setProperty("filter", "");
        QTest::qWait(50);
        window->resize(QSize(1680, 1020));
        QTest::qWait(50);
        window->setProperty("page", "edit");
        QTest::qWait(50);
        // The capture's inspector shows the story: badges on exactly the
        // two customized fields and the worn theme named to be read.
        {
            auto* gradientBadge =
                findNamed(window->contentItem(), "customizeBadgeBackgroundGradient");
            auto* sizeBadge = findNamed(window->contentItem(), "customizeBadgeFontSize");
            auto* colourBadge = findNamed(window->contentItem(), "customizeBadgeTextColor");
            auto* worn = findNamed(window->contentItem(), "themeWornName");
            auto* ownerLabel = findNamed(window->contentItem(), "placementOwner");
            check(gradientBadge->property("visible").toBool()
                      && sizeBadge->property("visible").toBool()
                      && !colourBadge->property("visible").toBool()
                      && worn->property("text").toString() == QStringLiteral("House")
                      && ownerLabel->property("visible").toBool()
                      && ownerLabel->property("text").toString()
                             == QStringLiteral("THEME PLACES"),
                  "the edit capture carries exactly the two badges, the worn name and "
                  "the THEME PLACES label on a box the theme drives");
        }
        window->grabWindow().save(capture + ".edit.png");
        // The Theme Editor, mid-edit on the placing theme: the overlay's
        // solid rectangle IS the record's rectangle, and the fields hold
        // its numbers.
        window->setProperty("page", "themes");
        QTest::qWait(80);
        if (auto* editor = findNamed(window->contentItem(), "themeEditorPage")) {
            editor->setProperty("selectedId", houseId);
        }
        QTest::qWait(80);
        {
            auto* editor = findNamed(window->contentItem(), "themeEditorPage");
            auto* x = findNamed(window->contentItem(), "themeEditorPlaceX");
            auto* y = findNamed(window->contentItem(), "themeEditorPlaceY");
            auto* w = findNamed(window->contentItem(), "themeEditorPlaceW");
            auto* h = findNamed(window->contentItem(), "themeEditorPlaceH");
            check(editor->property("selectedId").toString() == houseId
                      && x->property("text").toString() == QLatin1String("240")
                      && y->property("text").toString() == QLatin1String("620")
                      && w->property("text").toString() == QLatin1String("1440")
                      && h->property("text").toString() == QLatin1String("320"),
                  "selecting the placing theme puts its rectangle in the fields, and a "
                  "page re-shown after outside edits still reads the record");
        }
        window->grabWindow().save(capture + ".themes.png");
        // TV.2: the Themes workspace at every ledger size in both views.
        // The Bible captures carry a lectern context so the picture, the
        // overlay and the section are visibly the BIBLE view rather than
        // the Songs one again — one deterministic state, six pictures.
        {
            auto clickCaptureTile = [window](QQuickItem* item) {
                if (item) {
                    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                      item->mapToScene(QPointF(item->width() / 2,
                                                               item->height() / 2))
                                          .toPoint());
                }
            };
            QVariantMap lectern;
            lectern.insert(QStringLiteral("fontSize"), 40.0);
            QVariantMap lecternBox;
            lecternBox.insert(QStringLiteral("x"), 240.0);
            lecternBox.insert(QStringLiteral("y"), 120.0);
            lecternBox.insert(QStringLiteral("width"), 1440.0);
            lecternBox.insert(QStringLiteral("height"), 700.0);
            lectern.insert(QStringLiteral("box"), lecternBox);
            check(docs.applyThemeContext(houseId, QStringLiteral("scripture"), lectern),
                  "House gains a lectern context for the Bible captures");
            for (const auto size : {QSize(1680, 1020), QSize(1380, 800), QSize(1000, 680)}) {
                window->resize(size);
                QTest::qWait(70);
                auto* songsTile = findNamed(window->contentItem(), "themeViewSongs");
                auto* bibleTile = findNamed(window->contentItem(), "themeViewBible");
                check(songsTile && songsTile->isVisible() && bibleTile
                          && bibleTile->isVisible(),
                      "the view tiles are up for the capture matrix");
                const QString tag = QString::number(size.width());
                window->grabWindow().save(capture + ".themes-" + tag + ".png");
                clickCaptureTile(bibleTile);
                QTest::qWait(80);
                window->grabWindow().save(capture + ".themes-bible-" + tag + ".png");
                clickCaptureTile(songsTile);
                QTest::qWait(70);
            }
            window->resize(QSize(1680, 1020));
            QTest::qWait(70);
        }
        // CS.9: the rest of the capture matrix — Bible import and Edit at
        // every ledger size, so all five named pages exist at 1000/1380/1680
        // (Show is the base/.1380/.1000 set above, Themes both views are
        // the .themes-* set above). Scale 1.5 and 2 come from re-running
        // this harness with QT_SCALE_FACTOR and a distinct prefix.
        window->setProperty("page", "bible");
        for (const auto size : {QSize(1680, 1020), QSize(1380, 800), QSize(1000, 680)}) {
            window->resize(size);
            QTest::qWait(80);
            window->grabWindow().save(capture + ".bible-" + QString::number(size.width())
                                      + QStringLiteral(".png"));
        }
        window->setProperty("page", "edit");
        for (const auto size : {QSize(1680, 1020), QSize(1380, 800), QSize(1000, 680)}) {
            window->resize(size);
            QTest::qWait(80);
            window->grabWindow().save(capture + ".edit-" + QString::number(size.width())
                                      + QStringLiteral(".png"));
        }
        window->resize(QSize(1680, 1020));
        QTest::qWait(70);
        window->setProperty("page", "settings");
        QTest::qWait(50);
        window->grabWindow().save(capture + ".settings.png");
        // The empty-collection state, last because removal is a document
        // edit: the bin with nothing in it must say what is missing rather
        // than show a void. The list is copied first — removeAsset mutates
        // the very model the loop would otherwise be walking.
        window->setProperty("page", "show");
        QTest::qWait(50);
        const QVariantList capturedAssets = docs.assets();
        for (const QVariant& asset : capturedAssets)
            docs.removeAsset(asset.toMap().value(QStringLiteral("id")).toString());
        QTest::qWait(80);
        window->grabWindow().save(capture + ".emptybin.png");
    }
    worker.shutdownNow();
    for (const auto& warning : warnings) {
        if (warning.contains("Shortcut: Only binding to one of multiple key bindings")) continue;
        std::fprintf(stderr, "QML: %s\n", qPrintable(warning));
        failed = true;
    }
    return failed ? 1 : 0;
}
