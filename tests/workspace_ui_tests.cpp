// Interaction regressions for the operator workspace. No worker or user files.
#include <QGuiApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImage>
#include <QMimeData>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QQuickItem>
#include <QJSValue>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>

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
    // Tasks SL/SI: the shared Library, the clipboard import session and the
    // service bridge are part of the shell the operator sees.
    openshow::ui::CatalogController library(nullptr, base);
    openshow::ui::SongImport songImport(&library, &docs, &worker);
    openshow::ui::ServiceCatalogBridge songService(&library, &docs, &worker);
    openshow::ui::ReflowPlanner reflow(&docs, &worker);
    openshow::ui::ThemeApplySession applySession(&docs, &library);
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
    context->setContextProperty("library", &library);
    context->setContextProperty("songImport", &songImport);
    context->setContextProperty("songService", &songService);
    context->setContextProperty("reflow", &reflow);
    context->setContextProperty("applySession", &applySession);
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
    check(showPageItem->property("cueRows").toList().isEmpty(),
          "the initial Songs folder has no preview until an item is selected");
    auto* initialServiceFolder = findNamed(window->contentItem(), "libraryServiceHeader");
    if (initialServiceFolder)
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          initialServiceFolder->mapToScene(QPointF(40, 13)).toPoint());
    QTest::qWait(100);
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
                      == firstRow.value(QStringLiteral("preview")).toString()
                      && firstTile->property("displayCaption").toString()
                             .startsWith(QStringLiteral("1")),
                  "the cue card wears its row's own words behind the slide number");
            check(!firstTile->property("displayCaption").toString()
                       .contains(firstRow.value(QStringLiteral("showName")).toString()),
                  "the cue card no longer repeats the service item's name");
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

    // --- tasks SL.4/SL.5: the folder strip over the chosen folder --------
    auto* browserPane = findNamed(window->contentItem(), "libraryBrowser");
    auto* libraryList = findNamed(window->contentItem(), "collectionLibraryList");
    auto* playlistList = findNamed(window->contentItem(), "collectionPlaylistList");
    auto* songList = findNamed(window->contentItem(), "librarySongList");
    auto* songsFolder = findNamed(window->contentItem(), "librarySongsHeader");
    auto* presentationsFolder =
        findNamed(window->contentItem(), "libraryPresentationsHeader");
    auto* serviceFolder = findNamed(window->contentItem(), "libraryServiceHeader");
    check(browserPane && libraryList && playlistList && songList,
          "the sidebar hosts the shared Library and the current service");
    check(songsFolder && presentationsFolder && serviceFolder,
          "the top strip lists the folders rather than their files");
    auto openFolder = [window](QQuickItem* folder) {
        if (!folder) return;
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          folder->mapToScene(QPointF(40, 13)).toPoint());
        QTest::qWait(60);
    };
    openFolder(songsFolder);
    check(songList->isVisible() && !libraryList->isVisible()
              && !playlistList->isVisible(),
          "opening Songs lists its files below and nothing else");
    check(songList->property("count").toInt() == library.songCount(),
          "the Songs folder lists the shared catalog");
    openFolder(serviceFolder);
    check(playlistList->isVisible() && !songList->isVisible()
              && playlistList->property("count").toInt() ==
                     static_cast<int>(docs.playlistEntries().size()),
          "opening Service lists the running order's cues in the same pane");
    openFolder(presentationsFolder);
    check(libraryList->isVisible() && !playlistList->isVisible()
              && libraryList->property("count").toInt() ==
                     static_cast<int>(docs.songs().size()),
          "opening Presentations lists its files in that same pane");
    openFolder(songsFolder);
    check(songList->isVisible() && !playlistList->isVisible(),
          "and clicking Songs again brings its files back");
    // The strip collapses so the open folder's files get its room, and comes
    // back at its own height.
    auto* foldersToggle = findNamed(window->contentItem(), "libraryFoldersToggle");
    check(foldersToggle != nullptr, "the folder strip has a collapse control");
    if (foldersToggle) {
        const qreal listBefore = songList->height();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          foldersToggle->mapToScene(QPointF(11, 11)).toPoint());
        QTest::qWait(80);
        check(!songsFolder->isVisible() && !serviceFolder->isVisible()
                  && songList->height() > listBefore,
              "collapsing the folders hands their room to the open folder's files");
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          foldersToggle->mapToScene(QPointF(11, 11)).toPoint());
        QTest::qWait(80);
        check(songsFolder->isVisible() && serviceFolder->isVisible()
                  && qAbs(songList->height() - listBefore) < 1.0,
              "the folders come back at their own height");
    }

    // Title-only filtering (SL.5): a catalog song whose LYRICS contain the
    // word must not match; presentation titles and song titles both do.
    {
        QVariantList stanzas;
        QVariantMap stanza;
        stanza.insert("label", "Verse 1");
        stanza.insert("lines", QStringList{"Jesus is the reason we sing"});
        stanzas.append(stanza);
        const QString lyricOnly = library.addSong("Morning Song", stanzas, "", "", "");
        check(!lyricOnly.isEmpty(), "a catalog song is added for the filter");
        QVariantList jesusStanzas;
        QVariantMap jesusStanza;
        jesusStanza.insert("label", "Verse 1");
        jesusStanza.insert("lines", QStringList{"Jesus loves me"});
        jesusStanzas.append(jesusStanza);
        library.addSong("Jesus Loves Me", jesusStanzas, "", "", "");
        check(docs.createSong("Jesus Presentation"),
              "a presentation title is added for the filter");
        QTest::qWait(40);
        libraryList->setProperty("contentY", 0);
        songList->setProperty("contentY", 0);
        auto* search = findNamed(window->contentItem(), "librarySearchField");
        check(search != nullptr, "the Library owns the one title search");
        search->setProperty("text", "jesus");
        QTest::qWait(40);
        check(songList->property("count").toInt() == 1,
              "the song filter matches the TITLE only, not lyrics");
        check(libraryList->property("count").toInt() >= 1,
              "presentation titles match the same query");
        search->setProperty("text", "");
        QTest::qWait(40);
        check(songList->property("count").toInt() == library.songCount(),
              "clearing the query restores the whole Songs branch");
        check(libraryList->property("count").toInt() ==
                  static_cast<int>(docs.songs().size()),
              "clearing restores every presentation");
        check(songList->property("count").toInt() >= 1,
              "both filter additions remain in the Library");
    }

    // SL.7: a song row really drags between the two category headers, with
    // the stable catalog id surviving both moves.
    {
        QVariantList stanzas;
        QVariantMap stanza;
        stanza.insert("label", "Verse 1");
        stanza.insert("lines", QStringList{"drag me between categories"});
        stanzas.append(stanza);
        const QString dragId = library.addSong("Drag Me", stanzas, "", "", "");
        check(!dragId.isEmpty(), "a song exists for the category drag");
        QTest::qWait(40);
        check(songsFolder && presentationsFolder,
              "both folders are present as drop targets");
        const auto rowById = [&](QQuickItem* list, const QString& id) -> QQuickItem* {
            const auto property = browserPane->property(list == songList ? "songRows" : "presentationRows");
            const auto model = property.canConvert<QJSValue>()
                ? property.value<QJSValue>().toVariant().toList() : property.toList();
            for (int i = 0; i < model.size(); ++i) {
                if (model[i].toMap().value("id").toString() == id) {
                    QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, i), Q_ARG(int, 1));
                    QTest::qWait(30);
                    break;
                }
            }
            QList<QQuickItem*> rows;
            collectNamed(list, list == songList ? "librarySongRow" : "collectionLibraryRow",
                         rows);
            for (auto* row : rows) {
                const QVariantMap data = row->property("modelData").toMap();
                if (data.value("id").toString() == id)
                    return row;
            }
            return nullptr;
        };
        openFolder(songsFolder);
        QQuickItem* row = rowById(songList, dragId);
        check(row != nullptr, "the new song is in the Songs folder");
        if (row && presentationsFolder) {
            const auto start = row->mapToScene(QPointF(25, 14)).toPoint();
            const auto target = presentationsFolder->mapToScene(
                QPointF(40, 13)).toPoint();
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, start + QPoint(60, 0), 15);
            QTest::mouseMove(window, target + QPoint(20, 0), 15);
            QTest::mouseMove(window, target, 15);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
            QTest::qWait(60);
            check(library.itemOf(dragId).value("category").toString()
                      == "presentations",
                  "dragging to Presentations moves the classification");
            check(library.itemOf(dragId).value("contentRevision").toULongLong() == 1,
                  "a category move keeps the content revision");
        }
        // It is now listed in the Presentations folder and drags back.
        QTest::qWait(30);
        openFolder(presentationsFolder);
        QQuickItem* backRow = rowById(libraryList, dragId);
        check(backRow != nullptr &&
                  backRow->property("modelData").toMap().value("rowKind").toString()
                      == "catalog",
              "the moved song is listed in Presentations as a Library row");
        if (backRow && songsFolder) {
            const auto start = backRow->mapToScene(QPointF(25, 14)).toPoint();
            const auto target = songsFolder->mapToScene(QPointF(40, 13)).toPoint();
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, start + QPoint(60, 0), 15);
            QTest::mouseMove(window, target + QPoint(20, 0), 15);
            QTest::mouseMove(window, target, 15);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
            QTest::qWait(60);
            check(library.itemOf(dragId).value("category").toString() == "songs",
                  "dragging back to Songs restores the classification");
        }
        library.removeItem(dragId);
        QTest::qWait(30);
    }

    // Long presentation names stay one bounded, selectable row.
    const QString longName(180, 'x');
    check(docs.createSong(longName), "a long-named presentation is created");
    openFolder(presentationsFolder);
    QMetaObject::invokeMethod(libraryList, "positionViewAtEnd");
    QTest::qWait(20);
    {
        QList<QQuickItem*> rows;
        collectNamed(libraryList, "collectionLibraryRow", rows);
        QQuickItem* longRow = nullptr;
        for (auto* row : rows)
            if (row->property("modelData").toMap().value("name").toString() == longName)
                longRow = row;
        check(longRow && longRow->width() <= libraryList->width() && longRow->height() > 0,
              "a long name stays one bounded row in the presentation list");
        if (longRow) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              longRow->mapToScene(QPointF(20, 14)).toPoint());
            QTest::qWait(20);
        }
        check(!docs.currentSongId().isEmpty(), "and the long row is selectable");
    }
    // Dragging a library row into blank space must not displace it.
    {
        QList<QQuickItem*> libraryRows;
        collectNamed(libraryList, "collectionLibraryRow", libraryRows);
        check(!libraryRows.isEmpty(), "the presentation list instantiates rows");
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
    // Browsing a Library presentation re-sources the Show page; these UI.2
    // checks are about the running order, so hand the centre back to it.
    window->setProperty("browsedShowId", QString());
    QTest::qWait(20);
    check(docs.addShowToPlaylist(songId), "the order takes a second copy of the show");
    openFolder(serviceFolder);
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
            const int index = qRound(row->y() / 28.0);
            if (index < 0 || index >= static_cast<int>(entries.size()) ||
                row->property("rowEntryId").toString() !=
                    entries[index].toMap().value("entryId").toString())
                identities = false;
        }
        check(identities, "scrolled rows keep the identities of the cues they show");
        playlistList->setProperty("contentY", 0);
    }
    // Drag a presentation out of the Library onto the Service FOLDER:
    // preparation lands at the end of the running order, selection intact.
    {
        QString welcomeId;
        for (const auto& song : docs.songs())
            if (song.toMap().value("name").toString() == QLatin1String("Welcome"))
                welcomeId = song.toMap().value("id").toString();
        openFolder(presentationsFolder);
        QList<QQuickItem*> rows;
        collectNamed(libraryList, "collectionLibraryRow", rows);
        QQuickItem* welcomeRow = nullptr;
        for (auto* row : rows)
            if (row->property("modelData").toMap().value("id").toString() == welcomeId)
                welcomeRow = row;
        check(welcomeRow != nullptr, "the Welcome row is in the presentation list");
        if (welcomeRow) {
            const auto start = welcomeRow->mapToScene(QPointF(25, 14)).toPoint();
            const auto target = serviceFolder->mapToScene(QPointF(40, 13)).toPoint();
            const int before = static_cast<int>(docs.playlistEntries().size());
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, start + QPoint(60, 0), 15);
            QTest::mouseMove(window, (start + target) / 2 + QPoint(30, 0), 15);
            QTest::mouseMove(window, target + QPoint(20, 0), 15);
            QTest::mouseMove(window, target, 15);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
            QTest::qWait(40);
            const auto after = docs.playlistEntries();
            bool landed = false;
            for (const QVariant& entry : after)
                landed = landed || entry.toMap().value("showId").toString() == welcomeId
                                  && static_cast<int>(before) + 1 == after.size();
            check(static_cast<int>(after.size()) == before + 1 && landed,
                  "dragging a presentation onto the Service folder prepares it");
            check(docs.currentSongId() == songId,
                  "browsing and preparing never move the presented selection");
        }
        window->setProperty("browsedShowId", QString());
        openFolder(serviceFolder);
        QTest::qWait(20);
    }

    // Ctrl+F searches PRESENTATIONS, and the sidebar owns those now (UI.4a
    // moved the field there when the bottom of the window became the media
    // bin). The operator audit runs with the caret in it, and the caret is
    // what keeps the presenter's keys off: the tab strip that used to claim
    // Ctrl+1/2 for itself is gone, so nothing else has to step aside.
    window->setProperty("page", "show");
    QTest::qWait(30);
    // TC.7: the Show grid's rows carry the render fingerprint and owner the
    // thumbnail overlay needs, taken from the document's own compile. Without
    // them a warmed service could never be looked up and every row would cost
    // a worker render again.
    {
        auto* showPage = window->findChild<QQuickItem*>("showPage");
        check(showPage != nullptr, "the Show page is addressable");
        QVariant bandValue;
        const bool answered =
            showPage != nullptr &&
            QMetaObject::invokeMethod(showPage, "slideWindow", Q_RETURN_ARG(QVariant, bandValue));
        const QVariantList band = bandValue.toList();
        check(answered && !band.isEmpty(), "the visible Show band is non-empty");
        if (!band.isEmpty()) {
            const QVariantMap row = band.front().toMap();
            const QString slideId = row.value(QStringLiteral("key")).toString();
            const QVariantMap source = docs.thumbnailSourceForSlide(slideId);
            check(!slideId.isEmpty() && row.value(QStringLiteral("scene")).toString() ==
                                           source.value(QStringLiteral("scene")).toString(),
                  "a Show row carries the compiled scene it is cached under");
            check(!row.value(QStringLiteral("fingerprint")).toString().isEmpty() &&
                      row.value(QStringLiteral("fingerprint")).toString() ==
                          source.value(QStringLiteral("fingerprint")).toString(),
                  "a Show row carries the document's render fingerprint");
            check(!row.value(QStringLiteral("showId")).toString().isEmpty(),
                  "a Show row names the show that owns the picture");
        }
    }
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
    // Edit replaces the service/Library outline with thumbnails of only
    // the selected show. Preview clicks flush pending words and stay off air.
    {
        QTest::qWait(50);
        auto* rail = findNamed(window->contentItem(), "editorSlideRail");
        auto* list = findNamed(window->contentItem(), "editorSlideList");
        check(rail && list && rail->isVisible() && !left->isVisible(),
              "Edit shows a thumbnail rail instead of the Library/service pane");
        // TC.7: the Edit rail asks under the same identity contract as Show.
        if (auto* rail = window->findChild<QQuickItem*>("editorSlideRail")) {
            QVariant batchValue;
            const bool answered = QMetaObject::invokeMethod(rail, "thumbnailWindow",
                                                           Q_RETURN_ARG(QVariant, batchValue));
            const QVariantList batch = batchValue.toList();
            check(answered && !batch.isEmpty(), "the Edit rail names its visible window");
            if (!batch.isEmpty()) {
                const QVariantMap row = batch.front().toMap();
                const QString slideId = row.value(QStringLiteral("key")).toString();
                const QVariantMap source = docs.thumbnailSourceForSlide(slideId);
                check(!row.value(QStringLiteral("fingerprint")).toString().isEmpty() &&
                          row.value(QStringLiteral("fingerprint")).toString() ==
                              source.value(QStringLiteral("fingerprint")).toString(),
                      "an Edit row carries the document's render fingerprint");
                check(row.value(QStringLiteral("showId")).toString() == docs.currentSongId(),
                      "an Edit row names its owning show");
            }
        } else {
            check(false, "the Edit rail is addressable");
        }
        // EW.5's fallback, on this no-worker shell: the bottom field IS the
        // explicit source drawer and the canvas never offers in-place
        // editing without a worker that can answer its geometry.
        {
            auto* canvas = findNamed(window->contentItem(), "editCanvas");
            auto* source = findNamed(window->contentItem(), "slideTextEditor");
            auto* sourceAction = findNamed(window->contentItem(), "sourceTextButton");
            auto* editText = window->findChild<QObject*>("editTextMenuItem");
            check(source && source->isVisible(),
                  "without a worker the source drawer is the visible fallback");
            check(sourceAction &&
                      sourceAction->property("text").toString().contains(
                          QStringLiteral("required")) &&
                      !sourceAction->property("enabled").toBool(),
                  "the fallback names itself as required and cannot be closed");
            check(canvas && !canvas->property("textEditAvailable").toBool(),
                  "the canvas offers in-place editing only with a capable worker");
            check(editText && !editText->property("enabled").toBool(),
                  "the box menu's Edit text action follows the same capability gate");
        }

        auto* more = findNamed(window->contentItem(), "toolbarMore");
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          more->mapToScene(QPointF(20, 20)).toPoint());
        QTest::qWait(40);
        check(!findNamed(window->contentItem(), "newPresentationMenuItem")
                  && !findNamed(window->contentItem(), "libraryNewPresentationName")
                  && !window->findChild<QObject*>("libraryNewPresentationDialog"),
              "More has no manual presentation creation entry or backing dialog");
        check(findNamed(window->contentItem(), "songImportMenuItem")
                  && findNamed(window->contentItem(), "bibleImportMenuItem"),
              "clipboard song and Bible import remain available from More");
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(250);
        const auto originalSlides = docs.slides();
        const auto originalShow = docs.currentSongId();
        const int originalIndex = docs.currentSlide();
        check(list && list->property("count").toInt() == originalSlides.size(),
              "Edit lists only the selected presentation's slides");
        const int target = originalIndex == 2 ? 3 : 2;
        docs.setSlideText("Pending words before thumbnail navigation");
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, target), Q_ARG(int, 1));
        QTest::qWait(30);
        QList<QQuickItem*> tiles;
        collectNamed(list, "editorSlideTile", tiles);
        QQuickItem* tile = nullptr;
        for (auto* candidate : tiles) if (candidate->property("index").toInt() == target) tile = candidate;
        check(tile != nullptr, "an arbitrary slide thumbnail is reachable by scrolling");
        if (tile) QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                    tile->mapToScene(QPointF(20, 20)).toPoint());
        check(docs.currentSlide() == target && docs.currentSongId() == originalShow
                  && docs.slides()[originalIndex].toMap().value("preview")
                     == "Pending words before thumbnail navigation",
              "a thumbnail click selects that slide and commits text to the slide being left");
        const auto afterClick = docs.slides();
        QTest::keyClick(window, Qt::Key_Down);
        check(docs.currentSlide() == target + 1 && docs.slides() == afterClick,
              "Down in the preview rail navigates without nudging a box or changing slides");
        QTest::keyClick(window, Qt::Key_End);
        QTest::qWait(30);
        check(docs.currentSlide() == docs.slides().size() - 1 && list->property("contentY").toReal() > 0,
              "End selects and reveals the last slide in a long presentation");
        docs.undo();
        docs.selectSlide(originalIndex);
        check(docs.slides() == originalSlides, "preview navigation adds no history beyond the pending text edit");
        docs.createSong("Preview-only fixture");
        docs.addSlide();
        docs.addSlide();
        QTest::qWait(30);
        check(list->property("count").toInt() == 2, "switching presentations replaces the thumbnail model");
        auto* add = findNamed(window->contentItem(), "editorAddSlide");
        if (add) QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  add->mapToScene(QPointF(add->width() / 2, add->height() / 2)).toPoint());
        check(add && docs.slides().size() == 3 && docs.currentSlide() == 2,
              "Add slide remains reachable beside the thumbnail list");
        docs.undo();
        docs.undo();
        docs.undo();
        docs.undo();
        docs.selectSong(originalShow);
        docs.selectSlide(originalIndex);
        check(docs.slides() == originalSlides, "the preview fixture leaves the original show intact");
    }
    // --- EW.7/EW.8: real reorder drags, insertion gaps and insert verbs ---
    {
        auto* rail = findNamed(window->contentItem(), "editorSlideRail");
        auto* list = findNamed(window->contentItem(), "editorSlideList");
        const QString returnShow = docs.currentSongId();
        const int returnSlide = docs.currentSlide();
        check(rail && list, "the Edit rail and its lane are on the page");
        const auto waitFor = [](const std::function<bool()>& predicate, int budgetMs = 2000) {
            QElapsedTimer timer;
            timer.start();
            while (!predicate() && timer.elapsed() < budgetMs) QTest::qWait(20);
            return predicate();
        };
        auto railPreviews = [&]() {
            QStringList out;
            for (const QVariant& row : docs.slides())
                out << row.toMap().value(QStringLiteral("preview")).toString();
            return out;
        };
        auto tileAt = [&](int slot) -> QQuickItem* {
            QList<QQuickItem*> tiles;
            collectNamed(list, "editorSlideTile", tiles);
            for (auto* candidate : tiles)
                if (candidate->property("index").toInt() == slot) return candidate;
            return nullptr;
        };
        auto handleAt = [&](int slot) -> QQuickItem* {
            QList<QQuickItem*> handles;
            collectNamed(list, "editorSlideGrabHandle", handles);
            for (auto* candidate : handles)
                if (candidate->property("index").toInt() == slot) return candidate;
            return nullptr;
        };
        docs.createSong(QStringLiteral("Rail fixture"));
        const QString railShow = docs.currentSongId();
        for (int i = 1; i <= 5; ++i) {
            docs.addSlide();
            docs.setSlideText(QStringLiteral("Rail %1").arg(i));
            docs.flushText();
        }
        docs.selectSlide(0);
        window->setProperty("page", "edit");
        QTest::qWait(80);
        check(list->property("count").toInt() == 5,
              "the rail fixture shows five slides");
        const auto authored = railPreviews();
        check(authored == (QStringList{"Rail 1", "Rail 2", "Rail 3", "Rail 4", "Rail 5"}),
              "the rail fixture starts in authored order");

        // Pointer drag: grab slide 1's handle and drop in the gap after
        // slide 3. The ghost and the insertion gap are visible while the
        // grab is held; the visible gap translates to final slot 2.
        QQuickItem* sourceTile = tileAt(0);
        QQuickItem* handle = handleAt(0);
        check(sourceTile && handle, "every card carries a visible reorder handle");
        if (sourceTile && handle) {
            const QPoint start = handle->mapToScene(QPointF(handle->width() / 2,
                                                            handle->height() / 2)).toPoint();
            QQuickItem* thirdTile = tileAt(2);
            const QPoint target = thirdTile
                ? thirdTile->mapToScene(QPointF(thirdTile->width() / 2,
                                                thirdTile->height() - 4)).toPoint()
                : start + QPoint(0, 120);
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
            QTest::mouseMove(window, start + QPoint(0, 18), 15);
            QTest::mouseMove(window, target, 20);
            check(rail->property("dragActive").toBool(),
                  "a held grab is a live reorder gesture");
            auto* ghost = findNamed(window->contentItem(), "editorSlideGhost");
            auto* gap = findNamed(window->contentItem(), "editorSlideInsertionGap");
            check(ghost && ghost->isVisible() && gap && gap->isVisible(),
                  "the drag shows a ghost and an insertion gap");
            const int shownGap = rail->property("dragGap").toInt();
            check(shownGap == 3, "the gap follows the pointer between cards");
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
            check(railPreviews() ==
                      (QStringList{"Rail 2", "Rail 3", "Rail 1", "Rail 4", "Rail 5"}),
                  "dropping slide 1 in the third gap moves it once to final slot 2");
            check(docs.currentSlide() == 2 &&
                      docs.slides().at(2).toMap().value("preview").toString()
                          == QStringLiteral("Rail 1"),
                  "the moved slide stays selected by id");
            check(waitFor([&]() { return !rail->property("dragActive").toBool(); }),
                  "the gesture ends on release");
            docs.undo();
            check(railPreviews() == authored, "one undo restores the authored order");
            docs.redo();
        }

        // Upward drops use the captured source slot, and the gap before
        // the source is a no-op rather than an accidental one-slot move.
        {
            const auto before = railPreviews();
            auto* grab = handleAt(2);
            auto* destination = tileAt(1);
            check(grab && destination, "an upward rail drop is reachable");
            if (grab && destination) {
                const QPoint start = grab->mapToScene(QPointF(grab->width() / 2,
                                                              grab->height() / 2)).toPoint();
                const QPoint end = destination->mapToScene(QPointF(30, 1)).toPoint();
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
                QTest::mouseMove(window, end, 20);
                check(rail->property("dragGap").toInt() == 1,
                      "the upward drop previews the gap before the second slide");
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
                auto expected = before;
                expected.insert(1, expected.takeAt(2));
                check(railPreviews() == expected,
                      "an upward drop lands at its displayed gap without subtracting twice");
                docs.undo();
                check(railPreviews() == before, "one undo restores the upward move");
            }
            grab = handleAt(2);
            auto* source = tileAt(2);
            check(grab && source, "the source's preceding gap is reachable");
            if (grab && source) {
                const QPoint start = grab->mapToScene(QPointF(grab->width() / 2,
                                                              grab->height() / 2)).toPoint();
                const QPoint end = source->mapToScene(QPointF(30, 1)).toPoint();
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
                QTest::mouseMove(window, end, 20);
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
                check(railPreviews() == before,
                      "dropping in the source's preceding gap leaves the order unchanged");
            }
        }

        // Escape cancels a held gesture: no order change and no history.
        {
            const auto beforeCancel = railPreviews();
            QQuickItem* grab = handleAt(0);
            if (grab) {
                const QPoint start = grab->mapToScene(QPointF(grab->width() / 2,
                                                              grab->height() / 2)).toPoint();
                rail->forceActiveFocus();
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
                QTest::mouseMove(window, start + QPoint(0, 90), 20);
                QTest::keyClick(window, Qt::Key_Escape);
                QTest::qWait(20);
                check(!rail->property("dragActive").toBool(),
                      "Escape retires a held reorder");
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                    start + QPoint(0, 90));
                check(railPreviews() == beforeCancel,
                      "a cancelled drag changes no slide order");
                // The cancelled gesture added no history: one undo still
                // takes away the last real move (the redo above).
                docs.undo();
                check(railPreviews() == authored,
                      "a cancelled drag records nothing - the previous edit is what undo takes");
                docs.redo();
            }
        }

        // A release outside the lane and a drop on the source's own gap are
        // both no-ops: true for the controller, unchanged for the model.
        {
            const auto beforeNoop = railPreviews();
            QQuickItem* grab = handleAt(0);
            if (grab) {
                const QPoint start = grab->mapToScene(QPointF(grab->width() / 2,
                                                              grab->height() / 2)).toPoint();
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
                QTest::mouseMove(window, start + QPoint(0, 60), 20);
                const QPoint outside = rail->mapToScene(QPointF(rail->width() / 2,
                                                                rail->height() - 4)).toPoint();
                QTest::mouseMove(window, outside, 20);
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, outside);
                check(railPreviews() == beforeNoop,
                      "a release outside the lanes cancels without moving");
                QQuickItem* grabAgain = handleAt(0);
                if (grabAgain) {
                    const QPoint pressPoint = grabAgain->mapToScene(
                        QPointF(grabAgain->width() / 2, grabAgain->height() / 2)).toPoint();
                    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, pressPoint);
                    QTest::mouseMove(window, pressPoint + QPoint(0, 4), 15);
                    QTest::mouseMove(window, pressPoint, 15);
                    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, pressPoint);
                }
                check(railPreviews() == beforeNoop,
                      "a drop on the source's own gap changes nothing");
                docs.undo();
                check(railPreviews() == authored,
                      "the no-op drop added no history");
                docs.redo();
            }
        }

        // Context menu: Move up is the pointer-free alternative, and Insert
        // before/after land exactly where the row menu says.
        {
            const auto beforeMenu = railPreviews();
            QQuickItem* third = tileAt(2);
            check(third != nullptr, "a middle card is reachable for its menu");
            if (third) {
                QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                                  third->mapToScene(QPointF(20, 20)).toPoint());
                QTest::qWait(60);
                QObject* menu = window->findChild<QObject*>("editorSlideMenu");
                QObject* up = window->findChild<QObject*>("editorMoveUpMenuItem");
                QObject* down = window->findChild<QObject*>("editorMoveDownMenuItem");
                check(menu && menu->property("visible").toBool(),
                      "right-click opens the slide's row menu");
                check(up && up->property("enabled").toBool() && down
                          && down->property("enabled").toBool(),
                      "a middle slide can move either way");
                if (up) QMetaObject::invokeMethod(up, "clicked");
                QTest::qWait(40);
                QStringList moved = beforeMenu;
                moved.move(2, 1);
                check(railPreviews() == moved,
                      "Move up lands the slide one slot earlier");
                docs.undo();
                check(railPreviews() == beforeMenu, "one undo restores the menu move");
                // Insert before/after through the same menu. The delegate may
                // have been rebuilt by the move/undo, so the row is located
                // again rather than reused.
                QQuickItem* insertRow = tileAt(2);
                if (insertRow)
                    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                                      insertRow->mapToScene(QPointF(20, 20)).toPoint());
                QTest::qWait(60);
                QObject* insertBefore =
                    window->findChild<QObject*>("editorInsertBeforeMenuItem");
                check(insertBefore && insertBefore->property("enabled").toBool(),
                      "the row menu offers Insert before");
                const int countBeforeInsert = docs.slides().size();
                if (insertBefore)
                    QMetaObject::invokeMethod(insertBefore, "clicked");
                QTest::qWait(80);
                check(docs.slides().size() == countBeforeInsert + 1 &&
                          docs.currentSlide() == 2,
                      "Insert before lands at the named row and selects the fresh slide");
                check(docs.currentSlideText().isEmpty(),
                      "the fresh slide carries empty words");
                docs.undo();
                check(docs.slides().size() == countBeforeInsert,
                      "one undo removes the menu insertion");
            }
        }

        // Keyboard alternatives while the rail owns focus: Ctrl+Down moves
        // the selected slide, plain Down still only navigates.
        {
            docs.selectSlide(0);
            rail->forceActiveFocus();
            QTest::qWait(20);
            const auto beforeKeys = railPreviews();
            QTest::keyClick(window, Qt::Key_Down, Qt::ControlModifier);
            check(railPreviews() ==
                      (QStringList{beforeKeys[1], beforeKeys[0], beforeKeys[2],
                                   beforeKeys[3], beforeKeys[4]}),
                  "Ctrl+Down moves the selected slide one slot");
            check(docs.currentSlide() == 1,
                  "the moved selection follows the slide");
            QTest::keyClick(window, Qt::Key_Down);
            check(docs.currentSlide() == 2, "plain Down still navigates only");
            docs.undo();
            check(railPreviews() == beforeKeys,
                  "the keyboard move is one edit and the navigation none");
        }

        // Footer insertion: after the selection, selected and revealed, one
        // undo. The empty presentation inserts at position zero.
        {
            docs.selectSlide(1);
            QTest::qWait(20);
            auto* insert = findNamed(window->contentItem(), "editorAddSlide");
            check(insert && insert->property("text").toString() == QStringLiteral("Insert slide"),
                  "the rail footer offers Insert slide");
            const int beforeInsert = docs.slides().size();
            if (insert)
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  insert->mapToScene(QPointF(insert->width() / 2,
                                                             insert->height() / 2)).toPoint());
            QTest::qWait(80);
            check(docs.slides().size() == beforeInsert + 1 && docs.currentSlide() == 2,
                  "Insert slide lands after the selected slide");
            const QVariantList freshBoxes = docs.currentSlideBoxes();
            check(freshBoxes.size() == 1 &&
                      freshBoxes.front().toMap().value("kind").toString()
                          == QStringLiteral("text") &&
                      docs.currentSlideText().isEmpty(),
                  "the fresh slide is one empty editable text box");
            docs.undo();
            check(docs.slides().size() == beforeInsert,
                  "one undo removes the footer insertion");

            docs.createSong(QStringLiteral("Rail empty fixture"));
            docs.flushText();
            window->setProperty("page", "show");
            QTest::qWait(20);
            window->setProperty("page", "edit");
            QTest::qWait(60);
            auto* emptyInsert = findNamed(window->contentItem(), "editorAddSlide");
            check(docs.slides().isEmpty() && list->property("count").toInt() == 0,
                  "an empty presentation shows an empty rail");
            if (emptyInsert)
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  emptyInsert->mapToScene(QPointF(emptyInsert->width() / 2,
                                                                  emptyInsert->height() / 2)).toPoint());
            QTest::qWait(80);
            check(docs.slides().size() == 1 && docs.currentSlide() == 0,
                  "the empty presentation's Insert slide lands at position zero");
            docs.undo();
        }

        // Bounded autoscroll during a held grab, and a model change during
        // the drag retires the gesture instead of retargeting a slot.
        {
            docs.createSong(QStringLiteral("Rail long fixture"));
            for (int i = 1; i <= 14; ++i) {
                docs.addSlide();
                docs.setSlideText(QStringLiteral("Long %1").arg(i));
                docs.flushText();
            }
            docs.selectSlide(0);
            window->setProperty("page", "show");
            QTest::qWait(20);
            window->setProperty("page", "edit");
            QTest::qWait(80);
            QMetaObject::invokeMethod(list, "positionViewAtBeginning");
            QTest::qWait(40);
            QQuickItem* grab = handleAt(0);
            if (grab) {
                const QPoint press = grab->mapToScene(
                    QPointF(grab->width() / 2, grab->height() / 2)).toPoint();
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, press);
                QTest::mouseMove(window, press + QPoint(0, 10), 15);
                const QPoint bottomEdge = list->mapToScene(
                    QPointF(list->width() / 2, list->height() - 4)).toPoint();
                const qreal beforeScroll = list->property("contentY").toReal();
                QTest::mouseMove(window, bottomEdge, 20);
                check(waitFor([&]() {
                          return list->property("contentY").toReal() > beforeScroll + 8;
                      }, 1200),
                      "holding a grab at the lane's edge autoscrolls");
                // The rows move under the pointer; the captured source is
                // still the authored identity.
                check(rail->property("dragSlideId").toString()
                          == docs.slides().at(0).toMap().value("id").toString(),
                      "the gesture keeps the captured source identity");
                // A model change during the held grab cancels it.
                const int longCount = docs.slides().size();
                docs.addSlide();
                QTest::qWait(30);
                check(!rail->property("dragActive").toBool(),
                      "a slide added mid-drag retires the reorder");
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, bottomEdge);
                check(docs.slides().size() == longCount + 1,
                      "the cancelled gesture lands no structural move of its own");
                docs.undo();
            }
        }

        docs.selectSong(returnShow);
        docs.selectSlide(returnSlide);
        window->setProperty("page", "edit");
        QTest::qWait(40);
    }
    // Edit uses native text Delete while typing and one undoable slide
    // removal elsewhere. The same key remains inactive on Show.
    {
        const auto beforeSlides = docs.slides();
        const auto beforeService = docs.playlistEntries();
        const auto oldShow = docs.currentSongId();
        const int oldSlide = docs.currentSlide();
        QTest::qWait(30);
        window->contentItem()->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Delete);
        check(docs.slides().size() == beforeSlides.size() - 1
                  && docs.playlistEntries().size() == beforeService.size(),
              "Delete removes the selected slide without removing its service entry");
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        check(docs.slides() == beforeSlides, "one keyboard undo restores the exact deleted slide");
        editor->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const char c : std::string("Editable words")) QTest::keyClick(window, c);
        QTest::keyClick(window, Qt::Key_Home);
        QTest::keyClick(window, Qt::Key_Delete);
        check(editor->property("text").toString() == "ditable words"
                  && docs.slides().size() == beforeSlides.size(),
              "Delete while typing edits text and never deletes a slide");
        docs.flushText();
        window->contentItem()->forceActiveFocus();
        docs.undo();
        window->setProperty("page", "show");
        QTest::qWait(30);
        QTest::keyClick(window, Qt::Key_Delete);
        check(docs.slides() == beforeSlides, "Delete outside an editing/list context does not remove a slide");
        window->setProperty("page", "edit");
        QTest::qWait(30);
        docs.createSong("Temporary keyboard deletion fixture");
        docs.addSlide();
        const auto onlySlide = docs.slides();
        window->contentItem()->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Delete);
        check(docs.slides().isEmpty() && docs.currentSlide() == -1, "Delete can remove the final slide");
        QTest::keyClick(window, Qt::Key_Delete);
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        check(docs.slides() == onlySlide, "Delete with no slides is harmless and preserves the last undo");
        docs.undo();  // temporary slide creation
        docs.undo();  // temporary show creation
        docs.selectSong(oldShow);
        docs.selectSlide(oldSlide);
        check(docs.slides() == beforeSlides && docs.playlistEntries() == beforeService,
              "the deletion journey leaves the original presentation and service intact");
    }
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

        // MB.7: a rendered deck page is stored but never a bin row or count.
        {
            const QVariantList beforeDeck = docs.assets();
            const QString deckPage = docs.addDeckPageAsset(
                QStringLiteral("deck - 01.png"),
                files + QStringLiteral("/deck-page.png"));
            QTest::qWait(40);
            check(!deckPage.isEmpty() && docs.assets() == beforeDeck
                      && docs.storedAssets().size() == beforeDeck.size() + 1,
                  "a deck page is stored without joining the bin");
            check(mediaRows().size() == beforeDeck.size()
                      && collectionRows().first()->property("modelData").toMap()
                             .value(QStringLiteral("count")).toInt()
                         == beforeDeck.size(),
                  "a deck page adds no card and no collection count");
        }

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
        // Every row says where it goes: the video and camera to Edit, audio
        // to a service cue, and "other" nowhere at all. No row carries a
        // Play control.
        check(noticeRows == 3 && notices.size() == 3 && notices.at(0).contains("background")
                  && notices.at(1).contains("audio cue")
                  && notices.at(2).contains("nothing presents") && noticesWhole,
              "each row says where its file goes, in full, instead of hiding");

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
        check(notice("audio", true, false, false).startsWith(QLatin1String("audio cue"))
                  && notice("audio", true, false, true).startsWith(QLatin1String("audio cue")),
              "an audio row always says its route: a cue in a service");

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

        // Audio has no Play button: its route to the audience is a service
        // cue like every other kind, and the transport rail governs what is
        // on air. The info column names that route and the missing output.
        auto* statusLine = findNamed(mediaBin, "mediaInfoStatus");
        check(findNamed(window->contentItem(), "mediaActivateButton") == nullptr,
              "an audio row offers no Play button");
        check(statusLine != nullptr
                  && statusLine->property("text").toString().contains("audio cue"),
              "the info column names the row's route: a cue in a service");
        check(worker.standalonePath().isEmpty() && !worker.standaloneOnAir()
                  && worker.standaloneRefusal().isEmpty()
                  && worker.standaloneRefusalPath().isEmpty(),
              "and a bin with no output has said nothing about any track");

        // Back to all media: the two document actions, both Edit-only and both
        // honest about the kinds they cover.
        QList<QQuickItem*> allFirst = collectionRows();
        if (!allFirst.isEmpty()) clickAt(allFirst.first(), 20, 12);
        QTest::qWait(40);

        // MB.2: a card is a drag source carrying the asset id, not a verb.
        {
            QString loopId;
            for (const QVariant& entry : docs.assets())
                if (entry.toMap().value("name").toString() == QLatin1String("loop.mp4"))
                    loopId = entry.toMap().value("id").toString();
            QQuickItem* loopCard = nullptr;
            for (auto* row : mediaRows())
                if (row->property("modelData").toMap().value("name").toString()
                    == QLatin1String("loop.mp4"))
                    loopCard = row;
            check(!loopId.isEmpty() && loopCard != nullptr
                      && loopCard->property("dragPayload").toString()
                             == "media:" + loopId,
                  "a media card drags the asset id, not a document verb");
        }
        // MB.4: the document actions live in the card's context menu, not in
        // the body. Open it and read what it offers for the selection.
        auto openMenu = [&]() -> QObject* {
            auto* object = window->findChild<QObject*>("mediaContextMenu");
            if (!object) return nullptr;
            if (!object->property("visible").toBool()) {
                QMetaObject::invokeMethod(object, "open");
                for (int i = 0; i < 30 && !object->property("visible").toBool(); ++i)
                    QTest::qWait(10);
            }
            QTest::qWait(30);
            return object;
        };
        auto menuItem = [&](QObject* object, const char* name) -> QQuickItem* {
            return object ? object->findChild<QQuickItem*>(name) : nullptr;
        };
        auto clickMenu = [&](QObject* object, const char* name) {
            auto* item = menuItem(object, name);
            return item != nullptr && item->isVisible()
                   && QMetaObject::invokeMethod(item, "click");
        };
        auto closeMenu = [&](QObject* object) {
            if (object) {
                QMetaObject::invokeMethod(object, "close");
                for (int i = 0; i < 30 && object->property("visible").toBool(); ++i)
                    QTest::qWait(10);
            }
        };
        {
            QObject* object = openMenu();
            auto* placeAction = menuItem(object, "placeAssetAction");
            auto* addNdi = menuItem(object, "mediaAddNdiAction");
            check(menuItem(object, "mediaRemoveAction") == nullptr,
                  "the context menu leaves removal to the Delete key");
            check(placeAction == nullptr || !placeAction->isVisible(),
                  "Add to slide is absent for a non-picture");
            // An inapplicable row must not be a blank slot: with no live
            // worker this fixture hides Add NDI too, and both must measure 0.
            bool noBlank = placeAction == nullptr || placeAction->height() == 0.0;
            if (addNdi != nullptr && !addNdi->isVisible())
                noBlank = noBlank && addNdi->height() == 0.0;
            check(noBlank, "an inapplicable menu item leaves no blank row");
            // With only Import… applicable here, the popup is one row, not
            // the three-row box the blank entries used to reserve.
            check(object->property("height").toDouble() < 100.0,
                  "the menu is only as tall as the items it offers");
            closeMenu(object);
        }

        // The kinds with no runtime to fire are offered no cue at all: a
        // greyed-out Play would read as something wrong with the output, where
        // the truth is that this file has no cue to fire — which is what the
        // notice column and the info line say instead.
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("name").toString()
                == QLatin1String("loop.mp4"))
                clickAt(row, 20, 14);
        QTest::qWait(30);
        check(findNamed(window->contentItem(), "mediaActivateButton") == nullptr
                  && statusLine->property("text").toString().contains("background"),
              "a video row is not offered a cue either, and is told where it does go");

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
        check(statusLine->property("text").toString().contains("not where the service says"),
              "an audio row whose file is gone keeps saying so, with no cue control");
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("name").toString()
                == QLatin1String("notes.txt"))
                clickAt(row, 20, 14);
        QTest::qWait(30);
        {
            QObject* object = openMenu();
            auto* placeAction = menuItem(object, "placeAssetAction");
            check(placeAction == nullptr || !placeAction->isVisible(),
                  "Add to slide stays off for a file nothing places on a slide");
            check(menuItem(object, "mediaRemoveAction") == nullptr,
                  "the menu has no second removal door");
            closeMenu(object);
        }
        QTest::qWait(40);
        mediaBin->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Delete);
        QTest::qWait(40);
        const QVariantList afterRemove = docs.assets();
        QStringList remainingNames;
        for (const QVariant& entry : afterRemove)
            remainingNames << entry.toMap().value("name").toString();
        check(!remainingNames.contains(QLatin1String("notes.txt"))
                  && remainingNames.size() == 3,
              "Delete takes the card out of the media library");

        // A picture is the one kind a bin card can join today; Add to slide
        // stays in the menu for the foreground item (the background is the
        // drop, MB.3).
        // A real image, not the "BYTES" placeholder: the bin's card now
        // renders the picture, and a fake file would make the QML image
        // loader warn (the harness treats a QML warning as a failure).
        {
            QImage cross(8, 8, QImage::Format_ARGB32);
            cross.fill(Qt::red);
            check(cross.save(files + "/cross.png"), "the picture fixture is written");
        }
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
        {
            QObject* object = openMenu();
            check(clickMenu(object, "placeAssetAction"),
                  "the picture makes Add to slide live");
            closeMenu(object);
        }
        QTest::qWait(40);
        const QVariantList boxes = docs.currentSlideBoxes();
        check(boxes.size() == boxesBefore + 1
                  && boxes.last().toMap().value("kind").toString() == "image"
                  && boxes.last().toMap().value("assetId").toString() == pictureId,
              "Add to slide places the picture the operator had selected");
        check(worker.pendingCueKey().isEmpty() && !worker.outputLive(),
              "and placing it still presents nothing");
        // MB.3: a media card dropped on a cue sets that cue's slide background,
        // whichever show owns the cue, in one undo. The actual drag event is
        // Qt's; this drives the routing function the DropArea calls.
        {
            const QVariantList cues = showPageItem->property("cueRows").toList();
            check(!cues.isEmpty(), "the Show page has a cue to drop onto");
            if (!cues.isEmpty() && !pictureId.isEmpty()) {
                const QVariantMap cue = cues.first().toMap();
                const QString cueShow = cue.value("showId").toString();
                const QString cueSlide = cue.value("slideId").toString();
                auto backgroundId = [&docs](const QString& slideId) {
                    const QJsonObject scene =
                        QJsonDocument::fromJson(
                            docs.sceneJsonForSlide(slideId).toUtf8()).object();
                    return scene.value("background").toObject()
                                .value("assetId").toString();
                };
                const QString before = backgroundId(cueSlide);
                QVariant applied;
                const bool called = QMetaObject::invokeMethod(
                    showPageItem, "applyMediaToCue", Q_RETURN_ARG(QVariant, applied),
                    Q_ARG(QVariant, cueShow), Q_ARG(QVariant, cueSlide),
                    Q_ARG(QVariant, pictureId), Q_ARG(QVariant, QStringLiteral("cover")));
                QTest::qWait(40);
                check(called && applied.toBool() && backgroundId(cueSlide) == pictureId,
                      "a dropped picture becomes the cue's background");
                docs.undo();
                QTest::qWait(40);
                check(backgroundId(cueSlide) == before,
                      "one undo restores the cue's previous background");
                docs.redo();
                QTest::qWait(40);
            }
            auto* drop = findNamed(showPageItem, "cueMediaDrop");
            check(drop != nullptr
                      && drop->property("keys").toStringList().contains(
                             QStringLiteral("text/plain")),
                  "a cue tile accepts the media drag payload");
            auto* binDrop = findNamed(mediaBin, "mediaBinDrop");
            check(binDrop != nullptr
                      && binDrop->property("keys").toStringList().contains(
                             QStringLiteral("text/uri-list")),
                  "the Media Bin accepts files dropped from the desktop");
        }

        // MB.3's drop is the bin's OWN payload across Qt's drag, not just the
        // routing function beside it: a card that carries the right text must
        // also be understood by the cue tile's DropArea when the operator
        // actually drags it there.
        {
            docs.undo();  // the routing check above left the first cue wearing it
            window->setProperty("page", "show");
            QTest::qWait(80);
            auto backgroundId = [&docs](const QString& slideId) {
                const QJsonObject scene =
                    QJsonDocument::fromJson(docs.sceneJsonForSlide(slideId).toUtf8())
                        .object();
                return scene.value("background").toObject()
                            .value("assetId").toString();
            };
            QQuickItem* card = nullptr;
            for (auto* row : mediaRows())
                if (row->property("modelData").toMap().value("id").toString()
                    == pictureId)
                    card = row;
            QList<QQuickItem*> tiles;
            collectNamed(showPageItem, "operatorCueTile", tiles);
            QQuickItem* tile = nullptr;
            for (auto* candidate : tiles) {
                const QString slideId = candidate->property("modelData").toMap()
                                            .value("slideId").toString();
                if (candidate->isVisible() && !slideId.isEmpty()) {
                    tile = candidate;
                    break;
                }
            }
            check(card != nullptr && tile != nullptr,
                  "the bin card and a cue tile are both on screen");
            if (card && tile) {
                const QString slideId = tile->property("modelData").toMap()
                                            .value("slideId").toString();
                const QString before = backgroundId(slideId);
                const QPoint start = card->mapToScene(QPointF(20, 20)).toPoint();
                const QPoint target =
                    tile->mapToScene(QPointF(tile->width() / 2, tile->height() / 2))
                        .toPoint();
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
                QTest::mouseMove(window, start + QPoint(40, -20), 20);
                QTest::mouseMove(window, target + QPoint(20, 10), 20);
                QTest::mouseMove(window, target, 20);
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, target);
                QTest::qWait(80);
                check(before != pictureId && backgroundId(slideId) == pictureId,
                      "dragging a bin card onto a cue sets that slide's background");
                docs.undo();
                QTest::qWait(40);
            }
        }

        // MB.5: camera cards wear our own glyph, numbered 1..4 in creation
        // order; the model refuses a fifth, so no unnumbered camera can exist.
        check(docs.addCameraAsset(QStringLiteral("Camera 1"), QStringLiteral("SOURCE 1")),
              "a camera asset is added");
        check(docs.addCameraAsset(QStringLiteral("Camera 2"), QStringLiteral("SOURCE 2")),
              "a second camera is added");
        QTest::qWait(40);
        int numbered = 0;
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("kind").toString()
                == QLatin1String("camera"))
                numbered += row->property("cameraNumber").toInt() >= 1;
        check(numbered == 2, "camera cards are numbered 1..4 by creation order");
        check(docs.addCameraAsset(QStringLiteral("Camera 3"), QStringLiteral("SOURCE 3"))
                  && docs.addCameraAsset(QStringLiteral("Camera 4"),
                                         QStringLiteral("SOURCE 4")),
              "the third and fourth cameras are accepted");
        const int camerasBefore = docs.assets().size();
        check(!docs.addCameraAsset(QStringLiteral("Camera 5"), QStringLiteral("SOURCE 5"))
                  && docs.assets().size() == camerasBefore,
              "a fifth camera is refused and creates nothing");

        // ND.9: an off-air camera cue has no frame to show (its receiver
        // opens only for the scene the audience is on), so the service grid
        // wears the Media Bin's own glyph — the same drawing, same number.
        {
            QString cameraId;
            for (const QVariant& asset : docs.assets()) {
                if (asset.toMap().value(QStringLiteral("kind")).toString()
                        == QLatin1String("camera")) {
                    cameraId = asset.toMap().value(QStringLiteral("id")).toString();
                    break;
                }
            }
            const int cueCount = showPageItem->property("cueRows").toList().size();
            check(!cameraId.isEmpty()
                      && docs.addMediaCue(cameraId, docs.playlistEntries().size()),
                  "the camera becomes a service cue");
            QTest::qWait(120);
            const QVariantList cameraRows = showPageItem->property("cueRows").toList();
            check(cameraRows.size() == cueCount + 1
                      && cameraRows.last().toMap().value(QStringLiteral("cameraNumber"))
                             .toInt() == 1,
                  "the camera cue carries the bin's camera number");
            auto* grid = window->findChild<QQuickItem*>("operatorSlideGrid");
            // The view's cells are not cue indices: scroll through the
            // collection's own mapping, as the page does.
            QMetaObject::invokeMethod(grid, "scrollCueIntoView",
                                      Q_ARG(QVariant, cameraRows.size() - 1));
            QTest::qWait(160);
            QList<QQuickItem*> tiles;
            collectNamed(grid, "operatorCueTile", tiles);
            bool glyphNumbered = false;
            for (auto* tile : tiles) {
                if (!tile->isVisible()
                    || tile->property("index").toInt() != cameraRows.size() - 1)
                    continue;
                auto* glyph = findNamed(tile, "cueCameraArtwork");
                glyphNumbered = glyph && glyph->isVisible()
                    && glyph->property("number").toInt() == 1;
            }
            check(glyphNumbered,
                  "the off-air camera cue wears the bin's numbered glyph, not a black tile");
            const auto cameraCapture = qEnvironmentVariable("OPENSHOW_WORKSPACE_CAPTURE");
            if (!cameraCapture.isEmpty())
                window->grabWindow().save(cameraCapture + "-camera-cue.png");
            docs.undo();
            QTest::qWait(100);
            check(showPageItem->property("cueRows").toList().size() == cueCount,
                  "undo takes the camera cue and its glyph with it");
        }

        // ND.9: the camera's own menu offers the front-camera mirror, read off
        // the asset (unticked until the document says otherwise).
        {
            for (auto* row : mediaRows())
                if (row->property("modelData").toMap().value("name").toString()
                    == QLatin1String("Camera 1"))
                    clickAt(row, 20, 14);
            QTest::qWait(30);
            QObject* object = openMenu();
            auto* mirror = menuItem(object, "mediaCameraMirrorAction");
            check(mirror && mirror->isVisible()
                      && !mirror->property("text").toString().startsWith(
                          QStringLiteral("\u2713")),
                  "a selected camera offers the mirror, unticked while it is off");
            if (mirror != nullptr) {
                QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                                  mirror->mapToScene(QPointF(30, mirror->height() / 2))
                                      .toPoint());
                QTest::qWait(80);
            }
            if (object != nullptr) {
                QMetaObject::invokeMethod(object, "close");
                QTest::qWait(40);
            }
            bool mirrored = false;
            for (const QVariant& asset : docs.assets())
                if (asset.toMap().value("name").toString() == QLatin1String("Camera 1"))
                    mirrored = asset.toMap().value("cameraMirror").toBool();
            check(mirrored, "the menu flips the live picture the worker draws");
            docs.undo();
            QTest::qWait(40);
            bool restored = true;
            for (const QVariant& asset : docs.assets())
                if (asset.toMap().value("name").toString() == QLatin1String("Camera 1"))
                    restored = !asset.toMap().value("cameraMirror").toBool();
            check(restored, "one undo puts the camera's picture back the way it was");
        }

        // A camera is an asset like any other: the Delete key removes the
        // selected one, with the card click giving the bin the keyboard.
        {
            for (auto* row : mediaRows())
                if (row->property("modelData").toMap().value("name").toString()
                    == QLatin1String("Camera 4"))
                    clickAt(row, 20, 14);
            QTest::qWait(30);
            const int before = docs.assets().size();
            QTest::keyClick(window, Qt::Key_Delete);
            QTest::qWait(40);
            check(docs.assets().size() == before - 1,
                  "Delete removes a selected camera");
        }

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

        // At the smallest supported size the grid reflows to fewer columns
        // without losing names. The card's notice is the part that gives way
        // first; the file name always keeps a readable share of the width.
        window->resize(1000, 680);
        QTest::qWait(80);
        {
            // The card is the preview; the name lives in the inspector.
            if (!mediaRows().isEmpty())
                clickAt(mediaRows().first(), 20, 14);
            QTest::qWait(20);
            auto* name = findNamed(mediaBin, "mediaInfoName");
            const bool named = name != nullptr && name->width() > 40
                               && !name->property("text").toString().isEmpty()
                               && name->property("text").toString()
                                      != QLatin1String("nothing selected");
            check(mediaRows().size() >= 2 && named,
                  "a narrow bin reflows the grid and the inspector keeps the name");
        }
        window->resize(binSizeBefore);
        QTest::qWait(60);
        panels.resizeDrawer(300, window->height() - 364);
        QTest::qWait(40);
        // Leave the bin on the audio row: no cue control, just the route.
        for (auto* row : mediaRows())
            if (row->property("modelData").toMap().value("name").toString()
                == QLatin1String("hymn.mp3"))
                clickAt(row, 20, 14);
        QTest::qWait(40);
        check(findNamed(window->contentItem(), "mediaActivateButton") == nullptr
                  && !worker.standaloneOnAir(),
              "the audio row carries no transport control");

        // The Delete key removes the selected asset, the same verb the menu's
        // Remove calls. It works on Show as well as in Edit: the bin browses
        // the service's media, not the slide under edit. A disposable asset
        // keeps the later expectations intact.
        {
            std::ofstream(std::filesystem::path(scratch.path().toStdString())
                          / "delete-me.txt") << "BYTES";
            check(docs.importMediaFile(files + "/delete-me.txt"),
                  "import a disposable asset");
            window->setProperty("page", "show");
            QTest::qWait(40);
            for (auto* row : mediaRows())
                if (row->property("modelData").toMap().value("name").toString()
                    == QLatin1String("delete-me.txt"))
                    clickAt(row, 20, 14);
            QTest::qWait(20);
            const int before = docs.assets().size();
            QTest::keyClick(window, Qt::Key_Delete);
            QTest::qWait(40);
            check(docs.assets().size() == before - 1,
                  "Delete removes the selected media asset on the Show page");
        }

        // ND.7: the Add NDI action belongs to the bin's context menu. Its
        // items are instantiated when the menu opens; this fixture has no
        // worker, so the action must be present but hidden.
        if (auto* menuObject = mediaBin->findChild<QObject*>("mediaContextMenu")) {
            QMetaObject::invokeMethod(menuObject, "open");
            QTest::qWait(30);
            QQuickItem* addNdi = nullptr;
            const bool found = QMetaObject::invokeMethod(
                menuObject, "itemAt", Q_RETURN_ARG(QQuickItem*, addNdi), Q_ARG(int, 0));
            check(found && addNdi != nullptr &&
                      addNdi->objectName() == QLatin1String("mediaAddNdiAction"),
                  "the Media Bin offers an Add NDI action");
            if (addNdi != nullptr) {
                check(!addNdi->property("visible").toBool(),
                      "the Add NDI action is hidden without a live-capable worker");
            }
            QMetaObject::invokeMethod(menuObject, "close");
            QTest::qWait(20);
        }
    }

    window->setProperty("page", "show");
    QTest::qWait(80);
    panels.resizeDrawer(300, window->height() - 364);
    QTest::qWait(50);
    // MB.9: a media card dropped on the running order becomes its own cue:
    // one schedule-owned show with a single slide whose whole picture is the
    // video's background — no lyric layer, no Library row.
    {
        openFolder(serviceFolder);
        auto* serviceDrop = findNamed(window->contentItem(), "serviceDropArea");
        check(serviceDrop && serviceDrop->isVisible(),
              "the open Service folder shows the running order that accepts drops");
        QString videoId;
        for (const QVariant& entry : docs.assets()) {
            const QVariantMap row = entry.toMap();
            if (row.value(QStringLiteral("name")).toString() == QLatin1String("loop.mp4"))
                videoId = row.value(QStringLiteral("id")).toString();
        }
        check(!videoId.isEmpty(), "the video is still in the bin");
        if (!videoId.isEmpty() && serviceDrop) {
            const QVariantList entriesBefore = docs.playlistEntries();
            QStringList entryIdsBefore;
            for (const QVariant& entry : entriesBefore)
                entryIdsBefore << entry.toMap().value(QStringLiteral("entryId")).toString();
            QMimeData mime;
            mime.setText(QStringLiteral("media:") + videoId);
            const QPoint target = serviceDrop->mapToScene(
                QPointF(serviceDrop->width() / 2, 20)).toPoint();
            QDragEnterEvent enter(target, Qt::CopyAction, &mime, Qt::LeftButton,
                                  Qt::NoModifier);
            QCoreApplication::sendEvent(serviceDrop, &enter);
            QTest::qWait(20);
            check(enter.isAccepted(),
                  "the running order accepts a video card drag");
            QDropEvent drop(target, Qt::CopyAction, &mime, Qt::LeftButton,
                            Qt::NoModifier);
            QCoreApplication::sendEvent(serviceDrop, &drop);
            QTest::qWait(40);
            check(drop.isAccepted(), "the video drop is accepted");
            const QVariantList entries = docs.playlistEntries();
            check(entries.size() == entriesBefore.size() + 1,
                  "the video becomes one cue in the running order");
            QString newShowId;
            for (const QVariant& entry : entries) {
                const QString id = entry.toMap().value(QStringLiteral("entryId")).toString();
                if (!entryIdsBefore.contains(id))
                    newShowId = entry.toMap().value(QStringLiteral("showId")).toString();
            }
            check(!newShowId.isEmpty(), "the video cue names a new show");
            bool inLibrary = false;
            for (const QVariant& row : docs.songs())
                if (row.toMap().value(QStringLiteral("id")).toString() == newShowId)
                    inLibrary = true;
            check(!inLibrary, "the video cue is not a Library row");
            const QVariantList slides = docs.slidesOf(newShowId);
            check(slides.size() == 1, "the video cue is a single slide");
            if (slides.size() == 1) {
                const QVariantMap slide = slides.first().toMap();
                check(slide.value(QStringLiteral("backgroundAssetId")).toString() == videoId,
                      "the video fills the cue's background layer");
                check(slide.value(QStringLiteral("preview")).toString()
                          == QLatin1String("(empty)"),
                      "the video cue's slide has no lyric text");
            }
            docs.undo();
            check(docs.playlistEntries().size() == entriesBefore.size(),
                  "one undo removes the video cue");
        }

        // Audio takes the same drop, but ON a cue it joins that show's
        // audio layer; below the last row it makes an audio cue of its own.
        QString audioId;
        for (const QVariant& entry : docs.assets()) {
            const QVariantMap row = entry.toMap();
            if (row.value(QStringLiteral("name")).toString() == QLatin1String("hymn.mp3"))
                audioId = row.value(QStringLiteral("id")).toString();
        }
        const QVariantList order = docs.playlistEntries();
        check(!audioId.isEmpty() && !order.isEmpty(),
              "the audio card and a running-order cue are available");
        if (!audioId.isEmpty() && !order.isEmpty() && serviceDrop) {
            const QString targetShow =
                order.first().toMap().value(QStringLiteral("showId")).toString();
            QMimeData audioMime;
            audioMime.setText(QStringLiteral("media:") + audioId);
            const int entriesNow = docs.playlistEntries().size();
            const QPoint onRow = serviceDrop->mapToScene(
                QPointF(serviceDrop->width() / 2, 20)).toPoint();
            QDragEnterEvent audioEnter(onRow, Qt::CopyAction, &audioMime,
                                       Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(serviceDrop, &audioEnter);
            QTest::qWait(20);
            check(audioEnter.isAccepted(),
                  "an audio card is accepted over an existing cue");
            QDropEvent audioDrop(onRow, Qt::CopyAction, &audioMime,
                                 Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(serviceDrop, &audioDrop);
            QTest::qWait(40);
            check(audioDrop.isAccepted() &&
                      docs.showInfo(targetShow).value(QStringLiteral("audioAssetId"))
                              .toString() == audioId,
                  "dropping audio on a cue sets that show's audio layer");
            check(docs.playlistEntries().size() == entriesNow,
                  "and it adds no second cue");
            docs.undo();
            check(docs.showInfo(targetShow).value(QStringLiteral("audioAssetId"))
                      .toString().isEmpty(),
                  "one undo removes the cue's audio layer");

            const QPoint belowRows = serviceDrop->mapToScene(
                QPointF(serviceDrop->width() / 2, serviceDrop->height() - 4)).toPoint();
            QDragEnterEvent newEnter(belowRows, Qt::CopyAction, &audioMime,
                                     Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(serviceDrop, &newEnter);
            QTest::qWait(20);
            QDropEvent newDrop(belowRows, Qt::CopyAction, &audioMime,
                               Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(serviceDrop, &newDrop);
            QTest::qWait(40);
            check(docs.playlistEntries().size() == entriesNow + 1,
                  "audio dropped below the cues becomes an audio cue");
            docs.undo();
        }
    }
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
        // Operator cards use one fixed render, independent of stretched cells.
        const QVariantMap px = pageCall("bandPixels").toMap();
        int askW = px.value(QStringLiteral("w")).toInt();
        const int askH = px.value(QStringLiteral("h")).toInt();
        check(askW == 320 && askH == 180,
              "the operator grid always asks for the fixed 320x180 picture");
        check(askW >= 32 && askW <= 512 && askH >= 32 && askH <= 512
                  && openshow::protocol::thumbnailExtentFits(
                         static_cast<std::uint32_t>(askW), static_cast<std::uint32_t>(askH),
                         openshow::protocol::previewSlotStrideBytes(640, 360)),
              "every size the control picks fits the preview slot this app streams at");

        window->resize(window->width() + 12, window->height());
        QTest::qWait(200);
        check(pageCall("bandPixels").toMap() == px,
              "a small window resize reuses the render extent instead of invalidating every cached tile");
        window->resize(sizeBefore);
        QTest::qWait(200);

        // Shrinking to the smallest step scales the same cached picture.
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
        check(askW == 320, "the smallest cell reuses the same fixed render size");

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
            const qreal bandHeight =
                showPageItem->property("groupHeaderHeight").toReal();
            check(qAbs(inGrid.x() - cueRect(3).value(QStringLiteral("x")).toReal()) < 1.0
                      && qAbs(inGrid.y() + cueGrid->property("contentY").toReal()
                              - cueRect(3).value(QStringLiteral("y")).toReal()
                              - bandHeight) < 1.0,
                  "the head tile sits directly under its group's name band");
        }
        // The padding is inert: clicking the fill beside Alpha costs nothing.
        showPageItem->setProperty("cueIndex", -1);
        check(firstFiller >= 0, "the fixture leaves some padding to test");
        // Bring the pad row on screen first: a click is only a statement
        // about the grid when the point really is inside the grid.
        QMetaObject::invokeMethod(cueGrid, "positionViewAtIndex",
                                  Q_ARG(int, firstFiller), Q_ARG(int, 4));
        QTest::qWait(60);
        const QPointF fillPoint = cueGrid->mapToScene(
            QPointF((firstFiller % cs7cols) * cs7cellW + cs7cellW / 2.0,
                    (firstFiller / cs7cols) * cs7cellH + cs7cellH / 2.0
                        - cueGrid->property("contentY").toReal()));
        check(QRectF(cueGrid->mapToScene(QPointF(0, 0)),
                     QSizeF(cueGrid->width(), cueGrid->height())).contains(fillPoint),
              "the padding click lands inside the visible grid");
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
        check(pxAtZoom0 == 320 && pxAtZoom2 == 320,
              "zooming scales the same fixed picture instead of requesting another size");

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

        // Task TL.5: the domain-derived Remove customizations state. A
        // slide whose ONLY authored answer is the underline must wake the
        // button — the old hand-kept name list omitted fontUnderline — and
        // an explicit `false` (underline off over an underlined theme) is
        // an authored answer too, so it must keep the button awake until
        // the operator takes it back.
        // The Theme Editor host has an underline switch of the same name;
        // scope the search to the VISIBLE Edit rail so the click cannot
        // land on the hidden page.
        auto* underline = itemNamed("editUnderline");
        check(underline != nullptr, "the shared rail carries the underline switch");
        if (underline != nullptr) {
            scrollIntoRail(underline);
            // The switch itself, not a mapped point: the row re-lays out
            // when its inherit chip appears, and the gesture under test is
            // the button's click whichever way the pointer would land.
            QMetaObject::invokeMethod(underline, "clicked");
            QTest::qWait(120);
            check(docs.slideAppearance().value(QStringLiteral("fontUnderline")).toMap()
                          .value(QStringLiteral("customized"))
                          .toBool() &&
                      docs.currentSlideCustomized() &&
                      itemNamed("removeCustomizationsButton")->property("enabled").toBool(),
                  "an underline-only edit wakes Remove customizations");
            underline = itemNamed("editUnderline");
            scrollIntoRail(underline);
            // The switch itself, not a mapped point: the row re-lays out
            // when its inherit chip appears, and the gesture under test is
            // the button's click whichever way the pointer would land.
            QMetaObject::invokeMethod(underline, "clicked");
            QTest::qWait(120);
            check(docs.slideAppearance().value(QStringLiteral("fontUnderline")).toMap()
                          .value(QStringLiteral("customized"))
                          .toBool() &&
                      docs.slideAppearance().value(QStringLiteral("fontUnderline")).toMap()
                          .value(QStringLiteral("value")).toBool() == false &&
                      itemNamed("removeCustomizationsButton")->property("enabled").toBool(),
                  "an explicit underline OFF is still an authored answer, not a cleared one");
            auto* underlineOnlyRemove = itemNamed("removeCustomizationsButton");
            QMetaObject::invokeMethod(underlineOnlyRemove, "clicked");
            QTest::qWait(150);
            check(!docs.currentSlideCustomized() &&
                      !itemNamed("removeCustomizationsButton")->property("enabled").toBool(),
                  "one Remove customizations gesture hands the underline back");
        }

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

        // Review D5: the cue's own audio layer is output activity for the
        // Escape/Clear All gate. The gate is a named property, so the fixture
        // reads the decision itself rather than inferring it from a key press.
        {
            auto* map = findNamed(window->contentItem(), "shortcutMap");
            check(map != nullptr, "the key table is reachable from the window");
            if (map != nullptr) {
                check(!map->property("audioOnAir").toBool() &&
                          !map->property("outputClearEnabled").toBool(),
                      "with nothing on air, the Escape/Clear All gate is closed");
                map->setProperty("audioOnAir", true);
                check(map->property("outputClearEnabled").toBool(),
                      "an audible cue alone opens the Escape/Clear All gate");
                map->setProperty("audioOnAir", false);
                check(!map->property("outputClearEnabled").toBool(),
                      "and closing it again closes the gate");
            }
        }

        auto* transportPlay = findNamed(window->contentItem(), "transportPlayButton");
        auto* transportIdle = findNamed(window->contentItem(), "transportIdleNote");
        check(transportPlay && !transportPlay->isVisible()
                  && transportIdle && transportIdle->isVisible(),
              "the transport names its emptiness and offers no controls for nothing");
        // The worker reports a LOOP-CONTINUOUS position (a looping clip's
        // clock climbs past its length), while the seek slider's range is the
        // clip's own length. The rail wraps it back into the clip, which is
        // the difference between a loop returning to the start and the slider
        // pinning at the right edge forever.
        {
            auto wrapped = [right](double position, double duration, bool looping) {
                QVariant answer;
                QMetaObject::invokeMethod(right, "transportPositionOf",
                                          Q_RETURN_ARG(QVariant, answer),
                                          Q_ARG(QVariant, position), Q_ARG(QVariant, duration),
                                          Q_ARG(QVariant, looping));
                return answer.toDouble();
            };
            check(wrapped(-1.0, 6.0, true) < 0.0,
                  "an unknown transport position stays unknown");
            check(qAbs(wrapped(2.5, 6.0, true) - 2.5) < 1e-6,
                  "a position inside the clip is shown as it is");
            check(qAbs(wrapped(8.5, 6.0, true) - 2.5) < 1e-6,
                  "a looping position past the clip wraps back onto the clip's own clock");
            check(qAbs(wrapped(12.0, 6.0, true)) < 1e-6,
                  "a loop landing exactly on the length shows the start, not the edge");
            check(qAbs(wrapped(8.5, 6.0, false) - 6.0) < 1e-6,
                  "a finished clip with loop off stays pinned at its end");
        }
        // A running time past a minute reads m:ss, both ends in the same
        // form; short clips keep the tenths where they still mean something.
        {
            auto clocked = [right](double seconds, bool useClock) {
                QVariant answer;
                QMetaObject::invokeMethod(right, "clockText",
                                          Q_RETURN_ARG(QVariant, answer),
                                          Q_ARG(QVariant, seconds), Q_ARG(QVariant, useClock));
                return answer.toString();
            };
            check(clocked(7.4, false) == QLatin1String("7.4 s") &&
                      clocked(52.3, false) == QLatin1String("52.3 s"),
                  "a short clip keeps tenths of a second");
            check(clocked(0.0, true) == QLatin1String("0:00") &&
                      clocked(7.4, true) == QLatin1String("0:07") &&
                      clocked(67.6, true) == QLatin1String("1:08") &&
                      clocked(267.6, true) == QLatin1String("4:28"),
                  "a long clip reads minutes:seconds, seconds zero-padded");
            check(clocked(59.6, true) == QLatin1String("1:00"),
                  "rounding at the minute boundary carries into the minute");
            check(clocked(-1.0, true) == QString(QChar(0x2014)),
                  "an unknown time is still the dash");
        }

        // The rail has no separate audio area any more: audio is a cue, and
        // the transport above governs it like a video. The one audio control
        // that remains is the MASTER mute, hoisted to the audience header
        // with the layer clears: it must be present, off until the worker
        // reports otherwise, and it must NOT be a second mute inside the
        // transport.
        check(findNamed(window->contentItem(), "audioCueCombo") == nullptr
                  && findNamed(window->contentItem(), "audioPlayButton") == nullptr
                  && findNamed(window->contentItem(), "audioStopButton") == nullptr
                  && findNamed(window->contentItem(), "audioStatusLine") == nullptr,
              "the rail offers no production-track Play/Stop area");
        auto* muteAll = findNamed(window->contentItem(), "masterMuteButton");
        const QPointF muteTopLeft = muteAll ? muteAll->mapToScene(QPointF(0, 0)) : QPointF();
        check(muteAll != nullptr && !worker.mediaMuted(),
              "the audience offers one master audio mute, off by default");
        check(muteAll && muteTopLeft.y() < previewTopLeft.y()
                  && findNamed(window->contentItem(), "transportMuteAllCheck") == nullptr,
              "the master mute sits above the monitor and left the transport behind");
        check(muteAll && findByClass(muteAll, "CheckBox") == nullptr,
              "the master mute is one toggle, not a tick box");
        // The old rows said "Video audio" over a bare slider beside a Mute
        // tick box; the mix row now names the source and its volume, which is
        // the sentence the delegate paints.
        {
            auto volumeLabel = [right](const char* source) {
                QVariant answer;
                QMetaObject::invokeMethod(right, "sourceVolumeLabel",
                                          Q_RETURN_ARG(QVariant, answer),
                                          Q_ARG(QVariant, QString::fromLatin1(source)));
                return answer.toString();
            };
            check(volumeLabel("video") == QLatin1String("Video audio volume")
                      && volumeLabel("audio") == QLatin1String("Show audio volume"),
                  "each mix row says which source it is and that the slider is its volume");
        }
        check(worker.standalonePath().isEmpty() && !worker.standaloneOnAir()
                  && worker.standaloneRefusal().isEmpty() && worker.sceneActive() == 0,
              "nothing on the rail has sent a standalone selection");
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
        check(!search && binToggle && tabShow && tabEdit && tabSettings
                  && binToggle->isVisible()
                  && tabEdit->isVisible() && tabSettings->isVisible(),
              "the header offers the operator pages, media visibility and Settings without redundant Search");
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

        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QTest::qWait(80);
        auto* field = findNamed(window->contentItem(), "librarySearchField");
        check(field && field->property("activeFocus").toBool()
                  && window->property("typingIntoField").toBool(),
              "Ctrl+F takes the keyboard to the Library field");
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
        check(binToggle->isVisible() && tabShow->isVisible(),
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

        auto checkThemeMenuSpacing = [&](const QList<QQuickItem*>& rows) {
            auto* empty = findNamed(window->contentItem(), "themeMenuEmpty");
            auto* editor = findNamed(window->contentItem(), "themeMenuEditor");
            check(empty && !empty->isVisible() && empty->height() == 0,
                  "the hidden no-themes placeholder reserves no menu row");
            if (rows.isEmpty() || !editor) return;
            auto* last = rows.last();
            const qreal gap = editor->mapToScene(QPointF(0, 0)).y()
                - last->mapToScene(QPointF(0, last->height())).y();
            check(gap >= 0 && gap < last->height() / 2,
                  "only the separator separates the last theme and Theme Editor");
        };
        checkThemeMenuSpacing(menuRowsWith(1));

        check(docs.createBlankTheme("Alpha") && docs.createBlankTheme("Beta"),
              "two themes exist for the menu to name");
        QList<QQuickItem*> menuRows = menuRowsWith(3);
        check(!menuRows.isEmpty(), "every theme is a row in the open menu");
        checkThemeMenuSpacing(menuRows);
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
        checkThemeMenuSpacing(menuRows2);
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

        const auto checkPlacementHandles = [&] {
            auto* box = findNamed(window->contentItem(), "themeEditorPlacementBox");
            QList<QQuickItem*> handles;
            collectNamed(window->contentItem(), "themeEditorPlacementHandle", handles);
            check(box && handles.size() == 4, "the placement box has four corner handles");
            if (!box) return;
            for (auto* handle : handles) {
                const auto center = handle->mapToItem(box, QPointF(handle->width() / 2,
                                                                   handle->height() / 2));
                const bool onX = qAbs(center.x()) < 0.01 || qAbs(center.x() - box->width()) < 0.01;
                const bool onY = qAbs(center.y()) < 0.01 || qAbs(center.y() - box->height()) < 0.01;
                check(onX && onY, "placement handles remain centred on the rectangle corners");
            }
        };
        checkPlacementHandles();
        check(!findNamed(window->contentItem(), "themeEditorBoxLabel"),
              "placement boxes have no instructional text above their outlines");

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
        QObject* themeMenuObject = nullptr;
        QObject* themeResetObject = nullptr;
        // Drag the box the overlay is OFFERING (the dashed built-in lyric
        // rect 160,100,1600,880): press inside it, move up-left (the wide
        // box clamps against the canvas's right edge before it clamps at
        // zero), release once.
        const qreal zoom = previewFrame->width() / 1920.0;
        const auto dragOfferedBox = [&](double dx, double dy) {
            const QPointF hold = previewFrame->mapToScene(QPointF(960 * zoom, 540 * zoom));
            const QPointF dragged = hold + QPointF(dx, dy);
            QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, hold.toPoint());
            QTest::mouseMove(window, dragged.toPoint(), 20);
            QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, dragged.toPoint());
            QTest::qWait(80);
        };
        dragOfferedBox(-20, -15);
        checkPlacementHandles();
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
        // The gesture and its fixture share one snap answer: inside the
        // tolerance the axis lands exactly on the centre, outside it the
        // pointer's travel is kept.
        {
            auto snapped = [editorPage](double x, double y, double w, double h,
                                        double tolerance) {
                QVariant answer;
                QMetaObject::invokeMethod(editorPage, "snappedCentre",
                                          Q_RETURN_ARG(QVariant, answer),
                                          Q_ARG(QVariant, x), Q_ARG(QVariant, y),
                                          Q_ARG(QVariant, w), Q_ARG(QVariant, h),
                                          Q_ARG(QVariant, tolerance));
                return answer.toRectF();
            };
            const QRectF nearX = snapped(915.0, 200.0, 100.0, 100.0, 8.0);
            const QRectF farX = snapped(900.0, 200.0, 100.0, 100.0, 8.0);
            const QRectF nearY = snapped(200.0, 495.0, 100.0, 100.0, 8.0);
            check(qAbs(nearX.x() - 910.0) < 0.001 && qAbs(farX.x() - 900.0) < 0.001
                      && qAbs(nearY.y() - 490.0) < 0.001,
                  "the centre snap pulls a near axis exactly onto 960/540 and leaves a far one alone");
        }
        // BX.3: the box menu carries the reset the numeric row used to. A
        // second drag creates a placement; Menu opens the menu on the
        // selected box and Reset removes only the rectangle.
        dragOfferedBox(-20, -15);
        check(placementRow(placedId).value(QStringLiteral("placementSet")).toBool(),
              "the second drag creates the placement the menu acts on");
        const QVariantMap styledBefore = placementRow(placedId);
        QMetaObject::invokeMethod(editorPage, "openBoxMenuAtSelection");
        QTest::qWait(60);
        themeMenuObject = window->findChild<QObject*>("themeEditorBoxMenu");
        themeResetObject = window->findChild<QObject*>("themeEditorResetPlacementItem");
        check(themeMenuObject && themeMenuObject->property("visible").toBool(),
              "Menu opens the box menu on the selected box");
        check(themeResetObject && themeResetObject->property("enabled").toBool(),
              "the box menu offers Reset once the theme places");
        if (themeResetObject != nullptr)
            QMetaObject::invokeMethod(themeResetObject, "clicked");
        QTest::qWait(80);
        check(!placementRow(placedId).value(QStringLiteral("placementSet")).toBool()
                  && placementRow(placedId).value(QStringLiteral("name")).toString()
                         == styledBefore.value(QStringLiteral("name")).toString()
                  && placementRow(placedId).value(QStringLiteral("fontSize")).toDouble()
                         == styledBefore.value(QStringLiteral("fontSize")).toDouble(),
              "Reset box placement removes only the rectangle and keeps the theme's style");
        // Opening the menu with nothing placed offers no reset and creates
        // no record — the same rule the first check stated, now proven
        // after a reset has left the theme silent again.
        QMetaObject::invokeMethod(editorPage, "openBoxMenuAtSelection");
        QTest::qWait(60);
        themeMenuObject = window->findChild<QObject*>("themeEditorBoxMenu");
        themeResetObject = window->findChild<QObject*>("themeEditorResetPlacementItem");
        check(themeMenuObject && themeResetObject
                  && !themeResetObject->property("enabled").toBool()
                  && placementRow(placedId).value(QStringLiteral("placementSet")).toBool() == false,
              "opening the box menu with nothing placed offers no reset and creates nothing");
        if (themeMenuObject != nullptr)
            QMetaObject::invokeMethod(themeMenuObject, "close");
        QTest::qWait(80);
        // BX.3: the numeric geometry UI is gone and no blank spacer took its
        // place; TL's layout controls remain.
        check(findNamed(window->contentItem(), "themeEditorPlaceX") == nullptr
                  && findNamed(window->contentItem(), "themeEditorPlaceCenterH") == nullptr
                  && findNamed(window->contentItem(), "themeEditorNoPlacement") == nullptr
                  && findNamed(window->contentItem(), "themeEditorContextX") == nullptr
                  && findNamed(window->contentItem(), "themeEditorContextBoxClear") == nullptr
                  && findNamed(window->contentItem(), "boxGeometryControls") == nullptr,
              "no numeric geometry field, centre helper or old placement row remains in Themes");
        {
            auto* themeStyle = findNamed(window->contentItem(), "textStyleControls");
            auto* themeEffects = findNamed(window->contentItem(), "textEffectsControls");
            check(themeStyle && themeEffects
                      && themeEffects->mapToItem(themeStyle->parentItem(), QPointF()).y()
                             - (themeStyle->mapToItem(themeStyle->parentItem(), QPointF()).y()
                                + themeStyle->height()) <= 12.0,
                  "no blank spacer sits where the geometry section was");
        }
        check(findNamed(window->contentItem(), "themeEditorVerticalAlign") != nullptr
                  && findNamed(window->contentItem(), "themeEditorAllCaps") != nullptr,
              "TL's vertical alignment and All caps controls remain");
        // Right-click over the box opens the same menu by mouse.
        {
            const QPointF rightClick =
                previewFrame->mapToScene(QPointF(960 * zoom, 400 * zoom));
            QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                              rightClick.toPoint());
            QTest::qWait(60);
            check(themeMenuObject && themeMenuObject->property("visible").toBool(),
                  "right-click over the box opens the box menu");
            if (themeMenuObject != nullptr)
                QMetaObject::invokeMethod(themeMenuObject, "close");
            QTest::qWait(60);
        }

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
        auto* weightToggle = findNamed(window->contentItem(), "themeEditorWeight");
        auto* weightInherit = findNamed(window->contentItem(), "themeEditorWeightInherit");
        auto* alignControl = findNamed(window->contentItem(), "themeEditorAlign");
        auto* slantToggle = findNamed(window->contentItem(), "themeEditorSlant");
        auto* underlineToggle = findNamed(window->contentItem(), "themeEditorUnderline");
        auto* underlineInherit =
            findNamed(window->contentItem(), "themeEditorUnderlineInherit");
        auto* fitControl = findNamed(window->contentItem(), "themeEditorFit");
        check(alignControl && alignControl->property("count").toInt() == 3
                  && alignControl->property("glyphStyle").toString() == QLatin1String("align")
                  && fitControl && fitControl->property("count").toInt() == 2
                  && weightToggle && slantToggle && underlineToggle && underlineInherit,
              "alignment and fit are segmented; bold, italic and underline are B/I/U switches");
        auto* minSizeField = findNamed(window->contentItem(), "themeEditorMinSize");
        auto* strokeEnable = findNamed(window->contentItem(), "themeEditorStrokeEnable");
        auto* strokeOff = findNamed(window->contentItem(), "themeEditorStrokeInherit");
        auto* blurSlider = findNamed(window->contentItem(), "themeEditorShadowBlur");
        // A closed Popup is not in the visible item tree at all — in Qt 6 it
        // is not even an Item — so the QObject tree is where the picker is
        // looked for, and "no picker" is read the same way.
        auto* fontPopup = window->findChild<QObject*>(QStringLiteral("themeEditorFontPopup"));
        auto* familyField = findNamed(window->contentItem(), "themeEditorFontField");
        auto* fontStatus = findNamed(window->contentItem(), "themeEditorFontStatus");
        check(weightToggle && weightInherit && minSizeField && strokeEnable && strokeOff
                  && blurSlider && fontPopup && familyField && fontStatus,
              "the Songs inspector carries the typography, effect and family controls");
        check(!familyField->property("readOnly").toBool(),
              "without a worker the typeface field stays typeable and no Browse button is needed");
        check(!weightInherit->property("enabled").toBool()
                  && !strokeOff->property("enabled").toBool()
                  && !weightToggle->property("active").toBool()
                  && !underlineToggle->property("active").toBool()
                  && styleRow(placedId).value(QStringLiteral("fontWeightSet")).toBool() == false
                  && styleRow(placedId).value(QStringLiteral("fontUnderlineSet")).toBool() == false
                  && styleRow(placedId).value(QStringLiteral("strokeSet")).toBool() == false,
              "a silent theme's style controls start asleep and its row says nothing is set");

        // One switch is ONE field: the neighbours keep their silence.
        clickAt(weightToggle, weightToggle->width() / 2, weightToggle->height() / 2);
        QTest::qWait(80);
        check(styleRow(placedId).value(QStringLiteral("fontWeightSet")).toBool()
                  && styleRow(placedId).value(QStringLiteral("fontWeight")).toInt() == 700
                  && weightToggle->property("active").toBool()
                  && styleRow(placedId).value(QStringLiteral("strokeSet")).toBool() == false
                  && styleRow(placedId).value(QStringLiteral("minFontSizeSet")).toBool() == false,
              "the bold switch writes its own field and no other");
        check(weightInherit->property("enabled").toBool(),
              "the Inherit chip wakes when the theme has something to give back");
        docs.undo();
        QTest::qWait(80);
        check(!styleRow(placedId).value(QStringLiteral("fontWeightSet")).toBool()
                  && !weightInherit->property("enabled").toBool()
                  && !weightToggle->property("active").toBool(),
              "one undo takes the weight back to silence, and the switch repaints from the record");

        // Italic and underline are the same one-field switches; underline is
        // the contract's new decoration, so its round trip is pinned here.
        clickAt(slantToggle, slantToggle->width() / 2, slantToggle->height() / 2);
        QTest::qWait(80);
        check(styleRow(placedId).value(QStringLiteral("fontStyle")).toString()
                      == QLatin1String("italic")
                  && slantToggle->property("active").toBool(),
              "the italic switch writes italic and lights from the record");
        docs.undo();
        QTest::qWait(80);
        clickAt(underlineToggle, underlineToggle->width() / 2, underlineToggle->height() / 2);
        QTest::qWait(80);
        check(styleRow(placedId).value(QStringLiteral("fontUnderlineSet")).toBool()
                  && styleRow(placedId).value(QStringLiteral("fontUnderline")).toBool()
                  && underlineToggle->property("active").toBool()
                  && underlineInherit->property("enabled").toBool(),
              "underline materialises its own answer and wakes its Inherit chip");
        docs.undo();
        QTest::qWait(80);
        check(!styleRow(placedId).value(QStringLiteral("fontUnderlineSet")).toBool()
                  && !underlineToggle->property("active").toBool()
                  && !underlineInherit->property("enabled").toBool(),
              "one undo takes underline back to inheritance");

        // A field committed UNCHANGED writes nothing — and so it costs no
        // undo: the step back after it is still the weight, not a phantom.
        clickAt(weightToggle, weightToggle->width() / 2, weightToggle->height() / 2);
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
        check(!familyField->property("readOnly").toBool(),
              "with no worker that can enumerate, the typeface field stays typeable");
        clickAt(familyField, familyField->width() / 2, familyField->height() / 2);
        QTest::qWait(60);
        check(fontPopup && !fontPopup->property("visible").toBool(),
              "clicking a typeface field with no list to offer opens no empty picker");

        // The scripture reading context (task TF.4): the toggle CREATES the
        // entry out of the theme's current look and box, the fields commit
        // one property at a time, refusals snap back, and the line note
        // says it cannot measure rather than invent a number — no worker
        // runs in this fixture. What the section writes is the SAME entry
        // the Bible page paginates against; the e2e leg pins that.
        auto* ctxReset = findNamed(window->contentItem(), "themeEditorContextReset");
        auto* ctxSizeField = findNamed(window->contentItem(), "themeEditorContextSize");
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
        auto* themeNames = findNamed(window->contentItem(), "themeEditorList");
        check(viewSongs && viewBible && themeNames
                  && qAbs(viewSongs->mapToScene(QPointF()).x() - viewBible->mapToScene(QPointF()).x()) < 0.01
                  && viewBible->mapToScene(QPointF()).y() >= viewSongs->mapToScene(QPointF(0, viewSongs->height())).y()
                  && viewSongs->mapToScene(QPointF()).y() >= themeNames->mapToScene(QPointF(0, themeNames->height())).y(),
              "Songs and Bible previews stack vertically directly below the theme list");
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
        auto* contextFields = findNamed(window->contentItem(), "themeEditorContextFields");
        auto* initialAlign = findNamed(window->contentItem(), "themeEditorContextAlign");
        check(ctxReset && !ctxReset->isEnabled() && contextFields && contextFields->isVisible()
                  && ctxSizeField && ctxSizeField->isVisible()
                  && !contextRow(placedId).value(QStringLiteral("set")).toBool(),
              "Bible font controls are visible before an override exists, with no setup checkbox");
        QMetaObject::invokeMethod(ctxSizeField, "editingFinished");
        check(!contextRow(placedId).value(QStringLiteral("set")).toBool(),
              "leaving an unchanged inherited font field creates no context");
        if (initialAlign) QMetaObject::invokeMethod(initialAlign, "activated",
            Q_ARG(int, initialAlign->property("currentIndex").toInt()));
        QTest::qWait(80);
        check(contextRow(placedId).value(QStringLiteral("set")).toBool(),
              "the first deliberate font control edit creates the Bible view");
        docs.undo();
        QTest::qWait(80);
        check(!contextRow(placedId).value(QStringLiteral("set")).toBool()
                  && contextFields->isVisible(), "one undo removes the first edit and keeps the controls visible");
        if (initialAlign) QMetaObject::invokeMethod(initialAlign, "activated",
            Q_ARG(int, initialAlign->property("currentIndex").toInt()));
        QTest::qWait(80);
        const QVariantMap created = contextRow(placedId);
        check(created.value(QStringLiteral("set")).toBool() && created.value(QStringLiteral("boxSet")).toBool()
                  && created.value(QStringLiteral("height")).toDouble() == 880.0
                  && !created.value(QStringLiteral("fontSizeSet")).toBool(),
              "first edit preserves the inherited rectangle and renderer-default size");
        const auto inheritedReading = docs.themeSampleAppearance(placedId, "scripture");
        auto* contextFamily = findNamed(window->contentItem(), "themeEditorContextFont");
        auto* contextAlign = findNamed(window->contentItem(), "themeEditorContextAlign");
        check(contextFamily && contextFamily->property("text") == inheritedReading.value("fontFamily")
                  && !contextFamily->property("readOnly").toBool()
                  && ctxSizeField->property("text").toString().toDouble() == inheritedReading.value("fontSize").toDouble()
                  && contextAlign && contextAlign->property("count").toInt() == 3,
              "Bible uses the same resolved typeface row as Songs, typeable while no list answers");
        QMetaObject::invokeMethod(ctxSizeField, "editingFinished");
        check(contextRow(placedId) == created,
              "focus-out on an inherited font size does not create an override");
        checkPlacementHandles();
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
                  && ctxSizeField->property("text").toString().toDouble()
                        == inheritedReading.value("fontSize").toDouble(),
              "a negative size never reaches the context and the field restores its resolved size");
        check(waitFor([&] {
                  return ctxLinesLabel->property("text").toString().contains(
                          QStringLiteral("not measured"));
              }),
              "with no worker answering the line note says so instead of inventing a count");
        clickAt(ctxReset, ctxReset->width() / 2, ctxReset->height() / 2);
        QTest::qWait(80);
        check(!contextRow(placedId).value(QStringLiteral("set")).toBool()
                  && !ctxReset->isEnabled() && contextFields->isVisible(),
              "Reset Bible removes the override and leaves inherited controls visible");
        docs.undo();
        QTest::qWait(80);
        check(contextRow(placedId).value(QStringLiteral("set")).toBool()
                  && contextRow(placedId).value(QStringLiteral("height")).toDouble() == 880.0
                  && ctxReset->isEnabled() && contextFields->isVisible(),
              "one undo restores the complete Bible override");

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
        // BX.3: the Bible box's non-numeric actions live on its menu. The
        // theme gains a placement; Use theme's box copies it through the
        // existing merge (the reading size survives), and Reset box
        // placement removes ONLY the box.
        ctxSizeField->setProperty("text", "32");
        QMetaObject::invokeMethod(ctxSizeField, "editingFinished");
        QTest::qWait(80);
        check(docs.setThemePlacement(viewedId, 200.0, 120.0, 1400.0, 700.0),
              "the theme gains a placement the Bible view can borrow");
        QMetaObject::invokeMethod(editorPage, "openBoxMenuAtSelection");
        QTest::qWait(60);
        QObject* bibleMenu = window->findChild<QObject*>("themeEditorBoxMenu");
        QObject* borrowItem = window->findChild<QObject*>("themeEditorUseThemeBoxItem");
        check(bibleMenu && bibleMenu->property("visible").toBool() && borrowItem
                  && borrowItem->property("visible").toBool()
                  && borrowItem->property("enabled").toBool(),
              "the Bible box menu offers Use theme's box");
        if (borrowItem != nullptr)
            QMetaObject::invokeMethod(borrowItem, "clicked");
        QTest::qWait(80);
        const QVariantMap borrowed = contextRow(placedId);
        check(borrowed.value(QStringLiteral("boxSet")).toBool()
                  && borrowed.value(QStringLiteral("x")).toDouble() == 200.0
                  && borrowed.value(QStringLiteral("height")).toDouble() == 700.0
                  && borrowed.value(QStringLiteral("fontSizeSet")).toBool()
                  && borrowed.value(QStringLiteral("fontSize")).toDouble() == 32.0,
              "Use theme's box copies the rectangle and keeps the view's reading size");
        QMetaObject::invokeMethod(editorPage, "openBoxMenuAtSelection");
        QTest::qWait(60);
        QObject* bibleReset = window->findChild<QObject*>("themeEditorResetPlacementItem");
        check(bibleReset && bibleReset->property("enabled").toBool(),
              "the Bible box menu offers Reset once the view stores a box");
        if (bibleReset != nullptr)
            QMetaObject::invokeMethod(bibleReset, "clicked");
        QTest::qWait(80);
        const QVariantMap afterBoxReset = contextRow(placedId);
        check(!afterBoxReset.value(QStringLiteral("boxSet")).toBool()
                  && !afterBoxReset.value(QStringLiteral("insetSet")).toBool()
                  && afterBoxReset.value(QStringLiteral("fontSizeSet")).toBool()
                  && afterBoxReset.value(QStringLiteral("fontSize")).toDouble() == 32.0,
              "Reset box placement removes only the Bible box and keeps the reading size");
        docs.undo();
        QTest::qWait(80);
        check(contextRow(placedId).value(QStringLiteral("boxSet")).toBool()
                  && contextRow(placedId).value(QStringLiteral("height")).toDouble() == 700.0,
              "one undo restores the borrowed box");
        check(docs.clearThemePlacement(viewedId),
              "the temporary theme placement goes");
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
        check(editorPage->property("selectedView").toString() == QLatin1String("scripture")
                  && !worker.outputLive() && worker.frames() == 0,
              "browsing themes inside the Bible view still asks the output for nothing");

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
                         == docs.themeSampleSceneJson(typedId)
                  && second.value(QStringLiteral("scene")).toString()
                         == docs.themeSampleSceneJson(typedId, QStringLiteral("scripture")),
              "the tiles ask one batch: two keys naming the selection and each view's resolved sample");
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
        auto* ctxResetMove = findNamed(window->contentItem(), "themeEditorContextReset");
        auto* ctxMoveCombo = findNamed(window->contentItem(), "themeEditorContextTransitionKind");
        auto* ctxMoveInherit = findNamed(window->contentItem(),
                                         "themeEditorContextTransitionInherit");
        auto* ctxMoveWords = findNamed(window->contentItem(), "themeEditorContextTransitionWords");
        auto* ctxMoveBack = findNamed(window->contentItem(), "themeEditorContextTransitionMedia");
        auto* ctxMoveSource = findNamed(window->contentItem(),
                                        "themeEditorContextTransitionSource");
        auto* ctxMoveNote = findNamed(window->contentItem(), "themeEditorContextTransitionNote");
        auto* ctxSizeField = findNamed(window->contentItem(), "themeEditorContextSize");
        check(ctxResetMove && ctxMoveCombo && ctxMoveInherit && ctxMoveWords && ctxMoveBack
                  && ctxMoveSource && ctxMoveNote && ctxSizeField,
              "the Bible inspector carries the view's own Transition controls");
        if (!contextRow(movedId).value(QStringLiteral("set")).toBool()) {
            ctxSizeField->setProperty("text", "64");
            QMetaObject::invokeMethod(ctxSizeField, "editingFinished");
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
                  && !ctxMoveNote->isVisible(),
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
        clickAt(ctxResetMove, ctxResetMove->width() / 2, ctxResetMove->height() / 2);
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
        auto* cardGrid = findNamed(window->contentItem(), "bibleSlideCollection");
        check(cardGrid && !findNamed(window->contentItem(), "bibleVerseList")
                  && !findNamed(window->contentItem(), "biblePreviewCanvas")
                  && !scripture.previewActive(),
              "the import page uses generated thumbnails without verse selection or a duplicate canvas");
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
        // Re-resolve a row each time: the Library rebuilds its delegates on
        // every model change, so a pointer kept across an edit is a snapshot.
        auto findSongRow = [&](const QString& id) -> QQuickItem* {
            QList<QQuickItem*> rows;
            collectNamed(window->contentItem(), "librarySongRow", rows);
            for (auto* row : rows) {
                if (row->property("modelData").toMap().value(QStringLiteral("id"))
                        .toString() == id)
                    return row;
            }
            return nullptr;
        };
        auto clickMenuRow = [&](const char* name) {
            const QList<QQuickItem*> rows = settledRows(name, 1);
            if (rows.isEmpty())
                return false;
            clickAt(rows.first(), 60, rows.first()->height() / 2);
            return true;
        };
        auto findCueRow = [&](const QString& showId) -> QQuickItem* {
            QList<QQuickItem*> rows;
            collectNamed(window->contentItem(), "collectionPlaylistRow", rows);
            for (auto* row : rows) {
                if (row->property("modelData").toMap().value(QStringLiteral("showId"))
                        .toString() == showId)
                    return row;
            }
            return nullptr;
        };

        check(docs.createBlankTheme("House") && docs.createBlankTheme("Loud"),
              "two themes serve the apply surfaces");
        const QString house = themeIdNamed(QStringLiteral("House"));
        const QString loud = themeIdNamed(QStringLiteral("Loud"));
        check(docs.editTheme(loud, QStringLiteral("Loud"), QStringLiteral("#ff0000"),
                             QStringLiteral("#100000"), QString(), QString(), 0.0,
                             QStringLiteral("center"), 1.25),
              "Loud is restyled so which slide wears it is visible in the views");

        const QString viewed = docs.currentSongId();

        // --- the old apply bypasses are gone -------------------------------
        // The header's separate default picker and a cue tile's theme menu
        // were alternate, unguarded routes. Whole-item apply lives on the
        // left-hand rows now; per-slide styling stays in Edit.
        check(findNamed(window->contentItem(), "showThemeSelector") == nullptr,
              "the show header no longer carries a separate default-theme picker");
        QList<QQuickItem*> tiles;
        collectNamed(window->contentItem(), "operatorCueTile", tiles);
        check(tiles.size() >= 2, "the cue grid still has tiles");
        rightClickAt(tiles.at(1), 40, 20);
        QTest::qWait(80);
        check(window->findChild<QObject*>("cueThemeMenu") == nullptr,
              "a cue tile no longer opens a per-slide theme menu");

        // --- a prepared Library song: row -> Apply theme... -> dialog ------
        QVariantList dialogStanzas;
        QVariantMap dialogStanza;
        dialogStanza.insert(QStringLiteral("label"), QStringLiteral("Verse 1"));
        dialogStanza.insert(QStringLiteral("lines"),
                            QStringList{QStringLiteral("Dialog verse line")});
        dialogStanzas.append(dialogStanza);
        const QString dialogItem = library.addSong(
            QStringLiteral("Dialog Song"), dialogStanzas, QString(), QString(),
            QString());
        check(!dialogItem.isEmpty(), "a Library song exists for the apply dialog");
        QVariantList dialogPlan;
        dialogPlan.append(QVariantMap{
            {QStringLiteral("lines"), QStringList{QStringLiteral("Dialog verse line")}},
            {QStringLiteral("group"), QStringLiteral("Verse 1")},
            {QStringLiteral("label"), QStringLiteral("Verse 1")}});
        const QString dialogShow = docs.upsertCatalogSong(
            dialogItem, QStringLiteral("Dialog Song"), QString(), QString(),
            QString(), dialogPlan, true);
        check(!dialogShow.isEmpty() &&
                  docs.showForCatalogItem(dialogItem) == dialogShow,
              "the dialog song is prepared into the service and linked");
        openFolder(songsFolder);
        QTest::qWait(60);
        check(findSongRow(dialogItem) != nullptr,
              "the Library lists the dialog song's row");
        rightClickAt(findSongRow(dialogItem), 60, 14);
        auto* songMenu = window->findChild<QObject*>("librarySongMenu");
        const bool songMenuOpened =
            songMenu && waitFor([&] { return songMenu->property("visible").toBool(); });
        check(songMenuOpened && clickMenuRow("libraryApplyThemeItem"),
              "the Library song row offers Apply theme...");
        auto* dialog = window->findChild<QObject*>("applyThemeDialog");
        check(dialog && waitFor([&] { return dialog->property("visible").toBool(); }),
              "Apply theme... opens the whole-item dialog");
        check(dialog->property("itemTitle").toString() == QStringLiteral("Dialog Song"),
              "the dialog names the clicked record, not the editing cursor");
        check(dialog->property("occurrenceCount").toInt() == 1,
              "the dialog reports how many occurrences the item has");
        check(dialog->property("rememberOnly").toBool() == false,
              "a prepared song offers the service actions");
        auto* picker = window->findChild<QQuickItem*>("applyThemePicker");
        int loudIndex = -1;
        for (int i = 0; i < docs.themes().size(); ++i) {
            if (docs.themes()[i].toMap().value(QStringLiteral("id")).toString() == loud)
                loudIndex = i;
        }
        check(picker && loudIndex >= 0, "the dialog carries a theme picker");
        if (picker)
            picker->setProperty("currentIndex", loudIndex);
        QTest::qWait(40);
        auto* applyOnly = findNamed(window->contentItem(), "applyThemeOnlyButton");
        auto* applyReflow = findNamed(window->contentItem(), "applyThemeReflowButton");
        const auto checkApplyDialogGeometry = [&] {
            auto* content = dialog->property("contentItem").value<QQuickItem*>();
            check(content != nullptr, "the apply dialog has visible content");
            if (!content)
                return;
            const QRectF bounds(content->mapToScene(QPointF(0, 0)), content->size());
            check(bounds.left() >= 0 && bounds.right() <= window->width() &&
                      bounds.top() >= 0 && bounds.bottom() <= window->height(),
                  "the apply dialog fits in the application window");
            for (auto* button : {applyOnly, applyReflow,
                                findNamed(window->contentItem(), "applyThemeCancelButton")}) {
                if (!button || !button->isVisible())
                    continue;
                const QRectF rect(button->mapToScene(QPointF(0, 0)), button->size());
                check(bounds.contains(rect),
                      "every apply action fits inside the dialog content");
            }
            check(dialog->property("width").toDouble() >= 560,
                  "the apply dialog is sized for its actions, independently of the sidebar");
        };
        const QSize dialogWindowSize = window->size();
        for (const QSize& size : {QSize(1000, 650), QSize(1680, 900), QSize(1380, 800)}) {
            window->resize(size);
            QTest::qWait(60);
            checkApplyDialogGeometry();
        }
        window->resize(dialogWindowSize);
        QTest::qWait(60);
        check(applyOnly && applyOnly->property("enabled").toBool(),
              "Apply only is the safe default and is available");
        clickAt(applyOnly, applyOnly->width() / 2, applyOnly->height() / 2);
        check(waitFor([&] {
                  return !docs.showInfo(dialogShow)
                              .value(QStringLiteral("appliedThemeId"))
                              .toString().isEmpty();
              }),
              "Apply only publishes the chosen look on the linked presentation");
        docs.selectSong(dialogShow);
        docs.selectSlide(0);
        check(waitFor([&] {
                  return docs.slideAppearance()
                             .value(QStringLiteral("textColor"))
                             .toMap()
                             .value(QStringLiteral("value"))
                             .toString() == QStringLiteral("#ff0000");
              }),
              "the whole item wears Loud");
        check(!library.itemThemeSnapshot(dialogItem).isEmpty(),
              "the linked song remembers the chosen look for future services");
        check(waitFor([&] { return !dialog->property("visible").toBool(); }),
              "the dialog closes after a successful apply");
        docs.undo();
        check(docs.showInfo(dialogShow)
                  .value(QStringLiteral("appliedThemeId"))
                  .toString().isEmpty(),
              "one service undo removes the look");
        docs.redo();
        check(!docs.showInfo(dialogShow)
                   .value(QStringLiteral("appliedThemeId"))
                   .toString().isEmpty(),
              "redo puts it back");

        check(docs.showInfo(dialogShow).value(QStringLiteral("appliedThemeId"))
                      .toString() == loud,
              "applying a local theme keeps its identity instead of making a snapshot copy");
        check(docs.setThemeTextStyleField(loud, QStringLiteral("allCaps"), true),
              "the chosen theme is updated to All caps");

        // --- Apply and reflow with no measuring worker ---------------------
        // The workspace test runs off air: the button is available for an
        // eligible item, preparing refuses honestly, and nothing changes.
        openFolder(songsFolder);
        QTest::qWait(40);
        rightClickAt(findSongRow(dialogItem), 60, 14);
        check(waitFor([&] { return songMenu->property("visible").toBool(); }) &&
                  clickMenuRow("libraryApplyThemeItem"),
              "the row menu reopens for the reflow leg");
        check(dialog && waitFor([&] { return dialog->property("visible").toBool(); }),
              "the dialog opens for the reflow leg");
        check(picker && picker->property("currentValue").toString() == loud,
              "reopening selects the updated local theme");
        checkApplyDialogGeometry();
        const QVariantList slidesBeforeReflow = docs.slidesOf(dialogShow);
        if (picker)
            picker->setProperty("currentIndex", loudIndex);
        QTest::qWait(40);
        check(applyReflow && applyReflow->property("enabled").toBool(),
              "an eligible item offers Apply and reflow");
        clickAt(applyReflow, applyReflow->width() / 2, applyReflow->height() / 2);
        auto* notice = findNamed(window->contentItem(), "applyThemeNotice");
        check(waitFor([&] {
                  return notice && notice->property("visible").toBool() &&
                         notice->property("text").toString().contains(
                             QStringLiteral("connected renderer"));
              }),
              "with no worker the reflow explains that it needs a connected renderer");
        check(docs.slidesOf(dialogShow).size() == slidesBeforeReflow.size(),
              "the refused reflow changes no slide");
        // A plan can also be refused after measurement (for example, after
        // a document change). Keep the transaction's sentence visible;
        // reading a non-QML lastError member used to erase that message.
        dialog->setProperty("preparedSnapshot", docs.themeSnapshotJson(loud));
        dialog->setProperty("preparing", true);
        QMetaObject::invokeMethod(&reflow, "prepared", Q_ARG(bool, true),
                                  Q_ARG(QString, QString()));
        check(waitFor([&] {
                  return notice && notice->property("visible").toBool() &&
                         notice->property("text").toString().contains(
                             QStringLiteral("no prepared reflow plan"));
              }) && dialog->property("visible").toBool(),
              "a transaction refusal stays visible in the open apply dialog");
        closeMenu("applyThemeDialog");
        if (auto* popup = window->findChild<QObject*>("applyThemeDialog"))
            QMetaObject::invokeMethod(popup, "close");
        waitFor([&] { return !dialog->property("visible").toBool(); });

        // --- an ineligible item explains why, and still applies only ------
        docs.selectSong(dialogShow);
        docs.selectSlide(0);
        check(docs.addTextBox(), "the item gains a second text box");
        openFolder(songsFolder);
        QTest::qWait(40);
        rightClickAt(findSongRow(dialogItem), 60, 14);
        check(waitFor([&] { return songMenu->property("visible").toBool(); }) &&
                  clickMenuRow("libraryApplyThemeItem"),
              "the row menu opens for the ineligible leg");
        check(dialog && waitFor([&] { return dialog->property("visible").toBool(); }),
              "the dialog opens for the ineligible item");
        auto* eligibility = findNamed(window->contentItem(), "applyThemeEligibility");
        check(applyReflow && !applyReflow->property("enabled").toBool() &&
                  eligibility &&
                  eligibility->property("text").toString().contains(
                      QStringLiteral("more than one text box")),
              "an ineligible item disables reflow and states the reason");
        check(applyOnly && applyOnly->property("enabled").toBool(),
              "Apply only remains available for the ineligible item");
        QMetaObject::invokeMethod(dialog, "close");
        waitFor([&] { return !dialog->property("visible").toBool(); });
        docs.undo();  // the second box
        check(docs.slidesOf(dialogShow).size() > 0,
              "the ineligible leg left a undoable clean state");

        // --- Use service default returns the item to inheritance ----------
        docs.selectSong(dialogShow);
        docs.selectSlide(0);
        openFolder(serviceFolder);
        QTest::qWait(40);
        QQuickItem* dialogCue_unused = findCueRow(dialogShow);
        check(dialogCue_unused != nullptr,
              "the running order lists the dialog song's cue");
        // The cue may be appended past the visible rows; bring it on screen
        // so the click lands on it rather than on a neighbour.
        int dialogCueIndex = -1;
        for (int i = 0; i < docs.playlistEntries().size(); ++i) {
            if (docs.playlistEntries()[i].toMap().value(QStringLiteral("showId"))
                    .toString() == dialogShow)
                dialogCueIndex = i;
        }
        check(dialogCueIndex >= 0, "the dialog song has a cue index");
        playlistList->setProperty("currentIndex", dialogCueIndex);
        playlistList->setProperty("contentY",
                                  qMax(0, dialogCueIndex * 28 - 40));
        QTest::qWait(80);
        rightClickAt(findCueRow(dialogShow), 60, 14);
        QTest::qWait(80);
        auto* serviceMenu = window->findChild<QObject*>("serviceItemMenu");
        check(serviceMenu &&
                  waitFor([&] { return serviceMenu->property("visible").toBool(); }) &&
                  !settledRows("serviceApplyThemeItem", 1).isEmpty(),
              "a service row offers whole-item apply and Use service default");
        check(clickMenuRow("serviceUseDefaultItem"),
              "Use service default is reachable on the service row");
        check(waitFor([&] {
                  return docs.showInfo(dialogShow)
                             .value(QStringLiteral("appliedThemeId"))
                             .toString().isEmpty();
              }),
              "Use service default returns the item to inheritance");
        check(waitFor([&] { return library.itemThemeSnapshot(dialogItem).isEmpty(); }),
              "and forgets the linked song's remembered look");

        // --- the keyboard reaches the same row menu -----------------------
        openFolder(serviceFolder);
        QTest::qWait(40);
        auto* servicePanelItem = window->findChild<QQuickItem*>("currentServicePanel");
        check(servicePanelItem != nullptr, "the service panel is addressable");
        if (servicePanelItem)
            servicePanelItem->forceActiveFocus();
        playlistList->setProperty("currentIndex", 0);
        QTest::qWait(40);
        QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
        check(waitFor([&] { return serviceMenu->property("visible").toBool(); }),
              "Shift+F10 opens the service row menu for the keyboard");
        closeMenu("serviceItemMenu");
        waitFor([&] { return !serviceMenu->property("visible").toBool(); });
        openFolder(songsFolder);
        QTest::qWait(40);
        browserPane->forceActiveFocus();
        songList->setProperty("currentIndex", 0);
        QTest::qWait(40);
        QTest::keyClick(window, Qt::Key_Menu);
        check(waitFor([&] { return songMenu->property("visible").toBool(); }),
              "the Menu key opens the Library song row menu");
        closeMenu("librarySongMenu");
        waitFor([&] { return !songMenu->property("visible").toBool(); });

        // --- an unprepared Library song only remembers --------------------
        QVariantList rememberStanzas;
        QVariantMap rememberStanza;
        rememberStanza.insert(QStringLiteral("label"), QStringLiteral("Verse 1"));
        rememberStanza.insert(QStringLiteral("lines"),
                              QStringList{QStringLiteral("Future service line")});
        rememberStanzas.append(rememberStanza);
        const QString rememberItem = library.addSong(
            QStringLiteral("Unprepared Song"), rememberStanzas, QString(), QString(),
            QString());
        check(!rememberItem.isEmpty(), "an unprepared Library song exists");
        openFolder(songsFolder);
        QTest::qWait(40);
        check(findSongRow(rememberItem) != nullptr, "the unprepared song has a row");
        const qsizetype entriesBeforeRemember = docs.playlistEntries().size();
        rightClickAt(findSongRow(rememberItem), 60, 14);
        check(waitFor([&] { return songMenu->property("visible").toBool(); }) &&
                  clickMenuRow("libraryApplyThemeItem"),
              "the unprepared row offers Apply theme...");
        check(dialog && waitFor([&] { return dialog->property("visible").toBool(); }),
              "the unprepared song's dialog opens");
        check(dialog->property("rememberOnly").toBool(),
              "an unprepared song only remembers the look");
        auto* rememberButton = findNamed(window->contentItem(), "applyThemeRememberButton");
        check(rememberButton && rememberButton->isVisible() &&
                  applyOnly && !applyOnly->property("enabled").toBool(),
              "Remember theme replaces the service actions for an unprepared song");
        if (picker)
            picker->setProperty("currentIndex", loudIndex);
        QTest::qWait(40);
        clickAt(rememberButton, rememberButton->width() / 2,
                rememberButton->height() / 2);
        check(waitFor([&] { return !library.itemThemeSnapshot(rememberItem).isEmpty(); }),
              "Remember theme stores the look in the Library");
        check(docs.playlistEntries().size() == entriesBeforeRemember,
              "remembering does not add a hidden presentation or cue");
        check(waitFor([&] { return !dialog->property("visible").toBool(); }),
              "the remember dialog closes");

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

        // Backgrounds belong to the slide, independently of its theme.
        check(docs.customizeCurrentSlide("backgroundGradient", "#333333"),
              "the slide gains its own explicit background gradient");
        check(waitFor([&] {
                  return gradientField->property("text").toString()
                         == QLatin1String("#333333");
              }),
              "the gradient field follows the authored slide backdrop");

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
            auto* weightToggle = findNamed(window->contentItem(), "editWeight");
            auto* weightInherit = findNamed(window->contentItem(), "editWeightInherit");
            auto* underlineToggle = findNamed(window->contentItem(), "editUnderline");
            auto* underlineInherit = findNamed(window->contentItem(), "editUnderlineInherit");
            auto* strokeCheck = findNamed(window->contentItem(), "editStrokeEnable");
            auto* strokeOff = findNamed(window->contentItem(), "editStrokeOff");
            check(sizeField && sizeBadge && alignCombo && alignBadge && weightToggle
                      && weightInherit && underlineToggle && underlineInherit
                      && strokeCheck && strokeOff,
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
            // B / I / U speak, the ↺ quiets them, exactly as on the Songs
            // rail. No worker round-trip is needed for a switch.
            clickAt(weightToggle, weightToggle->width() / 2, weightToggle->height() / 2);
            QTest::qWait(120);
            check(docs.slideAppearance().value(QStringLiteral("fontWeight")).toMap()
                          .value(QStringLiteral("customized")).toBool()
                      && weightToggle->property("active").toBool()
                      && weightInherit->property("enabled").toBool(),
                  "bold in Edit customizes the slide and wakes its Inherit");
            scrollIntoView(weightInherit);
            QTest::qWait(60);
            clickAt(weightInherit, weightInherit->width() / 2, weightInherit->height() / 2);
            QTest::qWait(120);
            check(!docs.slideAppearance().value(QStringLiteral("fontWeight")).toMap()
                          .value(QStringLiteral("customized")).toBool()
                      && !weightToggle->property("active").toBool(),
                  "Inherit hands the weight back to the theme");
            clickAt(underlineToggle, underlineToggle->width() / 2,
                    underlineToggle->height() / 2);
            QTest::qWait(120);
            check(docs.slideAppearance().value(QStringLiteral("fontUnderline")).toMap()
                          .value(QStringLiteral("customized")).toBool()
                      && underlineToggle->property("active").toBool()
                      && underlineInherit->property("enabled").toBool(),
                  "underline in Edit customizes the slide and wakes its Inherit");
            scrollIntoView(underlineInherit);
            QTest::qWait(60);
            clickAt(underlineInherit, underlineInherit->width() / 2,
                    underlineInherit->height() / 2);
            QTest::qWait(120);
            check(!docs.slideAppearance().value(QStringLiteral("fontUnderline")).toMap()
                          .value(QStringLiteral("customized")).toBool()
                      && !underlineToggle->property("active").toBool(),
                  "Inherit hands underline back to the theme");
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
        // The box is placed by the canvas gesture (and the same controller
        // verb the gesture commits, used here so the fixture is not a
        // pointer-maths test): on a slide whose theme places, an edit
        // records a placement CUSTOMIZATION — one unit, with the box menu's
        // Reset as its revert — while an extra box keeps editing its
        // authored rectangle exactly as it always did. BX.3 removed the
        // numeric X/Y/W/H band; nothing below types a coordinate.
        check(docs.selectTextBox(0) &&
                  docs.setThemePlacement(house, 250.0, 150.0, 1300.0, 700.0),
              "House gains a lyric-box placement");
        auto* guideX = findNamed(window->contentItem(), "centreGuideX");
        auto* guideY = findNamed(window->contentItem(), "centreGuideY");
        check(guideX && guideY && !guideX->isVisible() && !guideY->isVisible()
                  && docs.currentSlideBoxes().at(0).toMap().value(QStringLiteral("x"))
                             .toDouble() == 250.0,
              "the canvas shows the themed rectangle with no resting guides");
        check(docs.setTextBoxGeometry(500.0, 150.0, 1300.0, 700.0),
              "an edit on the placed box writes the slide's own spot");
        check(waitFor([&] { return docs.slideAppearance().value(QStringLiteral("placement"))
                                       .toMap().value(QStringLiteral("customized")).toBool(); }),
              "the edit customizes the SLIDE's own spot");
        // Keyboard: Menu opens the box menu on the selected box, and its
        // Reset hands the rectangle back to the theme in one step.
        auto* canvas = findNamed(window->contentItem(), "editCanvas");
        check(canvas != nullptr, "the Edit canvas hosts the box menu");
        if (canvas != nullptr) {
            QMetaObject::invokeMethod(canvas, "openBoxMenuAtSelection");
            QTest::qWait(60);
            QObject* editMenu = window->findChild<QObject*>("editBoxMenu");
            QObject* editReset = window->findChild<QObject*>("resetBoxPlacementMenuItem");
            check(editMenu && editMenu->property("visible").toBool(),
                  "Menu opens the box menu on the selected box");
            check(editReset && editReset->property("enabled").toBool(),
                  "the box menu offers Reset once the slide holds a placement");
            if (editReset != nullptr)
                QMetaObject::invokeMethod(editReset, "clicked");
            QTest::qWait(120);
        }
        check(waitFor([&] { return !docs.slideAppearance().value(QStringLiteral("placement"))
                                          .toMap().value(QStringLiteral("customized")).toBool(); })
                  && docs.currentSlideBoxes().at(0).toMap().value(QStringLiteral("x"))
                             .toDouble() == 250.0,
              "Reset box placement hands the whole rectangle back to the theme in one step");
        check(findNamed(window->contentItem(), "geometryField") == nullptr
                  && findNamed(window->contentItem(), "centerBoxH") == nullptr
                  && findNamed(window->contentItem(), "centerBoxV") == nullptr
                  && findNamed(window->contentItem(), "placementOwner") == nullptr
                  && findNamed(window->contentItem(), "revertPlacementButton") == nullptr,
              "the Edit numeric band, centre helpers and owner row are gone");
        if (canvas != nullptr) {
            // Right-click over the box opens the same menu by mouse.
            const QVariantMap placedBox = docs.currentSlideBoxes().at(0).toMap();
            const double z = canvas->property("zoom").toDouble();
            const double stageX =
                (canvas->width() - canvas->property("stageWidth").toDouble()) / 2.0;
            const double stageY =
                (canvas->height() - canvas->property("stageHeight").toDouble()) / 2.0;
            const QPointF boxPoint = canvas->mapToScene(QPointF(
                stageX + (placedBox.value(QStringLiteral("x")).toDouble()
                          + placedBox.value(QStringLiteral("width")).toDouble() / 2.0) * z,
                stageY + (placedBox.value(QStringLiteral("y")).toDouble()
                          + placedBox.value(QStringLiteral("height")).toDouble() / 2.0) * z));
            QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, boxPoint.toPoint());
            QTest::qWait(60);
            QObject* editMenu = window->findChild<QObject*>("editBoxMenu");
            check(editMenu && editMenu->property("visible").toBool(),
                  "right-click over the Edit box opens the box menu");
            if (editMenu != nullptr)
                QMetaObject::invokeMethod(editMenu, "close");
            QTest::qWait(60);
        }
        // An extra box is geometry, never a placement: the theme drives
        // exactly one box, and everything else keeps authored behaviour.
        // The stack already carries whatever earlier fixtures left, so
        // everything here is relative to the box COUNT and the NEW id —
        // never an assumed index.
        auto* newBox = findNamed(window->contentItem(), "newBoxButton");
        // The stack row sits at the bottom of the Edit page's Flickable; at
        // this window height it can be under the viewport edge, so an
        // operator flicks the page up to reach it. The numeric band's
        // removal (BX.3) changed how much room the row has.
        if (newBox != nullptr) {
            QQuickItem* page = newBox->parentItem();
            while (page != nullptr && !page->property("contentY").isValid())
                page = page->parentItem();
            if (page != nullptr) {
                page->setProperty("contentY", page->property("contentHeight").toReal()
                                                  - page->property("height").toReal());
                QTest::qWait(80);
            }
        }
        const QStringList before = [&docs]() {
            QStringList ids;
            for (const QVariant& box : docs.currentSlideBoxes())
                ids.append(box.toMap().value(QStringLiteral("id")).toString());
            return ids;
        }();
        // Existing documents may contain several authored boxes. New box is
        // now intentionally disabled when one text box exists; construct this
        // legacy geometry fixture through the controller instead of the UI.
        check(newBox && !newBox->isEnabled(), "New box does not duplicate an existing text box");
        docs.addTextBox();
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
        check(docs.setTextBoxGeometry(90.0, 100.0, 400.0, 300.0)
                  && docs.currentSlideBoxes().at(newIndex).toMap()
                             .value(QStringLiteral("x")).toDouble() == 90.0
                  && !docs.slideAppearance().value(QStringLiteral("placement"))
                            .toMap().value(QStringLiteral("customized")).toBool(),
              "an edit on an unplaced box changes its AUTHORED rectangle and customizes "
              "no placement");
        // The stack tools survive the numeric band's removal.
        auto* backButton = findNamed(window->contentItem(), "boxBackButton");
        auto* forwardButton = findNamed(window->contentItem(), "boxForwardButton");
        check(newBox && backButton && forwardButton
                  && backButton->property("enabled").toBool(),
              "the stack commands stay reachable beside the canvas");
        // Leave the fixture exactly the story it told before this block.
        docs.removeCurrentTextBox();
        QTest::qWait(80);
        check(docs.currentSlideBoxes().size() == before.size() && docs.clearThemePlacement(house),
              "the extra box goes and House stops placing — nothing leaks to the captures");
        window->setProperty("page", "show");
        QTest::qWait(60);
    }

    // --- task BX.1: both editors move the edge pair a corner owns ---------
    // The reviewed implementation computed only the dragged left/top edge in
    // BOTH pages, so a bottom-right drag left the rectangle unchanged. These
    // call the pages' own gesture functions with a fixed captured rectangle:
    // they fail against that arithmetic and pass only when both editors use
    // the shared BoxTransform.
    {
        const auto close = [](const QRectF& a, const QRectF& b) {
            return std::abs(a.x() - b.x()) < 0.01 && std::abs(a.y() - b.y()) < 0.01 &&
                   std::abs(a.width() - b.width()) < 0.01 &&
                   std::abs(a.height() - b.height()) < 0.01;
        };
        // Edit canvas: set the captured rectangle and invoke its gesture.
        window->setProperty("page", "edit");
        QTest::qWait(160);
        auto* canvas = findNamed(window->contentItem(), "editCanvas");
        check(canvas != nullptr, "the Edit canvas is reachable for the corner matrix");
        if (canvas != nullptr) {
            const auto drag = [canvas](const char* mode, double dx, double dy) {
                canvas->setProperty("dragMode", QString::fromLatin1(mode));
                canvas->setProperty("startBox", QRectF(200, 150, 400, 300));
                canvas->setProperty("boxX", 200.0);
                canvas->setProperty("boxY", 150.0);
                canvas->setProperty("boxW", 400.0);
                canvas->setProperty("boxH", 300.0);
                QMetaObject::invokeMethod(canvas, "applyDrag", Q_ARG(QVariant, dx),
                                          Q_ARG(QVariant, dy));
                return QRectF(canvas->property("boxX").toDouble(),
                              canvas->property("boxY").toDouble(),
                              canvas->property("boxW").toDouble(),
                              canvas->property("boxH").toDouble());
            };
            check(close(drag("br", 50.0, 40.0), QRectF(200, 150, 450, 340)),
                  "Edit: a bottom-right drag changes BOTH dimensions");
            check(close(drag("tr", -30.0, 20.0), QRectF(200, 170, 370, 280)),
                  "Edit: top-right moves the right and top edges");
            check(close(drag("bl", 30.0, -20.0), QRectF(230, 150, 370, 280)),
                  "Edit: bottom-left moves the left and bottom edges");
            check(close(drag("tl", 30.0, 20.0), QRectF(230, 170, 370, 280)),
                  "Edit: top-left moves the left and top edges");
            canvas->setProperty("dragMode", QString());
        }
        // Themes placement overlay: same captured rectangle, its own gesture.
        window->setProperty("page", "themes");
        QTest::qWait(160);
        auto* themePage = findNamed(window->contentItem(), "themeEditorPage");
        check(themePage != nullptr, "the Theme Editor page is reachable for the corner matrix");
        if (themePage != nullptr) {
            const auto drag = [themePage](const char* mode, double dx, double dy) {
                themePage->setProperty("dragMode", QString::fromLatin1(mode));
                themePage->setProperty("anchorX", 200.0);
                themePage->setProperty("anchorY", 150.0);
                themePage->setProperty("anchorW", 400.0);
                themePage->setProperty("anchorH", 300.0);
                QMetaObject::invokeMethod(themePage, "moveGesture", Q_ARG(QVariant, dx),
                                          Q_ARG(QVariant, dy));
                return QRectF(themePage->property("penX").toDouble(),
                              themePage->property("penY").toDouble(),
                              themePage->property("penW").toDouble(),
                              themePage->property("penH").toDouble());
            };
            check(close(drag("br", 50.0, 40.0), QRectF(200, 150, 450, 340)),
                  "Themes: a bottom-right drag changes BOTH dimensions");
            check(close(drag("tr", -30.0, 20.0), QRectF(200, 170, 370, 280)),
                  "Themes: top-right moves the right and top edges");
            check(close(drag("bl", 30.0, -20.0), QRectF(230, 150, 370, 280)),
                  "Themes: bottom-left moves the left and bottom edges");
            check(close(drag("tl", 30.0, 20.0), QRectF(230, 170, 370, 280)),
                  "Themes: top-left moves the left and top edges");
            themePage->setProperty("dragMode", QString());
            themePage->setProperty("placing", false);
        }
        window->setProperty("page", "show");
        QTest::qWait(80);
    }

    // --- task BX.2: gesture lifetime and reachable handles -----------------
    // The gesture surface reaches one handle radius past the fitted content
    // (so the outer half of an edge handle is live), and a held gesture is
    // owned by the target captured at press: a click without motion, a
    // navigation, a selection change or a cancellation writes nothing and
    // never retargets a new box.
    {
        window->setProperty("page", "edit");
        QTest::qWait(160);
        auto* canvas = findNamed(window->contentItem(), "editCanvas");
        auto* gesture = findNamed(window->contentItem(), "editGestureArea");
        check(canvas != nullptr && gesture != nullptr,
              "the Edit canvas and its gesture surface exist");
        if (canvas != nullptr && gesture != nullptr) {
            const double stageWidth = canvas->property("stageWidth").toDouble();
            check(gesture->x() <= -7.5 && gesture->y() <= -7.5 &&
                      gesture->width() >= stageWidth + 15.0 &&
                      gesture->height() >= canvas->property("stageHeight").toDouble() + 15.0,
                  "the Edit gesture surface covers the outer half of edge handles");
        }
        const int index = docs.currentBox();
        if (canvas != nullptr && index >= 0) {
            const QVariantMap before = docs.currentSlideBoxes().at(index).toMap();
            const auto setup = [canvas, &before](const char* mode) {
                canvas->setProperty("dragMode", QString::fromLatin1(mode));
                canvas->setProperty("startBox",
                                    QRectF(before.value(QStringLiteral("x")).toDouble(),
                                           before.value(QStringLiteral("y")).toDouble(),
                                           before.value(QStringLiteral("width")).toDouble(),
                                           before.value(QStringLiteral("height")).toDouble()));
                canvas->setProperty("boxX", before.value(QStringLiteral("x")).toDouble());
                canvas->setProperty("boxY", before.value(QStringLiteral("y")).toDouble());
                canvas->setProperty("boxW", before.value(QStringLiteral("width")).toDouble());
                canvas->setProperty("boxH", before.value(QStringLiteral("height")).toDouble());
                QMetaObject::invokeMethod(canvas, "captureDragIdentity");
            };
            const auto widthOf = [&docs, index] {
                return docs.currentSlideBoxes().at(index).toMap()
                    .value(QStringLiteral("width")).toDouble();
            };

            setup("move");
            QMetaObject::invokeMethod(canvas, "endDrag");
            check(canvas->property("dragMode").toString().isEmpty() &&
                      widthOf() == before.value(QStringLiteral("width")).toDouble(),
                  "a click without motion writes nothing");

            setup("br");
            QMetaObject::invokeMethod(canvas, "applyDrag", Q_ARG(QVariant, 50.0),
                                      Q_ARG(QVariant, 40.0));
            QMetaObject::invokeMethod(canvas, "endDrag");
            check(widthOf() == before.value(QStringLiteral("width")).toDouble() + 50.0,
                  "a completed corner gesture commits one rectangle");
            docs.undo();
            check(widthOf() == before.value(QStringLiteral("width")).toDouble(),
                  "and one undo restores it exactly");

            if (docs.slides().size() > 1) {
                setup("br");
                QMetaObject::invokeMethod(canvas, "applyDrag", Q_ARG(QVariant, 50.0),
                                          Q_ARG(QVariant, 40.0));
                const int originalSlide = docs.currentSlide();
                docs.selectSlide((originalSlide + 1) % docs.slides().size());
                check(canvas->property("dragMode").toString().isEmpty(),
                      "navigation during a held drag cancels it");
                docs.selectSlide(originalSlide);
                QTest::qWait(60);
                check(widthOf() == before.value(QStringLiteral("width")).toDouble(),
                      "and the dragged slide keeps its geometry");
            }

            if (docs.currentSlideBoxes().size() > 1) {
                setup("br");
                QMetaObject::invokeMethod(canvas, "applyDrag", Q_ARG(QVariant, 50.0),
                                          Q_ARG(QVariant, 40.0));
                docs.selectTextBox(index == 0 ? 1 : 0);
                check(canvas->property("dragMode").toString().isEmpty() &&
                          widthOf() == before.value(QStringLiteral("width")).toDouble(),
                      "a selection change mid-drag cancels and never retargets");
            }
        }

        // Overlap: the SELECTED box's corner beats another box's body, so a
        // resize never selects or edits the item underneath it.
        if (canvas != nullptr && index >= 0 && docs.addTextBox()) {
            const QVariantMap first = docs.currentSlideBoxes().at(index).toMap();
            const int added = docs.currentBox();
            check(docs.setTextBoxGeometry(first.value(QStringLiteral("x")).toDouble() - 40.0,
                                          first.value(QStringLiteral("y")).toDouble() - 40.0,
                                          200.0, 200.0),
                  "a second box is laid over the first one's corner");
            docs.selectTextBox(index);
            QTest::qWait(40);
            const double zoom = canvas->property("zoom").toDouble();
            const double px = first.value(QStringLiteral("x")).toDouble() * zoom + 2.0;
            const double py = first.value(QStringLiteral("y")).toDouble() * zoom + 2.0;
            QVariant mode;
            QMetaObject::invokeMethod(canvas, "hitTest", Q_RETURN_ARG(QVariant, mode),
                                      Q_ARG(QVariant, px), Q_ARG(QVariant, py));
            check(mode.toString() == QStringLiteral("tl"),
                  "an overlapping body does not steal the selected box's handle");
            docs.selectTextBox(added);
            docs.removeCurrentTextBox();
            docs.selectTextBox(index);
        }

        // --- Themes: the overlay surface and the same identity rule --------
        window->setProperty("page", "themes");
        QTest::qWait(160);
        auto* themePage = findNamed(window->contentItem(), "themeEditorPage");
        auto* themeGesture = findNamed(window->contentItem(), "themeEditorGestureArea");
        check(themePage != nullptr && themeGesture != nullptr,
              "the Theme Editor page and its gesture surface exist");
        if (themeGesture != nullptr) {
            check(themeGesture->x() <= -7.5 && themeGesture->y() <= -7.5,
                  "the Themes gesture surface covers the outer half of edge handles");
        }
        const auto placementOf = [&docs](const QString& id) {
            for (const QVariant& row : docs.themes()) {
                if (row.toMap().value(QStringLiteral("id")).toString() == id) {
                    return row.toMap().value(QStringLiteral("placement")).toMap();
                }
            }
            return QVariantMap();
        };
        if (themePage != nullptr && docs.createBlankTheme(QStringLiteral("BX gesture"))) {
            QString scratchId;
            QString otherId;
            for (const QVariant& row : docs.themes()) {
                const QVariantMap map = row.toMap();
                if (map.value(QStringLiteral("name")).toString() == QStringLiteral("BX gesture")) {
                    scratchId = map.value(QStringLiteral("id")).toString();
                } else if (otherId.isEmpty()) {
                    otherId = map.value(QStringLiteral("id")).toString();
                }
            }
            themePage->setProperty("selectedId", scratchId);
            themePage->setProperty("selectedView", QString());
            QTest::qWait(80);
            QMetaObject::invokeMethod(themePage, "beginGesture",
                                      Q_ARG(QVariant, QStringLiteral("br")));
            QMetaObject::invokeMethod(themePage, "moveGesture", Q_ARG(QVariant, 50.0),
                                      Q_ARG(QVariant, 40.0));
            QMetaObject::invokeMethod(themePage, "endGesture");
            check(placementOf(scratchId).value(QStringLiteral("width")).toDouble() == 1650.0,
                  "a completed Themes gesture writes the placement once");
            docs.undo();
            check(placementOf(scratchId).isEmpty(),
                  "and one undo takes it back");

            if (!otherId.isEmpty()) {
                QMetaObject::invokeMethod(themePage, "beginGesture",
                                          Q_ARG(QVariant, QStringLiteral("br")));
                QMetaObject::invokeMethod(themePage, "moveGesture", Q_ARG(QVariant, 50.0),
                                          Q_ARG(QVariant, 40.0));
                QMetaObject::invokeMethod(themePage, "selectTheme", Q_ARG(QVariant, otherId));
                QMetaObject::invokeMethod(themePage, "endGesture");
                check(placementOf(scratchId).isEmpty() && placementOf(otherId).isEmpty(),
                      "a theme switch cancels the gesture and writes to neither theme");
            }
            themePage->setProperty("selectedId", scratchId);
            themePage->setProperty("selectedView", QString());
            QTest::qWait(60);
            QMetaObject::invokeMethod(themePage, "beginGesture",
                                      Q_ARG(QVariant, QStringLiteral("br")));
            QMetaObject::invokeMethod(themePage, "moveGesture", Q_ARG(QVariant, 50.0),
                                      Q_ARG(QVariant, 40.0));
            QMetaObject::invokeMethod(themePage, "selectView",
                                      Q_ARG(QVariant, QStringLiteral("scripture")));
            QMetaObject::invokeMethod(themePage, "endGesture");
            check(placementOf(scratchId).isEmpty(),
                  "a view switch cancels the gesture rather than committing under it");
            themePage->setProperty("selectedView", QString());
            check(docs.deleteTheme(scratchId), "the scratch theme goes");
        }
        window->setProperty("page", "show");
        QTest::qWait(80);
    }

    // --- task BX.4: the real-pointer matrix --------------------------------
    // Every corner and both centre axes, driven by actual press/move/release
    // at the three supported widths. The display-scale leg is the same
    // binary re-run with QT_SCALE_FACTOR=1.5 and 2 (the capture matrix's
    // rule); the assertions are in authored units, so they hold at any
    // device scale.
    {
        window->setProperty("page", "edit");
        QTest::qWait(160);
        auto* canvas = findNamed(window->contentItem(), "editCanvas");
        check(canvas != nullptr, "the Edit canvas is reachable for the pointer matrix");
        if (canvas != nullptr) {
            // Earlier blocks may have flicked the page to its stack row; the
            // pointer matrix needs the canvas's top edge on screen.
            QQuickItem* page = canvas->parentItem();
            while (page != nullptr && !page->property("contentY").isValid())
                page = page->parentItem();
            if (page != nullptr) {
                page->setProperty("contentY", 0.0);
                QTest::qWait(80);
            }
            const auto scenePoint = [canvas](double ax, double ay) {
                const double z = canvas->property("zoom").toDouble();
                const double sx =
                    (canvas->width() - canvas->property("stageWidth").toDouble()) / 2.0;
                const double sy =
                    (canvas->height() - canvas->property("stageHeight").toDouble()) / 2.0;
                return canvas->mapToScene(QPointF(sx + ax * z, sy + ay * z));
            };
            // A real drag delivers several position events. A single jump
            // followed by release misses the parent Flickable stealing the
            // MouseArea's grab after its drag threshold is crossed.
            const auto movePointer = [&](const QPointF& from, const QPointF& to) {
                for (int step = 1; step <= 12; ++step) {
                    const QPointF point = from + (to - from) * (step / 12.0);
                    QTest::mouseMove(window, point.toPoint(), 10);
                    QTest::qWait(5);
                }
            };
            const auto drag = [&](const QPointF& from, const QPointF& to) {
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
                movePointer(from, to);
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to.toPoint());
                QTest::qWait(80);
            };
            const auto boxAt = [&docs](int index) {
                return docs.currentSlideBoxes().at(index).toMap();
            };
            // Pointer coordinates are whole device pixels, so a drag's
            // authored travel carries up to one pixel of rounding at the
            // current zoom. The helper tests prove the exact arithmetic; this
            // matrix proves the gesture wiring and direction.
            const auto near = [](double a, double b) { return std::abs(a - b) <= 4.0; };
            const auto setBox = [&docs](double x, double y, double w, double h) {
                docs.selectTextBox(0);
                return docs.setTextBoxGeometry(x, y, w, h);
            };

            for (const int width : {1000, 1380, 1680}) {
                window->resize(width, 800);
                QTest::qWait(140);
                canvas->setProperty("selected", true);
                const double z = canvas->property("zoom").toDouble();
                const auto corner = [&](const char* mode, double dx, double dy,
                                        double ex, double ey, double ew, double eh) {
                    check(setBox(200.0, 150.0, 400.0, 300.0),
                          "the pointer matrix sets its captured rectangle");
                    QTest::qWait(60);
                    const QPointF from = mode == std::string("tl") ? scenePoint(200, 150)
                                        : mode == std::string("tr") ? scenePoint(600, 150)
                                        : mode == std::string("bl") ? scenePoint(200, 450)
                                                                    : scenePoint(600, 450);
                    drag(from, from + QPointF(dx * z, dy * z));
                    const QVariantMap box = boxAt(0);
                    check(near(box.value(QStringLiteral("x")).toDouble(), ex) &&
                              near(box.value(QStringLiteral("y")).toDouble(), ey) &&
                              near(box.value(QStringLiteral("width")).toDouble(), ew) &&
                              near(box.value(QStringLiteral("height")).toDouble(), eh),
                          (std::string("real pointer: ") + mode +
                           " resizes both of its edges at " + std::to_string(width))
                              .c_str());
                    docs.undo();
                    QTest::qWait(60);
                };
                // Cross the scrolling parent's threshold at every zoom,
                // rather than stopping before a real drag can be stolen.
                corner("br", 180, 140, 200, 150, 580, 440);
                corner("tr", -150, 120, 200, 270, 250, 180);
                corner("bl", 150, -120, 350, 150, 250, 180);
                corner("tl", 150, 120, 350, 270, 250, 180);

                // Centre axes: the body drag snaps exactly and shows its
                // guide; beyond the tolerance it stays free. Size never moves.
                check(setBox(200.0, 150.0, 400.0, 300.0), "the snap fixture is set");
                QTest::qWait(60);
                {
                    const QPointF from = scenePoint(400, 300);
                    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                                      from.toPoint());
                    movePointer(from, scenePoint(960, 300));
                    QTest::qWait(40);
                    auto* guideX = findNamed(window->contentItem(), "centreGuideX");
                    check(canvas->property("snappedX").toBool() && guideX
                              && guideX->isVisible(),
                          "a body drag near the horizontal centre snaps and shows its guide");
                    if (guideX != nullptr) {
                        check(QLineF(guideX->mapToScene(QPointF(0, 0)),
                                     scenePoint(960, 0)).length() <= 0.5 &&
                                  QLineF(guideX->mapToScene(QPointF(0, guideX->height())),
                                         scenePoint(960, 1080)).length() <= 0.5,
                              "the vertical guide lies on the slide centre from top to bottom");
                    }
                    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                        scenePoint(960, 300).toPoint());
                    QTest::qWait(80);
                    const QVariantMap box = boxAt(0);
                    check(near(box.value(QStringLiteral("x")).toDouble(), 760.0) &&
                              near(box.value(QStringLiteral("y")).toDouble(), 150.0) &&
                              near(box.value(QStringLiteral("width")).toDouble(), 400.0) &&
                              near(box.value(QStringLiteral("height")).toDouble(), 300.0),
                          "the horizontal snap lands exactly on 960 and keeps the size");
                    docs.undo();
                    QTest::qWait(60);
                }
                {
                    const QPointF from = scenePoint(400, 300);
                    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                                      from.toPoint());
                    movePointer(from, scenePoint(400, 540));
                    QTest::qWait(40);
                    auto* guideY = findNamed(window->contentItem(), "centreGuideY");
                    check(canvas->property("snappedY").toBool() && guideY
                              && guideY->isVisible(),
                          "a body drag near the vertical centre snaps and shows its guide");
                    if (guideY != nullptr) {
                        check(QLineF(guideY->mapToScene(QPointF(0, 0)),
                                     scenePoint(0, 540)).length() <= 0.5 &&
                                  QLineF(guideY->mapToScene(QPointF(guideY->width(), 0)),
                                         scenePoint(1920, 540)).length() <= 0.5,
                              "the horizontal guide lies on the slide centre from left to right");
                    }
                    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                        scenePoint(400, 540).toPoint());
                    QTest::qWait(80);
                    const QVariantMap box = boxAt(0);
                    check(near(box.value(QStringLiteral("y")).toDouble(), 390.0) &&
                              near(box.value(QStringLiteral("height")).toDouble(), 300.0),
                          "the vertical snap lands exactly on 540 and keeps the size");
                    docs.undo();
                    QTest::qWait(60);
                }
                {
                    // Both axes at once: the intersection snaps to the centre
                    // and both guides show while the body is held there.
                    const QPointF from = scenePoint(400, 300);
                    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                                      from.toPoint());
                    movePointer(from, scenePoint(960, 540));
                    QTest::qWait(40);
                    auto* guideX = findNamed(window->contentItem(), "centreGuideX");
                    auto* guideY = findNamed(window->contentItem(), "centreGuideY");
                    check(canvas->property("snappedX").toBool() &&
                              canvas->property("snappedY").toBool() && guideX &&
                              guideY && guideX->isVisible() && guideY->isVisible(),
                          "a body drag onto the intersection snaps both axes");
                    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                        scenePoint(960, 540).toPoint());
                    QTest::qWait(80);
                    const QVariantMap box = boxAt(0);
                    check(near(box.value(QStringLiteral("x")).toDouble(), 760.0) &&
                              near(box.value(QStringLiteral("y")).toDouble(), 390.0) &&
                              near(box.value(QStringLiteral("width")).toDouble(), 400.0) &&
                              near(box.value(QStringLiteral("height")).toDouble(), 300.0),
                          "the intersection lands exactly on 960/540 and keeps the size");
                    docs.undo();
                    QTest::qWait(60);
                }
                {
                    // A small rectangle: its body still moves without a
                    // corner taking the press, and the size is preserved.
                    check(setBox(200.0, 150.0, 80.0, 80.0),
                          "the small fixture is set");
                    QTest::qWait(60);
                    const QPointF from = scenePoint(240, 190);
                    drag(from, from + QPointF(100 * z, 60 * z));
                    const QVariantMap box = boxAt(0);
                    check(near(box.value(QStringLiteral("x")).toDouble(), 300.0) &&
                              near(box.value(QStringLiteral("y")).toDouble(), 210.0) &&
                              near(box.value(QStringLiteral("width")).toDouble(), 80.0) &&
                              near(box.value(QStringLiteral("height")).toDouble(), 80.0),
                          "a small box's body drags and keeps its size");
                    docs.undo();
                    QTest::qWait(60);
                }
                {
                    check(setBox(200.0, 150.0, 400.0, 300.0),
                          "the tolerance fixture is set");
                    QTest::qWait(60);
                    const QPointF from = scenePoint(400, 300);
                    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                                      from.toPoint());
                    QTest::mouseMove(window, scenePoint(1000, 300).toPoint(), 20);
                    QTest::qWait(40);
                    check(!canvas->property("snappedX").toBool(),
                          "a drag beyond the tolerance stays free");
                    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                        scenePoint(1000, 300).toPoint());
                    QTest::qWait(80);
                    check(near(boxAt(0).value(QStringLiteral("x")).toDouble(), 800.0),
                          "and the pointer's travel is kept");
                    docs.undo();
                    QTest::qWait(60);
                }

                // A handle at the slide edge: the outer half is live.
                check(setBox(0.0, 0.0, 1920.0, 1080.0), "the edge fixture is set");
                QTest::qWait(60);
                {
                    const QPointF from = scenePoint(0, 0) - QPointF(3, 3);
                    drag(from, from + QPointF(30 * z, 20 * z));
                    const QVariantMap box = boxAt(0);
                    check(near(box.value(QStringLiteral("x")).toDouble(), 30.0) &&
                              near(box.value(QStringLiteral("y")).toDouble(), 20.0) &&
                              near(box.value(QStringLiteral("width")).toDouble(), 1890.0) &&
                              near(box.value(QStringLiteral("height")).toDouble(), 1060.0),
                          "a corner at the slide edge is reachable and resizes");
                    docs.undo();
                    QTest::qWait(60);
                }

                // Overlap: the selected box's handle wins over another box.
                check(setBox(200.0, 150.0, 400.0, 300.0), "the overlap fixture is set");
                if (docs.addTextBox()) {
                    const int added = docs.currentBox();
                    docs.setTextBoxGeometry(150.0, 100.0, 300.0, 300.0);
                    docs.selectTextBox(0);
                    QTest::qWait(60);
                    const QPointF from = scenePoint(200, 150);
                    drag(from, from + QPointF(30 * z, 20 * z));
                    const QVariantMap first = boxAt(0);
                    const QVariantMap second = boxAt(added);
                    check(near(first.value(QStringLiteral("x")).toDouble(), 230.0) &&
                              near(first.value(QStringLiteral("width")).toDouble(), 370.0) &&
                              near(second.value(QStringLiteral("x")).toDouble(), 150.0) &&
                              near(second.value(QStringLiteral("width")).toDouble(), 300.0),
                          "an overlapping body does not steal the selected handle");
                    docs.undo();
                    docs.selectTextBox(added);
                    docs.removeCurrentTextBox();
                    docs.selectTextBox(0);
                    QTest::qWait(60);
                }

                // Cancel: Escape during a held drag writes nothing.
                check(setBox(200.0, 150.0, 400.0, 300.0), "the cancel fixture is set");
                QTest::qWait(60);
                {
                    const QPointF from = scenePoint(600, 450);
                    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                                      from.toPoint());
                    QTest::mouseMove(window, (from + QPointF(50 * z, 40 * z)).toPoint(), 20);
                    QTest::qWait(40);
                    QTest::keyClick(window, Qt::Key_Escape);
                    QTest::qWait(40);
                    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                        (from + QPointF(50 * z, 40 * z)).toPoint());
                    QTest::qWait(80);
                    const QVariantMap box = boxAt(0);
                    check(near(box.value(QStringLiteral("width")).toDouble(), 400.0) &&
                              near(box.value(QStringLiteral("height")).toDouble(), 300.0),
                          "Escape cancels a held drag without a write");
                }
            }
            // Leave the document as the block found it: the last fixture wrote
            // a placement customization, and later captures read the theme's
            // rectangle.
            if (docs.slideAppearance().value(QStringLiteral("placement"))
                    .toMap().value(QStringLiteral("customized")).toBool()) {
                docs.revertCurrentSlideField(QStringLiteral("placement"));
            }
            window->resize(1680, 1020);
            QTest::qWait(80);
        }
        window->setProperty("page", "show");
        QTest::qWait(80);
    }

    // --- task BX.4: the Themes overlay, driven by real pointers -----------
    // Songs and Bible are the same overlay over two records. Press/move/release
    // through all four corners, the centre snap and its guide, an edge corner
    // and an Escape cancellation, then check the record the view actually owns.
    {
        window->setProperty("page", "themes");
        QTest::qWait(200);
        auto* themePage = findNamed(window->contentItem(), "themeEditorPage");
        auto* gesture = findNamed(window->contentItem(), "themeEditorGestureArea");
        check(themePage != nullptr && gesture != nullptr,
              "the Theme Editor overlay is reachable for the pointer matrix");
        if (themePage != nullptr && gesture != nullptr &&
            docs.createBlankTheme(QStringLiteral("BX pointer"))) {
            QString id;
            for (const QVariant& row : docs.themes()) {
                if (row.toMap().value(QStringLiteral("name")).toString()
                    == QStringLiteral("BX pointer")) {
                    id = row.toMap().value(QStringLiteral("id")).toString();
                }
            }
            themePage->setProperty("selectedId", id);
            themePage->setProperty("selectedView", QString());
            QTest::qWait(100);

            QQuickItem* layer = gesture->parentItem();
            check(layer != nullptr, "the Themes overlay has its placement layer");
            const auto scenePoint = [layer](double ax, double ay) {
                const double z = layer->width() / 1920.0;
                return layer->mapToScene(QPointF(ax * z, ay * z));
            };
            const auto drag = [&](const QPointF& from, const QPointF& to) {
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
                QTest::mouseMove(window, to.toPoint(), 20);
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to.toPoint());
                QTest::qWait(100);
            };
            const auto near = [](double a, double b) { return std::abs(a - b) <= 4.0; };
            const auto placementOf = [&docs](const QString& themeId) {
                for (const QVariant& row : docs.themes()) {
                    if (row.toMap().value(QStringLiteral("id")).toString() == themeId) {
                        return row.toMap().value(QStringLiteral("placement")).toMap();
                    }
                }
                return QVariantMap();
            };

            for (const QString view : {QString(), QStringLiteral("scripture")}) {
                themePage->setProperty("selectedView", view);
                // A Bible view with a size and no box inherits the theme's
                // rectangle, so every corner starts from the same fixture.
                if (!view.isEmpty()) {
                    check(docs.applyThemeContext(
                              id, QStringLiteral("scripture"),
                              QVariantMap{{QStringLiteral("fontSize"), 32.0}}),
                          "the Bible view speaks without a box");
                }
                const auto record = [&]() {
                    return view.isEmpty()
                            ? placementOf(id)
                            : docs.themeContext(id, QStringLiteral("scripture"));
                };
                struct Corner {
                    const char* name;
                    bool left;
                    bool top;
                    double dx;
                    double dy;
                    double ex;
                    double ey;
                    double ew;
                    double eh;
                };
                const Corner corners[] = {
                    {"top-left", true, true, 50.0, 40.0, 250.0, 190.0, 350.0, 260.0},
                    {"top-right", false, true, -50.0, 40.0, 200.0, 190.0, 350.0, 260.0},
                    {"bottom-left", true, false, 50.0, -40.0, 250.0, 150.0, 350.0, 260.0},
                    {"bottom-right", false, false, -50.0, -40.0, 200.0, 150.0, 350.0, 260.0},
                    {"bottom-right out", false, false, 50.0, 40.0, 200.0, 150.0, 450.0, 340.0},
                };
                for (const Corner& c : corners) {
                    check(docs.setThemePlacement(id, 200.0, 150.0, 400.0, 300.0),
                          "the pointer fixture is set");
                    QTest::qWait(90);
                    const QRectF shown = themePage->property("shownBox").toRectF();
                    check(near(shown.x(), 200.0) && near(shown.y(), 150.0) &&
                              near(shown.width(), 400.0) && near(shown.height(), 300.0),
                          "the overlay shows the fixture's box");
                    const double ax = c.left ? shown.x() : shown.x() + shown.width();
                    const double ay = c.top ? shown.y() : shown.y() + shown.height();
                    drag(scenePoint(ax, ay), scenePoint(ax + c.dx, ay + c.dy));
                    const QVariantMap r = record();
                    check(near(r.value(QStringLiteral("x")).toDouble(), c.ex) &&
                              near(r.value(QStringLiteral("y")).toDouble(), c.ey) &&
                              near(r.value(QStringLiteral("width")).toDouble(), c.ew) &&
                              near(r.value(QStringLiteral("height")).toDouble(), c.eh),
                          (std::string(view.isEmpty() ? "Themes/Songs: " : "Themes/Bible: ") +
                           c.name + " drags its two edges").c_str());
                    docs.undo();
                    QTest::qWait(80);
                    if (!view.isEmpty()) {
                        check(!docs.themeContext(id, QStringLiteral("scripture"))
                                   .value(QStringLiteral("boxSet")).toBool(),
                              "one undo removes the Bible drag's context box");
                    }
                }
                // Centre axis: the body drag snaps and shows its guide.
                check(docs.setThemePlacement(id, 200.0, 150.0, 400.0, 300.0),
                      "the snap fixture is set");
                QTest::qWait(80);
                {
                    const QPointF from = scenePoint(400, 300);
                    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                                      from.toPoint());
                    QTest::mouseMove(window, scenePoint(960, 300).toPoint(), 20);
                    QTest::qWait(50);
                    auto* guideX = findNamed(window->contentItem(),
                                             "themeEditorCentreGuideX");
                    check(themePage->property("snappedX").toBool() && guideX
                              && guideX->isVisible(),
                          "Themes: a body drag near the centre shows its guide");
                    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                        scenePoint(960, 300).toPoint());
                    QTest::qWait(100);
                }
                const QVariantMap snapped = view.isEmpty()
                        ? placementOf(id)
                        : docs.themeContext(id, QStringLiteral("scripture"));
                check(near(snapped.value(QStringLiteral("x")).toDouble(), 760.0) &&
                          near(snapped.value(QStringLiteral("width")).toDouble(), 400.0),
                      "Themes: the snap lands exactly on the centre and keeps the width");
                docs.undo();
                QTest::qWait(80);
            }
            themePage->setProperty("selectedView", QString());

            // An edge corner: press just outside the painted corner.
            check(docs.setThemePlacement(id, 0.0, 0.0, 1920.0, 1080.0),
                  "the edge fixture is set");
            QTest::qWait(100);
            {
                const QPointF from = scenePoint(0, 0) - QPointF(3, 3);
                drag(from, from + QPointF(30 * (layer->width() / 1920.0),
                                          20 * (layer->width() / 1920.0)));
                const QVariantMap p = placementOf(id);
                check(near(p.value(QStringLiteral("x")).toDouble(), 30.0) &&
                          near(p.value(QStringLiteral("y")).toDouble(), 20.0) &&
                          near(p.value(QStringLiteral("width")).toDouble(), 1890.0),
                      "Themes: a corner at the slide edge is reachable and resizes");
                docs.undo();
                QTest::qWait(80);
            }

            // Escape during a held drag writes nothing.
            check(docs.setThemePlacement(id, 200.0, 150.0, 400.0, 300.0),
                  "the cancel fixture is set");
            QTest::qWait(80);
            {
                const QPointF from = scenePoint(600, 450);
                QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
                QTest::mouseMove(window, (from + QPointF(50, 40)).toPoint(), 20);
                QTest::qWait(50);
                QTest::keyClick(window, Qt::Key_Escape);
                QTest::qWait(50);
                QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                                    (from + QPointF(50, 40)).toPoint());
                QTest::qWait(80);
                check(near(placementOf(id).value(QStringLiteral("width")).toDouble(), 400.0),
                      "Themes: Escape cancels a held drag without a write");
            }
            check(docs.deleteTheme(id), "the pointer theme goes");
        }
        window->setProperty("page", "show");
        QTest::qWait(80);
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
        // Legacy shell/empty-state images. Populated worker-backed five-page
        // acceptance uses control_set_ui_tests + OPENSHOW_CONTROL_CAPTURE;
        // these snapshots alone do not establish Bible preview fidelity.
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
            QVariantMap shownBox;
            for (const QVariant& box : docs.currentSlideBoxes()) {
                if (box.toMap().value(QStringLiteral("primary")).toBool()) {
                    shownBox = box.toMap();
                }
            }
            if (shownBox.isEmpty() || shownBox.value(QStringLiteral("x")).toDouble() != 240.0) {
                std::fprintf(stderr,
                             "DEBUG capture box x=%.1f y=%.1f w=%.1f h=%.1f placed=%d worn=%s\n",
                             shownBox.value(QStringLiteral("x")).toDouble(),
                             shownBox.value(QStringLiteral("y")).toDouble(),
                             shownBox.value(QStringLiteral("width")).toDouble(),
                             shownBox.value(QStringLiteral("height")).toDouble(),
                             shownBox.value(QStringLiteral("placed")).toBool() ? 1 : 0,
                             worn->property("text").toString().toLocal8Bit().constData());
                const QVariantMap placement =
                    docs.slideAppearance().value(QStringLiteral("placement")).toMap();
                std::fprintf(stderr, "DEBUG placement customized=%d value x=%.1f\n",
                             placement.value(QStringLiteral("customized")).toBool() ? 1 : 0,
                             placement.value(QStringLiteral("value")).toMap()
                                 .value(QStringLiteral("x")).toDouble());
            }
            check(gradientBadge->property("visible").toBool()
                      && sizeBadge->property("visible").toBool()
                      && !colourBadge->property("visible").toBool()
                      && worn->property("text").toString() == QStringLiteral("House")
                      && findNamed(window->contentItem(), "placementOwner") == nullptr
                      && shownBox.value(QStringLiteral("x")).toDouble() == 240.0
                      && shownBox.value(QStringLiteral("y")).toDouble() == 620.0
                      && shownBox.value(QStringLiteral("width")).toDouble() == 1440.0
                      && shownBox.value(QStringLiteral("height")).toDouble() == 320.0,
                  "the edit capture carries exactly the two badges and the worn name, "
                  "with the theme's placed rectangle on the canvas and no ownership row");
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
            const QRectF shown = editor != nullptr
                    ? editor->property("shownBox").toRectF() : QRectF();
            check(editor != nullptr && editor->property("selectedId").toString() == houseId
                      && std::abs(shown.x() - 240.0) < 0.01
                      && std::abs(shown.y() - 620.0) < 0.01
                      && std::abs(shown.width() - 1440.0) < 0.01
                      && std::abs(shown.height() - 320.0) < 0.01
                      && findNamed(window->contentItem(), "themeEditorPlaceX") == nullptr
                      && findNamed(window->contentItem(), "themeEditorContextX") == nullptr,
                  "selecting the placing theme draws its rectangle on the sample with no "
                  "numeric rows, and a re-shown page still reads the record");
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
