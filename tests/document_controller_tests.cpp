/*
 * Task 3.3 fixtures: DocumentController — the document session behind the
 * lyric panel. The tests drive the very class QML binds to (Qt Core only),
 * in a private scratch directory: song and slide editing, the debounced
 * text commit, navigation, the scene.prepare hand-off (compiled slides
 * must satisfy scene::fromJson), service save/load, the task 2.3 startup
 * rule (autosave wins over the saved service), and undo/redo with
 * navigation re-anchoring.
 *
 * The debounce timer is never waited on: flushText() is the synchronous
 * edge of the same commit path, so the fixtures stay deterministic.
 */

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <boost/json.hpp>

#include <QCoreApplication>
#include <QFileInfo>
#include <QTimer>

#include <sys/stat.h>

#include "openshow/domain/documents.h"
#include "openshow/domain/storage.h"
#include "openshow/scene/scene.h"

#include "DocumentController.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::domain::Autosave;
using openshow::ui::DocumentController;

// A QVariantList of {id,...} maps from the controller, read as strings.
std::vector<std::string> mapIds(const QVariantList& list) {
    std::vector<std::string> ids;
    for (const QVariant& entry : list) {
        ids.push_back(entry.toMap().value(QStringLiteral("id")).toString().toStdString());
    }
    return ids;
}

// The drawer reads rows by entryId/showName; the fixtures want the same.
std::vector<std::string> mapEntryIds(const QVariantList& list) {
    std::vector<std::string> ids;
    for (const QVariant& entry : list) {
        ids.push_back(
            entry.toMap().value(QStringLiteral("entryId")).toString().toStdString());
    }
    return ids;
}

std::vector<std::string> entryShowNames(const QVariantList& list) {
    std::vector<std::string> names;
    for (const QVariant& entry : list) {
        names.push_back(
            entry.toMap().value(QStringLiteral("showName")).toString().toStdString());
    }
    return names;
}

// --- editing, navigation and the presentation hand-off -------------------

void exerciseEditingAndPresentation(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    check(docs.songs().isEmpty() && docs.currentSongId().isEmpty() &&
              docs.currentSlideText().isEmpty(),
          "an empty scratch session starts empty");
    check(!docs.dirty(), "empty is not dirty");

    check(!docs.createSong(QStringLiteral("   ")), "a blank song name is refused");
    check(docs.songs().isEmpty(), "a refused create changed nothing");

    check(docs.createSong(QStringLiteral("Amazing Grace")), "creating a song works");
    check(docs.songs().size() == 1 && docs.currentSongId() ==
              QString::fromStdString(mapIds(docs.songs()).front()),
          "the new song is current");
    check(docs.createSong(QStringLiteral("Be Thou Now O My Rememberance")),
          "a second song can exist");
    const std::vector<std::string> songIds = mapIds(docs.songs());
    check(docs.songs().size() == 2 && songIds[0] != songIds[1],
          "both songs are listed with distinct ids");
    // Creating a song makes it current; the fixture edits the first one.
    const QString firstSongId = QString::fromStdString(songIds[0]);
    docs.selectSong(firstSongId);
    check(docs.currentSongId() == firstSongId && docs.slides().isEmpty(),
          "selecting a song with no slides shows an empty slide list");

    check(docs.addSlide() && docs.addSlide(), "slides append to the current song");
    check(docs.slides().size() == 2 && docs.currentSlide() == 1,
          "the new slide becomes current");
    check(docs.slides().at(0).toMap().value(QStringLiteral("preview")).toString() ==
              QStringLiteral("(empty)"),
          "an empty slide previews as (empty)");

    // Typing commits once, on flush, not per keystroke.
    docs.setSlideText(QStringLiteral("Amazing grace! How sweet"));
    check(docs.currentSlideText().isEmpty(), "typing is pending until the commit");
    docs.setSlideText(QStringLiteral("Amazing grace! How sweet\nthe word"));
    docs.flushText();
    check(docs.currentSlideText() ==
              QStringLiteral("Amazing grace! How sweet\nthe word"),
          "the flushed text is the last typed value");
    check(docs.dirty(), "editing marks unsaved work");

    // The scene hand-off: what QML feeds worker.presentSceneJson must be a
    // valid wire scene naming this slide with these lines.
    const QString json = docs.currentSceneJson();
    boost::system::error_code parseError;
    const boost::json::value parsed = boost::json::parse(json.toStdString(), parseError);
    openshow::scene::Scene scene;
    std::string sceneError;
    check(!parseError && !json.isEmpty() &&
              openshow::scene::fromJson(parsed, &scene, &sceneError),
          "currentSceneJson is a scene the wire validator accepts");
    check(scene.showId == firstSongId.toStdString() &&
              scene.slideId == docs.slides().at(1).toMap()
                                   .value(QStringLiteral("id")).toString().toStdString() &&
              scene.items.size() == 1 &&
              scene.items[0].lines ==
                  std::vector<std::string>{"Amazing grace! How sweet", "the word"},
          "the compiled scene names the right slide with the typed lines");

    docs.selectSong(QString::fromStdString(songIds[1]));
    check(docs.currentSongId() == QString::fromStdString(songIds[1]) &&
              docs.slides().isEmpty(),
          "selecting the second song shows its own (empty) slide list");
    docs.selectSong(firstSongId);
    check(docs.currentSongId() == firstSongId && docs.slides().size() == 2 &&
              docs.currentSlide() == 0,
          "selecting a song re-anchors navigation to its first slide");

    docs.stepSlide(9);
    check(docs.currentSlide() == 1, "stepSlide clamps at the last slide");
    docs.stepSlide(-9);
    check(docs.currentSlide() == 0, "stepSlide clamps at the first slide");

    // Per-slide present (clicking a slide during a service).
    const QString slideId =
        docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();
    const QString perSlide = docs.sceneJsonForSlide(slideId);
    openshow::scene::Scene compiled;
    check(!perSlide.isEmpty() &&
              openshow::scene::fromJson(boost::json::parse(perSlide.toStdString()),
                                        &compiled, &sceneError) &&
              compiled.slideId == slideId.toStdString(),
          "sceneJsonForSlide compiles the named slide");
    check(docs.sceneJsonForSlide(QStringLiteral("slide999")).isEmpty(),
          "an unknown slide compiles to nothing");
    check(!docs.currentSceneJson().isEmpty(), "the current slide still compiles");
}

// The commit must not echo into the editor: QML re-reads the field only
// when navigation, undo or a load changes the slide underneath it.
void exerciseCommitSilence(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Silence"));
    docs.addSlide();
    int textFieldRefreshes = 0;
    QObject::connect(&docs, &DocumentController::slideTextChanged,
                     [&textFieldRefreshes] { ++textFieldRefreshes; });
    docs.setSlideText(QStringLiteral("word"));
    docs.flushText();
    check(textFieldRefreshes == 0, "a text commit does not rewrite the editor field");
    docs.setSlideText(QStringLiteral("more words"));
    docs.flushText();
    docs.addSlide();
    check(textFieldRefreshes == 1, "moving to another slide does refresh the field");
    docs.selectSlide(0);
    check(textFieldRefreshes == 2 && docs.currentSlideText() == QStringLiteral("more words"),
          "navigation reports the field's new content");
}

// --- undo / redo with re-anchoring ----------------------------------------

void exerciseUndoRedo(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Climbing"));
    check(!docs.canRedo(), "nothing to redo yet");
    docs.addSlide();
    docs.addSlide();
    docs.setSlideText(QStringLiteral("to the top"));
    docs.flushText();
    check(docs.canUndo(), "edits can be undone");

    docs.undo();  // the text
    check(docs.currentSlideText().isEmpty() && docs.canRedo(), "undo reverted the text");
    docs.redo();
    check(docs.currentSlideText() == QStringLiteral("to the top"), "redo restored the text");

    docs.undo();  // the text again (it sits on the redo stack's tip)
    docs.undo();  // the second addSlide — the current slide no longer exists
    check(docs.slides().size() == 1 && docs.currentSlide() == 0,
          "undoing the current slide re-anchors navigation, it does not dangle");
    docs.undo();  // the first addSlide — no slides left
    check(docs.slides().isEmpty() && docs.currentSlide() == -1,
          "with the last slide undone navigation points nowhere");
    docs.undo();  // the creation
    check(docs.songs().isEmpty() && docs.currentSongId().isEmpty(),
          "undoing the creation leaves an empty session");
    check(docs.currentSceneJson().isEmpty(),
          "nothing selected means nothing to present");
    docs.redo();
    docs.redo();
    check(docs.songs().size() == 1 && docs.slides().size() == 1 &&
              docs.currentSlide() == 0,
          "redo brings the song and its first slide back, navigation re-anchors");
}

// --- authored text boxes (task 4.3) ------------------------------------------

void exerciseGeometryEdits(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Framed"));
    docs.addSlide();

    // The slides rows carry the box so the edit page can bind to it.
    QVariantMap first = docs.slides().at(0).toMap();
    check(first.value(QStringLiteral("x")).toDouble() == 160.0 &&
              first.value(QStringLiteral("y")).toDouble() == 100.0 &&
              first.value(QStringLiteral("width")).toDouble() == 1600.0 &&
              first.value(QStringLiteral("height")).toDouble() == 880.0,
          "a slide row carries the default text box");

    check(docs.setSlideGeometry(240.0, 120.0, 1400.0, 800.0),
          "the controller accepts a box for the current slide");
    first = docs.slides().at(0).toMap();
    check(first.value(QStringLiteral("x")).toDouble() == 240.0 &&
              first.value(QStringLiteral("width")).toDouble() == 1400.0,
          "the edited box reaches the slides view");

    // One call is one history entry (a drag commits once, on release).
    docs.undo();
    first = docs.slides().at(0).toMap();
    check(first.value(QStringLiteral("x")).toDouble() == 160.0 &&
              first.value(QStringLiteral("height")).toDouble() == 880.0,
          "one undo reverts a whole drag to the box it started from");
    docs.redo();
    first = docs.slides().at(0).toMap();
    check(first.value(QStringLiteral("x")).toDouble() == 240.0,
          "redo restores the committed box");

    check(!docs.setSlideGeometry(0.0, 0.0, -10.0, 100.0),
          "a degenerate box is refused by the controller");
    check(docs.status().contains(QStringLiteral("text box")),
          "the refusal explains itself on the status line");
    first = docs.slides().at(0).toMap();
    check(first.value(QStringLiteral("width")).toDouble() == 1400.0,
          "the refused edit did not touch the box");

    DocumentController bare(nullptr, root / "bare");
    check(!bare.setSlideGeometry(10.0, 10.0, 100.0, 100.0),
          "with no current slide there is nothing to resize");
}

// --- save, load and the startup rule ---------------------------------------

void exercisePersistence(const std::filesystem::path& root) {
    const std::filesystem::path service = root / "service";
    {
        DocumentController docs(nullptr, root);
        check(docs.save(), "saving an empty session succeeds");
        check(!docs.dirty(), "a save clears the dirty flag");
        check(std::filesystem::exists(service / "service.json"),
              "the service package appeared where it is shown");

        docs.createSong(QStringLiteral("Saved Hymn"));
        docs.addSlide();
        docs.setSlideText(QStringLiteral("saved words"));
        docs.flushText();
        check(docs.dirty(), "editing after a save dirties again");
        check(docs.save() && !docs.dirty(), "saving again clears it");
        check(!Autosave::exists(root / "autosave"),
              "a save removes the autosave package - nothing is unsaved");

        docs.addSlide();  // unsaved on purpose: autosave must protect it
        check(Autosave::exists(root / "autosave"),
              "an edit after the save re-creates the autosave");
    }
    {
        // Startup: autosave (the unsaved addSlide) wins over the service.
        DocumentController docs(nullptr, root);
        check(docs.songs().size() == 1 && docs.slides().size() == 2 &&
                  docs.currentSlideText() == QStringLiteral("saved words"),
              "recovery brings back the unsaved slide");
        check(docs.dirty(), "recovered work is dirty");
        check(docs.status().contains(QStringLiteral("recovered")),
              "the status says where the data came from");
    }
    {
        // After the recovered work is saved, a later startup loads the
        // service cleanly.
        DocumentController docs(nullptr, root);
        check(docs.save(), "saving the recovered work succeeds");
    }
    {
        DocumentController docs(nullptr, root);
        check(docs.songs().size() == 1 && docs.slides().size() == 2 &&
                  docs.currentSlideText() == QStringLiteral("saved words") && !docs.dirty() &&
                  !docs.canUndo(),
              "a clean startup loads the saved service, undirtied and unhistoried");
        check(docs.status().contains(QStringLiteral("loaded")),
              "the status names the loaded service");

        // load() discards local changes and their autosave trail.
        docs.removeCurrentSong();
        check(docs.songs().isEmpty(), "removal before the reload");
        check(Autosave::exists(root / "autosave"), "the removal is protected by autosave");
        check(docs.load(), "load succeeds");
        check(docs.songs().size() == 1 && !docs.dirty() && !docs.canUndo() &&
                  !Autosave::exists(root / "autosave"),
              "load restores the file and clears history, dirty and autosave");
    }
}

// --- task 6.12 (review R7): the package owns the bytes after a save --------

// R7's exact sequence with real files: import, an interrupted session
// (scratch present, nothing saved), restart into recovery — where every
// recovered asset names a file inside the scratch tree — save, and then the
// scratch AND the original import are gone. The session must still present,
// edit, undo, redo and save again. Before this task the second save failed
// with "has no readable file": the live model, the undo snapshots and the
// autosave ticks all still named deleted files.
void exerciseRecoveredAssetOwnership(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "originals");
    const std::filesystem::path original = root / "originals" / "still.jpg";
    {
        std::ofstream out(original, std::ios::binary);
        out << "REALSTILLBYTES";
    }
    const std::filesystem::path scratch = root / "autosave";
    const std::filesystem::path package = root / "service";
    QString slideId;
    QString assetId;
    QString owned;

    {
        // The interrupted session: import and attach; the autosave protects
        // the edit and the app dies before any save.
        DocumentController docs(nullptr, root);
        check(docs.createSong(QStringLiteral("Petition")), "adopt: the song exists");
        check(docs.addSlide(), "adopt: the slide exists");
        slideId = QString::fromStdString(
            docs.slides().at(0).toMap().value(QStringLiteral("id")).toString().toStdString());
        check(docs.importMediaFile(QString::fromStdString(original.string())),
              "adopt: the import is recorded");
        assetId = QString::fromStdString(
            docs.assets().at(0).toMap().value(QStringLiteral("id")).toString().toStdString());
        check(docs.setSlideBackground(assetId), "adopt: the slide takes the background");
        check(docs.assets().at(0).toMap().value(QStringLiteral("sourcePath")).toString() ==
                  QString::fromStdString(original.string()),
              "adopt: before any save the asset names the original");
        check(Autosave::exists(scratch),
              "adopt: the unsaved edit is protected in the scratch tree");
    }
    {
        DocumentController docs(nullptr, root);
        check(docs.status().contains(QStringLiteral("recovered")),
              "adopt: the new session recovers the scratch");
        const QString scratchPath =
            docs.assets().at(0).toMap().value(QStringLiteral("sourcePath")).toString();
        check(scratchPath.contains(QStringLiteral("/autosave/")) && QFileInfo::exists(scratchPath),
              "adopt: the recovered asset names a file inside the scratch tree");

        // One edit BEFORE the save, and an undo, so both history stacks hold
        // a pre-adoption snapshot when the adoption runs — undo/redo after
        // the save is exactly where the deleted scratch used to resurface.
        docs.setSlideText(QStringLiteral("O for a thousand tongues"));
        docs.flushText();
        docs.undo();

        // The signal the worker controller is wired to: the save's report
        // of what moved, captured here so the emission itself is a tested
        // contract rather than a line of main.cpp nobody watches.
        QVariantMap emitted;
        int emissions = 0;
        QObject::connect(&docs, &DocumentController::assetsAdopted,
                         [&emitted, &emissions](const QVariantMap& moved) {
                             emitted = moved;
                             ++emissions;
                         });

        check(docs.save(), "adopt: saving the recovered session succeeds");
        owned = docs.assets().at(0).toMap().value(QStringLiteral("sourcePath")).toString();
        check(emissions == 1 && emitted.value(scratchPath).toString() == owned,
              "adopt: the save announced the move scratch -> package, once");
        const std::filesystem::path packaged =
            (package / "assets" / (assetId.toStdString() + "-still.jpg")).lexically_normal();
        check(owned == QString::fromStdString(packaged.string()),
              "adopt: the model now names the package's own copy by name");
        check(!Autosave::exists(scratch), "adopt: the save removed the scratch tree");
        check(docs.currentSlideText().isEmpty(), "adopt: the pre-save edit is still undone");
        docs.redo();
        check(docs.assets().at(0).toMap().value(QStringLiteral("sourcePath")).toString() == owned &&
                  docs.currentSlideText() == QStringLiteral("O for a thousand tongues"),
              "adopt: redo lands on an adopted snapshot, not the deleted scratch path");
        check(Autosave::exists(scratch),
              "adopt: the tick right after the save can copy the bytes it names - before the "
              "fix this was the first thing to fail, silently, against a deleted file");

        // The worst housekeeping: the original import is deleted too. This
        // is the half of R7 the operator notices — the live document used
        // to keep depending on this file until a reload.
        std::filesystem::remove(original);

        // Present: the compiled scene names the owned copy and nothing else.
        openshow::scene::Scene scene;
        boost::system::error_code parseError;
        const boost::json::value parsed =
            boost::json::parse(docs.sceneJsonForSlide(slideId).toStdString(), parseError);
        std::string sceneError;
        check(!parseError && openshow::scene::fromJson(parsed, &scene, &sceneError) &&
                  scene.background.assetPath == packaged.string(),
              "adopt: the compiled scene names the package's copy");
        check(scene.background.assetPath.find("autosave") == std::string::npos,
              "adopt: no live reference into deleted scratch storage");

        // Edit again: the autosave must be able to protect the new work —
        // before this task its tick copied from the scratch file IT had
        // just deleted, and the operator's edits went unguarded in silence.
        docs.setSlideText(QStringLiteral("O for a thousand tongues, as"));
        docs.flushText();
        check(Autosave::exists(scratch),
              "adopt: after the save, edits are protected by a writable autosave again");

        docs.undo();
        check(docs.assets().at(0).toMap().value(QStringLiteral("sourcePath")).toString() == owned,
              "adopt: undo did not resurrect a deleted scratch path");
        check(Autosave::exists(scratch), "adopt: the post-undo tick can still write");
        docs.redo();
        check(docs.save(),
              "adopt: the second save succeeds - before task 6.12 it failed 'has no readable file'");
        check(emissions == 1,
              "adopt: a save that moves nothing emits no adoption signal - caches are not "
              "disturbed by a save with nothing to rebind");

        for (const QVariant& entry : docs.assets()) {
            const QString path = entry.toMap().value(QStringLiteral("sourcePath")).toString();
            check(path.startsWith(QString::fromStdString(package.string())) &&
                      QFileInfo::exists(path),
                  "adopt: every asset names a file inside the package, and it exists");
        }
    }
    {
        DocumentController docs(nullptr, root);
        check(!docs.dirty() && docs.assets().size() == 1 &&
                  docs.assets().at(0).toMap().value(QStringLiteral("sourcePath")).toString() ==
                      owned,
              "adopt: a clean startup keeps naming the package's copy");
        check(docs.assets().at(0).toMap()
                  .value(QStringLiteral("sourcePath"))
                  .toString()
                  .indexOf(QStringLiteral("autosave")) == -1,
              "adopt: and the whole session names no scratch path at all");
    }
}

// --- task 4.2: playlist, drawer search and the media library -----------

void exercisePlaylistAndLibrary(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    check(docs.playlists().size() == 1 && !docs.currentPlaylistId().isEmpty() &&
              docs.playlistEntries().isEmpty(),
          "a session seeds one (empty) service playlist");

    docs.createSong("Amazing Grace");
    docs.addSlide();
    docs.setSlideText("amazing grace how sweet the sound");
    docs.flushText();
    docs.createSong("Cafe Grace");
    docs.addSlide();
    docs.setSlideText("searching me O Lord");
    docs.flushText();
    docs.createSong("Third Song");
    const std::vector<std::string> songIds = mapIds(docs.songs());
    const QString first = QString::fromStdString(songIds[0]);
    const QString second = QString::fromStdString(songIds[1]);
    const QString third = QString::fromStdString(songIds[2]);

    check(docs.addShowToPlaylist(first) && docs.playlistEntries().size() == 1,
          "the drawer can add a show to the running order");
    check(!docs.addShowToPlaylist(QStringLiteral("show999")) &&
              docs.playlistEntries().size() == 1,
          "a show that does not exist is refused");
    check(docs.insertShowInPlaylist(second, 0) &&
              entryShowNames(docs.playlistEntries())[0] == "Cafe Grace",
          "insert lands at the requested index");

    // The drawer's Enter: the match lands after the current show when the
    // playlist already runs it.
    docs.selectSong(first);
    check(docs.insertShowAfterCurrentShow(third) &&
              entryShowNames(docs.playlistEntries()) ==
                  std::vector<std::string>{"Cafe Grace", "Amazing Grace", "Third Song"},
          "Enter inserts after the current show");

    // Reordering (the drag handle calls this; toIndex is the position after
    // the move), and overshoot clamps.
    const QString thirdEntry = QString::fromStdString(mapEntryIds(docs.playlistEntries())[2]);
    check(docs.movePlaylistEntry(thirdEntry, 0) &&
              entryShowNames(docs.playlistEntries())[0] == "Third Song",
          "a row moves to the front");
    check(docs.movePlaylistEntry(thirdEntry, 99) &&
              entryShowNames(docs.playlistEntries())[2] == "Third Song",
          "overshoot clamps to the end");

    // Arrangement edits undo and redo like everything else.
    const int before = static_cast<int>(docs.playlistEntries().size());
    const QString frontEntry = QString::fromStdString(mapEntryIds(docs.playlistEntries())[0]);
    check(docs.removePlaylistEntry(frontEntry) &&
              docs.playlistEntries().size() == before - 1,
          "a row can be removed");
    docs.undo();
    check(docs.playlistEntries().size() == before, "undo brings the row back");

    // More than one playlist: rows belong to the current one.
    const QString service = docs.currentPlaylistId();
    check(docs.createPlaylist("Evening") && docs.playlistEntries().isEmpty(),
          "a new playlist becomes current, starting empty");
    const QString evening = docs.currentPlaylistId();
    check(docs.addShowToPlaylist(second), "the new playlist takes its own rows");
    docs.selectPlaylist(service);
    check(docs.currentPlaylistId() == service &&
              docs.playlistEntries().size() == static_cast<std::size_t>(before),
          "each playlist keeps its own running order");

    // The domain rule: removing a show drops the entries naming it.
    docs.selectSong(second);
    check(docs.removeCurrentSong(), "the show is removed");
    check(docs.playlistEntries().size() == 2,
          "playlist rows naming a deleted show go with it");

    // Media library: registered by file name, undoable, removable.
    const QString mediaPath = QString::fromStdWString((root / "loop.mp4").wstring());
    check(docs.importMediaFile(mediaPath) && docs.assets().size() == 1 &&
              docs.assets().front().toMap().value(QStringLiteral("name")) ==
                  QStringLiteral("loop.mp4"),
          "import registers media under its file name");
    docs.undo();
    check(docs.assets().isEmpty(), "undo takes the import back");
    docs.redo();
    check(docs.assets().size() == 1, "redo restores it");
    const QString assetId =
        docs.assets().front().toMap().value(QStringLiteral("id")).toString();
    check(docs.removeAsset(assetId) && docs.assets().isEmpty(), "and it can be removed");

    // The drawer's find: title and lyrics both answer (the scoring itself
    // is library_search_tests' job).
    QVariantList hits = docs.searchShows(QStringLiteral("sweet"));
    check(hits.size() == 1 &&
              hits.front().toMap().value(QStringLiteral("name")) ==
                  QStringLiteral("Amazing Grace"),
          "lyrics answer a search");
    check(docs.searchShows(QStringLiteral("grace")).size() == 1,
          "the deleted song drops out of the library");
    check(docs.searchShows(QString()).size() == docs.songs().size(),
          "a blank query is the whole library");

    // And the arrangement survives the round trip.
    check(docs.save(), "the service saves");
    {
        DocumentController reloaded(nullptr, root);
        check(reloaded.playlists().size() == 2 && reloaded.playlistEntries().size() == 2,
              "playlists and their rows persist through save/load");
    }
}

// --- the style panel (task 4.4) ---------------------------------------------

// The compiled wire scene for one slide, through the very path QML uses.
openshow::scene::Scene compiledFor(DocumentController& docs, const QString& slideId) {
    openshow::scene::Scene scene;
    const QString json = docs.sceneJsonForSlide(slideId);
    if (!json.isEmpty()) {
        (void)openshow::scene::fromJson(boost::json::parse(json.toStdString()), &scene);
    }
    return scene;
}

// --- several text boxes on one slide (task 6.17a) ---------------------------

std::string readManifest(const std::filesystem::path& file) {
    std::ifstream in(file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A service written by hand, because this half of the split has no editor for
// a second box yet (that is 6.17a2). A file is how a multi-box service really
// reaches this controller: from another build, or from this one later.
//   slide1 — three boxes: the default one, a title strip, a credit corner
//   slide2 — no boxes at all: a backdrop with no words
void writeMultiBoxService(const std::filesystem::path& service) {
    std::filesystem::create_directories(service);
    std::ofstream out(service / "service.json");
    // One raw literal with its own delimiter: this manifest is JSON, so it
    // contains no ')' at all, and a custom closing sequence means a stray
    // paren in the data cannot silently end the string early. (Two earlier
    // attempts at this fixture did exactly that and produced malformed JSON
    // the loader refused for the wrong reason.)
    out << R"svc({"format":2,"nextOrdinal":90,"shows":[{"id":"show1","name":"Stacked","notes":"","defaultStyleId":"","slides":[{"id":"slide1","group":"","items":[{"id":"text1","lines":["Verse line"],"x":160,"y":100,"width":1600,"height":880},{"id":"text2","lines":["TITLE"],"x":100,"y":40,"width":800,"height":120},{"id":"text3","lines":["photo: someone"],"x":1400,"y":980,"width":460,"height":60}],"notes":"","styleId":"","backgroundAssetId":""},{"id":"slide2","group":"","items":[],"notes":"","styleId":"","backgroundAssetId":""}]}],"styles":[],"assets":[],"playlists":[]})svc";
    out.close();
}

void exerciseMultiBoxSession(const std::filesystem::path& root) {
    const std::filesystem::path service = root / "service";
    writeMultiBoxService(service);
    DocumentController docs(nullptr, root);
    check(docs.songs().size() == 1, "the multi-box service loads at startup");
    docs.selectSong(QStringLiteral("show1"));
    check(docs.slides().size() == 2, "both of its slides are listed");

    // The row the list paints: the FIRST box, which is the one the canvas and
    // the nudge keys edit until an operator can pick (6.17a2).
    const QVariantMap row1 = docs.slides().at(0).toMap();
    const QString slide1 = row1.value(QStringLiteral("id")).toString();
    check(row1.value(QStringLiteral("preview")).toString() == QStringLiteral("Verse line") &&
              row1.value(QStringLiteral("x")).toDouble() == 160.0 &&
              row1.value(QStringLiteral("width")).toDouble() == 1600.0,
          "a stacked slide previews and positions by its first box");
    const QVariantMap row2 = docs.slides().at(1).toMap();
    const QString slide2 = row2.value(QStringLiteral("id")).toString();
    check(row2.value(QStringLiteral("preview")).toString() == QStringLiteral("(empty)") &&
              row2.value(QStringLiteral("width")).toDouble() == 1600.0,
          "a slide with no boxes still reports a placeable box, not zeros");

    // The inspector shows ONE box's words until an operator can pick (6.17a2),
    // and it shows the first: the same rule the rows and the nudge keys use, so
    // the panel, the list and the canvas cannot be looking at three different
    // boxes on a slide nobody has selected a box on yet.
    docs.selectSlide(0);
    check(docs.currentSlideText() == QStringLiteral("Verse line"),
          "the text inspector shows the FIRST box's words");

    // Search covers every box. An index over box 0 only would tell the operator
    // this show does not contain the credit they typed second — the search
    // result is a claim about the slide, not about its first element.
    check(!docs.searchShows(QStringLiteral("photo: someone")).isEmpty(),
          "a word living only in the third box still finds the show");
    check(docs.searchShows(QStringLiteral("no such wording")).isEmpty(),
          "and the same search is not simply always true");

    // What the worker is handed: every box, in order, at its own size. This is
    // the acceptance for the half — the compiled scene the operator sees off
    // air is the model's stack, not its first element.
    const openshow::scene::Scene stacked = compiledFor(docs, slide1);
    check(stacked.items.size() == 3, "the compiled scene carries all three boxes");
    check(stacked.items[0].lines == std::vector<std::string>{"Verse line"} &&
              stacked.items[1].lines == std::vector<std::string>{"TITLE"} &&
              stacked.items[2].lines == std::vector<std::string>{"photo: someone"},
          "each box's words stay in its own item");
    check(stacked.items[1].x == 100.0 && stacked.items[1].width == 800.0 &&
              stacked.items[2].x == 1400.0 && stacked.items[2].y == 980.0,
          "each box's rectangle travels to the wire unchanged");
    check(compiledFor(docs, slide2).items.empty(),
          "the no-box slide compiles to a scene with no items");

    // Typing edits box 0 and nobody else — the legacy rule, and the reason the
    // old single-box services and the text inspector are untouched by this.
    docs.selectSlide(0);
    docs.setSlideText(QStringLiteral("Verse line two"));
    docs.flushText();
    const openshow::scene::Scene afterTyping = compiledFor(docs, slide1);
    check(afterTyping.items[0].lines == std::vector<std::string>{"Verse line two"} &&
              afterTyping.items[1].lines == std::vector<std::string>{"TITLE"} &&
              afterTyping.items[2].lines == std::vector<std::string>{"photo: someone"},
          "typing edits the first box and leaves the other two alone");

    // Copy and paste a stacked slide. A clip that carried the boxes' ids would
    // write a file no loader accepts, so the paste is checked where it hurts:
    // save it, and read the saved package back.
    docs.selectSlide(0);
    check(docs.copyCurrentSlide() && docs.pasteSlide(), "a stacked slide copies and pastes");
    check(docs.slides().size() == 3, "paste inserted one slide");
    const QString pastedId =
        docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();
    const openshow::scene::Scene pasted = compiledFor(docs, pastedId);
    check(pasted.items.size() == 3 && pasted.items[1].x == 100.0 &&
              pasted.items[2].y == 980.0 &&
              pasted.items[1].lines == std::vector<std::string>{"TITLE"},
          "the pasted slide carries the same stack in the same order");
    check(docs.pasteSlide() && docs.slides().size() == 4, "the same slide pastes a second time");
    const QString pastedTwice =
        docs.slides().at(2).toMap().value(QStringLiteral("id")).toString();
    check(compiledFor(docs, pastedTwice).items.size() == 3,
          "and the second paste is a full copy too");

    check(docs.save(), "the session with pasted stacks saves");
    openshow::domain::Documents fromDisk;
    std::string loadError;
    check(openshow::domain::loadService(&fromDisk, service, &loadError),
          "the saved package with pasted stacks loads (a copied box id would refuse it)");
    if (!fromDisk.showIds().empty()) {
        const openshow::domain::Show* show = fromDisk.findShow("show1");
        std::set<std::string> boxIds;
        std::size_t total = 0;
        for (const openshow::domain::Slide& slide : show->slides) {
            for (const openshow::domain::TextBox& box : slide.items) {
                boxIds.insert(box.id);
                ++total;
            }
        }
        // Three slides of three boxes (the original plus two pastes of it)
        // and one slide with none: nine boxes, and the two pastes cannot have
        // written the original's ids a second time.
        check(total == 9 && boxIds.size() == total,
              "every box in the saved file has its own id, paste included");
    }

    // One undo removes a pasted slide whole — its boxes are not separate steps.
    const int before = static_cast<int>(docs.slides().size());
    docs.undo();
    check(static_cast<int>(docs.slides().size()) == before - 1,
          "one undo removes a pasted stacked slide, boxes included");

    // Pasting a slide with NO boxes does not hand back a slide with a box in
    // it: the copy is the copy, and a slide whose author deleted every box is
    // a backdrop an operator makes on purpose.
    docs.selectSlide(2);  // the box-less original slide, back where it started
    check(docs.currentSlide() == 2, "the box-less original slide is selected by index");
    check(docs.copyCurrentSlide() && docs.pasteSlide(), "the box-less slide copies and pastes");
    // Paste inserts after the source and selects what it made.
    const QString emptyPaste =
        docs.slides().at(docs.currentSlide()).toMap().value(QStringLiteral("id")).toString();
    check(!emptyPaste.isEmpty() && emptyPaste != slide2 &&
              compiledFor(docs, emptyPaste).items.empty(),
          "the pasted box-less slide is a new slide with still no boxes");
}

void exerciseStylePanel(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Consistency"));
    docs.addSlide();
    docs.addSlide();
    docs.selectSlide(0);  // addSlide leaves slide 2 current; start from slide 1
    const QString slideA = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    const QString slideB = docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();

    check(docs.currentStyle().isEmpty() && !docs.currentStyleIsOverride(),
          "a song with no styles has no current style to show");

    check(docs.createStyleFromCurrent(QStringLiteral("Chorus")),
          "a style is created from the current slide's built-in look");
    const QVariantMap chorus = docs.currentStyle();
    const QString chorusId = chorus.value(QStringLiteral("id")).toString();
    check(chorus.value(QStringLiteral("name")).toString() == QStringLiteral("Chorus") &&
              chorus.value(QStringLiteral("textColor")).toString() == QStringLiteral("#ffffff") &&
              chorus.value(QStringLiteral("backgroundColor")).toString() ==
                  QStringLiteral("#000000") &&
              chorus.value(QStringLiteral("fontSize")).toDouble() == 0.0 &&
              chorus.value(QStringLiteral("align")).toString() == QStringLiteral("center") &&
              chorus.value(QStringLiteral("lineSpacing")).toDouble() == 1.25 &&
              docs.currentStyleIsOverride(),
          "the new style wears the built-in look and overrides only this slide");
    check(docs.styles().size() == 1, "the styles view lists the new style");

    check(docs.setShowDefaultStyle(chorusId), "the style can become the show's default");
    docs.selectSlide(1);
    check(!docs.currentStyleIsOverride() &&
              docs.currentStyle().value(QStringLiteral("id")).toString() == chorusId,
          "the second slide follows the show default with no override of its own");

    check(docs.editStyle(chorusId, QStringLiteral("Chorus"), QStringLiteral("#ff0000"),
                         QStringLiteral("#101010"), QStringLiteral("DejaVu Serif"), 72.0,
                         QStringLiteral("left"), 1.5),
          "editing the style once is accepted");
    const openshow::scene::Scene a = compiledFor(docs, slideA);
    const openshow::scene::Scene b = compiledFor(docs, slideB);
    check(a.items.size() == 1 && b.items.size() == 1 &&
              a.items[0].color == 0xFF0000u && b.items[0].color == 0xFF0000u &&
              a.items[0].align == "left" && b.items[0].align == "left" &&
              a.items[0].lineSpacing == 1.5 && b.items[0].lineSpacing == 1.5 &&
              a.items[0].fontSize == 72.0 && b.items[0].fontSize == 72.0,
          "one edit reaches every slide following the style identically");

    docs.undo();
    check(compiledFor(docs, slideA).items[0].color == 0xFFFFFFu &&
              compiledFor(docs, slideB).items[0].color == 0xFFFFFFu,
          "one undo returns both slides to the pre-edit look");
    docs.redo();
    check(compiledFor(docs, slideB).items[0].color == 0xFF0000u,
          "redo restyles both again");

    // Per-slide override: fork the followed style for one slide.
    check(docs.createStyleFromCurrent(QStringLiteral("Whisper")),
          "a slide can fork the style it follows into its own");
    const QString whisperId = docs.currentStyle().value(QStringLiteral("id")).toString();
    check(!whisperId.isEmpty() && whisperId != chorusId && docs.currentStyleIsOverride(),
          "the fork overrides only this slide");
    check(docs.editStyle(whisperId, QStringLiteral("Whisper"), QStringLiteral("#00ff00"),
                         QStringLiteral("#000000"), QString(), 0.0, QStringLiteral("right"),
                         1.25),
          "the fork edits independently");
    check(compiledFor(docs, slideB).items[0].color == 0x00FF00u &&
              compiledFor(docs, slideB).items[0].align == "right" &&
              compiledFor(docs, slideA).items[0].color == 0xFF0000u,
          "the override changes only its own slide");
    docs.undo();  // the fork's appearance back
    check(compiledFor(docs, slideB).items[0].color == 0xFF0000u,
          "undoing the fork edit returns the slide to the shared look");
    docs.undo();  // the fork itself back (creation + override were one entry)
    check(docs.styles().size() == 1 && !docs.currentStyleIsOverride() &&
              compiledFor(docs, slideB).items[0].color == 0xFF0000u,
          "undoing the fork removes the extra style and hands the slide back");

    // Explicitly point a slide at a style, then hand it back (empty id).
    check(docs.applyStyleToCurrentSlide(chorusId) && docs.currentStyleIsOverride(),
          "a slide can be pinned to a style by id");
    check(docs.applyStyleToCurrentSlide(QString()) && !docs.currentStyleIsOverride() &&
              docs.currentStyle().value(QStringLiteral("id")).toString() == chorusId,
          "the empty id follows the show default again");

    // Refusals name themselves and leave everything alone.
    check(!docs.editStyle(QStringLiteral("style999"), QStringLiteral("Ghost"),
                          QStringLiteral("#ffffff"), QStringLiteral("#000000"), QString(), 0.0,
                          QStringLiteral("center"), 1.25) &&
              docs.status().contains(QStringLiteral("edit style")),
          "editing an unknown style is refused on the status line");
    check(!docs.editStyle(chorusId, QStringLiteral("   "), QStringLiteral("#ffffff"),
                          QStringLiteral("#000000"), QString(), 0.0,
                          QStringLiteral("center"), 1.25),
          "a blank name is refused before anything is recorded");
    check(!docs.applyStyleToCurrentSlide(QStringLiteral("style999")) &&
              !docs.currentStyleIsOverride(),
          "an unknown style id is refused and the follow-show state survives");
    check(!docs.setShowDefaultStyle(QStringLiteral("style999")) &&
              docs.currentStyle().value(QStringLiteral("id")).toString() == chorusId,
          "an unknown show default is refused too");
    check(!docs.applyStyleToCurrentSlide(QStringLiteral("Chorus")) &&
              !docs.currentStyleIsOverride(),
          "styles are addressed by id — a name is refused and the state survives");

    // Layering: the background image lands on the current slide and its
    // compiled scene, and undo takes it back.
    const QString mediaPath = QString::fromStdWString((root / "backdrop.jpg").wstring());
    check(docs.importMediaFile(mediaPath) && docs.assets().size() == 1,
          "a media file is imported for the background picker");
    const QString assetId =
        docs.assets().front().toMap().value(QStringLiteral("id")).toString();
    check(docs.setSlideBackground(assetId), "the current slide gets the image behind its words");
    check(docs.slides().at(1).toMap().value(QStringLiteral("backgroundAssetId")).toString() ==
              assetId,
          "the slide row carries the background for the picker");
    check(compiledFor(docs, slideB).background.hasImage,
          "the compiled scene draws the background image");
    check(docs.setSlideBackground(QString()) &&
              !compiledFor(docs, slideB).background.hasImage,
          "the empty id clears the background back to the style's colour");
    check(!docs.setSlideBackground(QStringLiteral("asset999")),
          "an unknown asset is refused for a background");

    DocumentController bare(nullptr, root / "bare");
    check(!bare.createStyleFromCurrent(QStringLiteral("Orphan")) &&
              !bare.setSlideBackground(QString()) &&
              !bare.editStyle(QStringLiteral("style1"), QStringLiteral("x"),
                              QStringLiteral("#fff"), QStringLiteral("#000"), QString(), 0.0,
                              QStringLiteral("center"), 1.25),
          "with no current slide the style panel has nothing to act on");
}

// --- slide clipboard --------------------------------------------------------

void exerciseClipboard(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Copycat"));
    docs.addSlide();
    docs.addSlide();
    docs.selectSlide(0);  // addSlide leaves the newest slide current
    const QString sourceId = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();

    check(!docs.canPaste(), "the clipboard starts empty");
    check(!docs.pasteSlide() && docs.status().contains(QStringLiteral("clipboard")),
          "pasting with an empty clipboard is refused and explains itself");

    // Give slide 1 a full presentation: words, a box, a pinned style and
    // a background image.
    docs.setSlideText(QStringLiteral("Amazing grace"));
    check(docs.setSlideGeometry(240.0, 120.0, 1400.0, 800.0),
          "the source slide carries a box");
    check(docs.createStyleFromCurrent(QStringLiteral("Bright")),
          "the source slide gets a style of its own");
    const QString styleId = docs.currentStyle().value(QStringLiteral("id")).toString();
    check(docs.editStyle(styleId, QStringLiteral("Bright"), QStringLiteral("#ffd23f"),
                         QStringLiteral("#101828"), QString(), 64.0, QStringLiteral("left"),
                         1.5),
          "the style is edited into a distinctive look");
    const QString mediaPath = QString::fromStdWString((root / "backdrop.jpg").wstring());
    check(docs.importMediaFile(mediaPath) && docs.assets().size() == 1,
          "a background image is imported");
    const QString assetId = docs.assets().front().toMap().value(QStringLiteral("id")).toString();
    check(docs.setSlideBackground(assetId), "the source slide gets the background");

    check(docs.copyCurrentSlide(), "copy captures the current slide");
    check(docs.canPaste(), "copy fills the clipboard");

    check(docs.pasteSlide(), "paste is accepted");
    check(docs.currentSlide() == 1, "the pasted slide is inserted after the source and selected");
    const QVariantList rows = docs.slides();
    check(rows.size() == 3, "paste inserts exactly one slide");
    const QVariantMap pasted = rows.at(1).toMap();
    const QString pastedId = pasted.value(QStringLiteral("id")).toString();
    check(!pastedId.isEmpty() && pastedId != sourceId, "the pasted slide is a fresh slide");
    check(pasted.value(QStringLiteral("preview")).toString() == QStringLiteral("Amazing grace"),
          "the pasted slide carries the lines");
    check(pasted.value(QStringLiteral("x")).toDouble() == 240.0 &&
              pasted.value(QStringLiteral("width")).toDouble() == 1400.0 &&
              pasted.value(QStringLiteral("y")).toDouble() == 120.0 &&
              pasted.value(QStringLiteral("height")).toDouble() == 800.0,
          "the pasted slide carries the box");
    check(pasted.value(QStringLiteral("backgroundAssetId")).toString() == assetId,
          "the pasted slide carries the background");
    check(docs.currentStyleIsOverride() &&
              docs.currentStyle().value(QStringLiteral("id")).toString() == styleId,
          "the pasted slide wears the same style override");
    const openshow::scene::Scene copyScene = compiledFor(docs, pastedId);
    check(copyScene.background.hasImage && copyScene.items[0].color == 0xFFD23Fu &&
              copyScene.items[0].align == "left" &&
              copyScene.items[0].fontSize == 64.0 && copyScene.items[0].lineSpacing == 1.5,
          "the pasted slide compiles with the same look as its source");

    // One undo removes the whole pasted slide; the clipboard is not part
    // of history, so it stays ready.
    docs.undo();
    check(docs.slides().size() == 2, "one undo removes the pasted slide entirely");
    check(docs.canPaste(), "the clipboard is not part of undo/redo");
    docs.redo();
    check(docs.slides().size() == 3 &&
              compiledFor(docs, docs.slides().at(1).toMap().value(QStringLiteral("id")).toString())
                      .items[0].color == 0xFFD23Fu,
          "redo brings the styled paste back");
    docs.undo();

    // Cut captures and removes; one undo puts the slide back with its
    // content intact.
    docs.selectSlide(0);
    check(docs.cutCurrentSlide(), "cut captures the current slide and removes it");
    check(docs.canPaste() && docs.slides().size() == 1,
          "cut leaves one fewer slide and a full clipboard");
    check(docs.slides().at(0).toMap().value(QStringLiteral("preview")).toString() !=
              QStringLiteral("Amazing grace"),
          "the cut slide really left the show");
    docs.undo();
    check(docs.slides().size() == 2 &&
              docs.slides().at(0).toMap().value(QStringLiteral("preview")).toString() ==
                  QStringLiteral("Amazing grace"),
          "one undo restores the cut slide with its words");

    // Duplicate works from the slide itself and never touches the
    // clipboard: with "Amazing grace" loaded, duplicating the *second*
    // (empty) slide must still paste the loaded one afterwards.
    docs.selectSlide(1);
    check(docs.duplicateCurrentSlide(), "a slide with no words can still be duplicated");
    check(docs.slides().size() == 3 && docs.currentSlide() == 2,
          "the duplicate lands after its source and is selected");
    check(docs.slides().at(2).toMap().value(QStringLiteral("preview")).toString() ==
              QStringLiteral("(empty)"),
          "the duplicate is of the slide that was current, not of the clipboard");
    docs.selectSlide(0);
    check(docs.pasteSlide(), "the clipboard survived the duplicate");
    check(docs.slides().at(1).toMap().value(QStringLiteral("preview")).toString() ==
              QStringLiteral("Amazing grace"),
          "the paste after a duplicate still yields the copied slide");

    // The clipboard keeps its background reference, so deleting the asset
    // makes that reference stale — paste drops it instead of refusing.
    check(docs.removeAsset(assetId), "the background asset can be removed");
    check(docs.pasteSlide(), "paste still succeeds once the captured asset is gone");
    const QVariantMap stalePaste = docs.slides().at(2).toMap();
    check(stalePaste.value(QStringLiteral("preview")).toString() ==
                  QStringLiteral("Amazing grace") &&
              stalePaste.value(QStringLiteral("backgroundAssetId")).toString().isEmpty() &&
              !compiledFor(docs, stalePaste.value(QStringLiteral("id")).toString())
                       .background.hasImage,
          "the paste keeps the words and drops the deleted background");

    // Refusals name themselves.
    DocumentController bare(nullptr, root / "bare");
    check(!bare.copyCurrentSlide() && bare.status().contains(QStringLiteral("copy")),
          "with no slides copy has nothing to capture and says so");
    check(!bare.cutCurrentSlide() && bare.status().contains(QStringLiteral("copy")),
          "cut refuses where copy refuses");
    check(!bare.duplicateCurrentSlide() &&
              bare.status().contains(QStringLiteral("duplicate")),
          "with no slides duplicate has nothing to copy");
    check(!bare.pasteSlide() && bare.status().contains(QStringLiteral("clipboard")),
          "with an empty clipboard paste says so");
}

// --- task 6.14a (review R14): autosave health is visible, quitting flushes -------

// R14 measured three silent failures: the autosave tick's error was
// discarded, so a full disk emptied the crash net invisibly; the clear()
// after save/load was discarded, so stale scratch could resurrect itself
// on the next start with no word said; and the 600 ms debounced text was
// never flushed at exit, so the last keystrokes of a session died with
// the window. Every one of these fixtures asserts the VISIBLE outcome —
// statuses after processEvents (the refusal rides a zero timer so it can
// outlast the caller's own status line), and recovered text after a
// real aboutToQuit (QCoreApplication::exec exited by quit(), the same
// edge a window close or the SIGTERM poll drives in production).
void exerciseAutosaveSurfacing(const std::filesystem::path& root) {
    // (a) Quit inside the debounce keeps the final text...
    const std::filesystem::path quitBase = root / "quit";
    {
        DocumentController docs(nullptr, quitBase);
        docs.createSong(QStringLiteral("Final Word"));
        docs.addSlide();
        docs.setSlideText(QStringLiteral("committed before the storm"));
        docs.flushText();
        docs.setSlideText(QStringLiteral("LAST LINE typed, never flushed by hand"));
        check(docs.currentSlideText() == QStringLiteral("committed before the storm"),
              "the pending line is genuinely pending when the app starts to quit");
        QTimer::singleShot(0, [] { QCoreApplication::quit(); });
        QCoreApplication::exec();  // aboutToQuit -> flushText, as in production
        check(docs.currentSlideText() == QStringLiteral("LAST LINE typed, never flushed by hand"),
              "aboutToQuit committed the debounced text");
    }
    {
        DocumentController reopened(nullptr, quitBase);
        check(reopened.currentSlideText() ==
                  QStringLiteral("LAST LINE typed, never flushed by hand"),
              "the quit flush reached the scratch - the next session opens on the final words");
    }
    {
        // ...and the control shows the flush is what preserved it: without
        // a quit, an unflushed keystroke is simply gone next session.
        const std::filesystem::path controlBase = root / "quit-control";
        {
            DocumentController docs(nullptr, controlBase);
            docs.createSong(QStringLiteral("Doomed"));
            docs.addSlide();
            docs.setSlideText(QStringLiteral("kept by the last commit"));
            docs.flushText();
            docs.setSlideText(QStringLiteral("LOST with the window"));
        }
        DocumentController reopened(nullptr, controlBase);
        check(reopened.currentSlideText() == QStringLiteral("kept by the last commit"),
              "without the quit flush the pending keystroke is lost - the hook earns its keep");
    }

    // (b) A refusing disk is visible on the status line, per event kind.
    {
        const std::filesystem::path base = root / "broken-disk";
        DocumentController docs(nullptr, base);
        docs.createSong(QStringLiteral("Watched"));
        docs.addSlide();
        const std::filesystem::path scratch = base / "autosave";
        check(Autosave::exists(scratch), "the first edit is protected on disk");
        ::chmod(scratch.c_str(), 0555);  // the disk refuses writes from here on
        check(docs.addSlide() && docs.addSlide(),
              "editing keeps working while the scratch is refusing");
        QCoreApplication::processEvents();
        check(docs.status().contains(QStringLiteral("autosave refused after edit")) &&
                  docs.status().contains(QStringLiteral("NOT on disk")),
              "a refused tick after an edit is visible, and says work is not on disk");
        docs.undo();  // back to a state the scratch never held: the retry refuses too
        QCoreApplication::processEvents();
        check(docs.status().contains(QStringLiteral("autosave refused after undo")),
              "the refused undo tick reports undo as its own event");
        docs.redo();
        QCoreApplication::processEvents();
        check(docs.status().contains(QStringLiteral("autosave refused after redo")),
              "the refused redo tick reports redo as its own event");
        // The deferral is load-bearing: importMediaFile writes its own
        // success line synchronously after the refused tick, and the
        // refusal must still end up the last word - the operator must not
        // be left reading "added" with a scratch that is refusing.
        const std::filesystem::path hymnImage = base / "hymn.jpg";
        {
            std::ofstream out(hymnImage);
            out << "NOT-A-JPEG";
        }
        check(docs.importMediaFile(QString::fromStdWString(hymnImage.wstring())),
              "importing still edits the model while the scratch refuses");
        check(docs.status().contains(QStringLiteral("added")),
              "the import's success line lands first, synchronously");
        QCoreApplication::processEvents();
        check(docs.status().contains(QStringLiteral("autosave refused")),
              "the deferred refusal overwrites the success line - the last word is the hole in the net");
        ::chmod(scratch.c_str(), 0755);
        docs.setSlideText(QStringLiteral("healed"));
        docs.flushText();  // this tick writes again
        QCoreApplication::processEvents();
        check(docs.status().contains(QStringLiteral("autosave recovered")),
              "and the status says when the scratch is current again");
    }

    // (b2) The race guard: a refusal queued in the same turn, by the time
    // its zero-timer runs a scratch that has since caught up must not
    // stamp a healthy session with the stale warning.
    {
        const std::filesystem::path base = root / "race-guard";
        DocumentController docs(nullptr, base);
        docs.createSong(QStringLiteral("Raced"));
        docs.addSlide();
        const std::filesystem::path scratch = base / "autosave";
        ::chmod(scratch.c_str(), 0555);
        docs.addSlide();  // tick refused, warning queued
        ::chmod(scratch.c_str(), 0755);
        docs.setSlideText(QStringLiteral("caught up"));
        docs.flushText();  // scratch current again - before any event loop ran
        QCoreApplication::processEvents();
        check(!docs.status().contains(QStringLiteral("autosave refused")) &&
                  docs.status().contains(QStringLiteral("autosave recovered")),
              "the queued refusal is suppressed once the scratch has caught up");
    }

    // (c) save/load that cannot delete the stale scratch say so — the
    // survivor is a resurrection risk, not a detail.
    {
        const std::filesystem::path base = root / "stale-clear";
        DocumentController docs(nullptr, base);
        docs.createSong(QStringLiteral("Committed"));
        docs.addSlide();
        check(docs.save() && !Autosave::exists(base / "autosave"),
              "a healthy save saves and clears the scratch");
        docs.addSlide();  // dirty again; the scratch is re-created
        const std::filesystem::path scratch = base / "autosave";
        check(Autosave::exists(scratch), "the new edit is protected again");
        ::chmod(scratch.c_str(), 0555);  // nothing inside it can be deleted
        check(docs.save() && !docs.dirty(),
              "the save itself still succeeds - only the delete failed");
        check(docs.status().contains(QStringLiteral("saved")) &&
                  docs.status().contains(QStringLiteral("stale autosave")) &&
                  Autosave::exists(scratch),
              "save NAMES the survivor it could not remove, instead of claiming a clean state");
        check(docs.load(), "load succeeds");
        check(docs.status().contains(QStringLiteral("loaded")) &&
                  docs.status().contains(QStringLiteral("stale autosave")),
              "load names it too - discarded work must not resurface silently");
        ::chmod(scratch.c_str(), 0755);
    }
}

// --- task 6.14b (review R14): persistence on the coalescing executor ----------

// The storage-delay seam (OPENSHOW_STORAGE_DELAY_MS, storage.cpp) holds
// each save at its start, and the fixtures re-read the freedom 6.13a
// proved for faults: the variable is read per call, so a fixture can
// arm the slowness for one write, disarm it, and let a DECISION run
// fast while the old write is still sleeping. That is how these fixtures
// schedule the exact races R14 named.
void exerciseBackgroundPersistence(const std::filesystem::path& root) {
    // (a) Editing is a snapshot copy: the UI thread never waits for the
    // slow disk, requests coalesce, and the scratch still ends on the
    // final state.
    const std::filesystem::path coalesceBase = root / "coalescing";
    {
        DocumentController docs(nullptr, coalesceBase);
        docs.startBackgroundPersistence();
        ::setenv("OPENSHOW_STORAGE_DELAY_MS", "250", 1);
        docs.createSong(QStringLiteral("Storm"));
        docs.addSlide();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
        for (int i = 0; i < 30; ++i) {
            docs.setSlideText(QStringLiteral("storm line %1").arg(i));
            docs.flushText();  // each commit queues exactly one autosave request
        }
        check(std::chrono::steady_clock::now() < deadline,
              "thirty commits against a 250 ms disk never blocked the UI thread");
        docs.awaitPersistence();
        check(docs.autosaveWrites() <= 5 && docs.autosaveWrites() >= 1,
              "one write in flight plus one queued is the whole executor");
        check(docs.autosaveCoalesced() >= 20,
              "the storm of requests merged - the crash net needs only the newest");
        ::unsetenv("OPENSHOW_STORAGE_DELAY_MS");
    }
    {
        DocumentController reopened(nullptr, coalesceBase);
        check(reopened.currentSlideText() == QStringLiteral("storm line 29"),
              "and the settled scratch holds the very last edit");
    }

    // (b) Save while an autosave write is in flight: the decision waits,
    // and the older write cannot resurrect the scratch.
    {
        const std::filesystem::path base = root / "resurrection";
        DocumentController docs(nullptr, base);
        docs.startBackgroundPersistence();
        ::setenv("OPENSHOW_STORAGE_DELAY_MS", "600", 1);
        docs.createSong(QStringLiteral("Decide"));  // write IN FLIGHT, sleeping 600
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        docs.addSlide();  // queued behind the sleeper - this save supersedes it too
        ::unsetenv("OPENSHOW_STORAGE_DELAY_MS");  // the save itself runs fast
        const auto decisionStart = std::chrono::steady_clock::now();
        check(docs.save(), "the save succeeds while the old write is mid-flight");
        const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - decisionStart)
                                  .count();
        check(waitedMs >= 400,
              "slow storage, witnessed: the save really waited out the in-flight 600 ms write");
        check(!Autosave::exists(base / "autosave"),
              "save quiesced first: the clear is the last word, not the in-flight write "
              "nor the queued request the save superseded");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));  // past the old write's window
        docs.awaitPersistence();
        check(!Autosave::exists(base / "autosave"),
              "the sleeping write never landed - a stale autosave cannot resurrect");
    }

    // (c) The same protection on the discard path (load).
    {
        const std::filesystem::path base = root / "discard";
        {
            DocumentController docs(nullptr, base);
            docs.createSong(QStringLiteral("Kept"));
            docs.addSlide();
            check(docs.save(), "a service exists to load back");
        }
        DocumentController docs(nullptr, base);
        docs.startBackgroundPersistence();
        ::setenv("OPENSHOW_STORAGE_DELAY_MS", "600", 1);
        docs.addSlide();  // discarded-soon work, write IN FLIGHT
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        ::unsetenv("OPENSHOW_STORAGE_DELAY_MS");
        check(docs.load(), "load succeeds mid-flight");
        check(!Autosave::exists(base / "autosave"), "the discard cleared the scratch");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        docs.awaitPersistence();
        check(!Autosave::exists(base / "autosave"),
              "and the in-flight write of the discarded edit never came back");
        check(docs.slides().size() == 1, "the session shows the loaded file, not the discarded edit");
    }

    // (d) Background refusals acknowledge ON the UI thread, healthy
    // recoveries too.
    {
        const std::filesystem::path base = root / "acked";
        DocumentController docs(nullptr, base);
        docs.startBackgroundPersistence();
        docs.createSong(QStringLiteral("Acked"));
        docs.addSlide();
        docs.awaitPersistence();
        ::chmod((base / "autosave").c_str(), 0555);
        docs.addSlide();
        docs.awaitPersistence();  // the write ran (and failed); the ack landed
        check(docs.status().contains(QStringLiteral("autosave refused after edit")) &&
                  docs.status().contains(QStringLiteral("NOT on disk")),
              "a refusal on the background thread surfaces on the status line all the same");
        ::chmod((base / "autosave").c_str(), 0755);
        docs.setSlideText(QStringLiteral("healthy again"));
        docs.flushText();
        docs.awaitPersistence();
        check(docs.status().contains(QStringLiteral("autosave recovered")),
              "and the background success is acknowledged too");
    }

    // (e) Quit drains: after prepareForQuit returns, the scratch is
    // durable — the process may exit at that instant.
    {
        const std::filesystem::path base = root / "quit-drain";
        DocumentController docs(nullptr, base);
        docs.startBackgroundPersistence();
        docs.createSong(QStringLiteral("Bye"));
        docs.addSlide();
        docs.awaitPersistence();  // quiet before the storm of leaving
        ::setenv("OPENSHOW_STORAGE_DELAY_MS", "600", 1);
        docs.setSlideText(QStringLiteral("last words before quit"));
        docs.flushText();  // the final request is queued/sleeping
        docs.prepareForQuit();
        ::unsetenv("OPENSHOW_STORAGE_DELAY_MS");
        {
            DocumentController reopened(nullptr, base);
            check(reopened.currentSlideText() == QStringLiteral("last words before quit"),
                  "prepareForQuit waited: the durable scratch, not a queued intention, "
                  "is what the next session finds");
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("openshow-document-tests"));

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
                                           ("openshow-docctrl-" + std::to_string(getpid()));
    std::filesystem::remove_all(tempRoot);
    exerciseEditingAndPresentation(tempRoot / "edit");
    exerciseCommitSilence(tempRoot / "silence");
    exerciseUndoRedo(tempRoot / "undo");
    exerciseGeometryEdits(tempRoot / "geometry");
    exercisePersistence(tempRoot / "persist");
    exerciseRecoveredAssetOwnership(tempRoot / "adopt");
    exercisePlaylistAndLibrary(tempRoot / "playlist");
    exerciseStylePanel(tempRoot / "styles");
    exerciseMultiBoxSession(tempRoot / "multibox");
    exerciseClipboard(tempRoot / "clipboard");
    exerciseAutosaveSurfacing(tempRoot / "surfacing");
    exerciseBackgroundPersistence(tempRoot / "background");
    std::filesystem::remove_all(tempRoot);

    if (gFailed) {
        return 1;
    }
    std::printf("document_controller_tests: all fixtures passed\n");
    return 0;
}
