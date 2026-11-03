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

#include <algorithm>
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
            // Task UI.4a rides along for free here: the bin labels and filters
            // a row by its name's extension, so an adoption that dropped the
            // extension would silently turn every picture into "other".
            check(entry.toMap().value(QStringLiteral("kind")).toString() ==
                      QStringLiteral("picture"),
                  "adopt: the package copy keeps its extension, so its kind survives");
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

    // --- task UI.3a: the read-only cue projection -------------------------
    {
        check(docs.createPlaylist("Cues") && docs.runningOrderCues().isEmpty(),
              "the cue projection follows the playlist under preparation");
        check(docs.addShowToPlaylist(first) && docs.addShowToPlaylist(second) &&
                  docs.addShowToPlaylist(first),
              "the running order takes the first show twice");
        const QVariantList cues = docs.runningOrderCues();
        const std::vector<std::string> cueEntryIds = mapEntryIds(cues);
        check(cues.size() == 3 && cueEntryIds[0] != cueEntryIds[2] &&
                  cues[0].toMap().value(QStringLiteral("showId")).toString() == first &&
                  cues[2].toMap().value(QStringLiteral("showId")).toString() == first &&
                  cues[1].toMap().value(QStringLiteral("showId")).toString() == second,
              "every entry is its own cue group, duplicates included");
        check(cues[0].toMap().value(QStringLiteral("entryIndex")).toInt() == 0 &&
                  cues[2].toMap().value(QStringLiteral("entryIndex")).toInt() == 2 &&
                  cues[0].toMap().value(QStringLiteral("showName")).toString() ==
                      QStringLiteral("Amazing Grace") &&
                  cues[0].toMap().contains(QStringLiteral("section")),
              "cue groups carry their position, show name and section");
        const auto slideIds = [](const QVariantList& rows) {
            QStringList ids;
            for (const QVariant& row : rows) {
                ids.append(row.toMap().value(QStringLiteral("id")).toString());
            }
            return ids;
        };
        const QStringList firstSlides = slideIds(
            cues[0].toMap().value(QStringLiteral("slides")).toList());
        check(!firstSlides.isEmpty() &&
                  firstSlides ==
                      slideIds(cues[2].toMap().value(QStringLiteral("slides")).toList()),
              "the two cues of one show list exactly the same slides");

        // Editing B while A runs twice: compiling every cue of every group
        // reads the projection alone and never moves the editing cursor.
        docs.selectSong(second);
        docs.selectSlide(0);
        const QString cursorSong = docs.currentSongId();
        const int cursorSlide = docs.currentSlide();
        bool compiled = !firstSlides.isEmpty();
        for (const QVariant& group : docs.runningOrderCues()) {
            for (const QVariant& slide : group.toMap().value(QStringLiteral("slides")).toList()) {
                compiled = compiled && !docs.sceneJsonForSlide(
                                           slide.toMap().value(QStringLiteral("id")).toString())
                                           .isEmpty();
            }
        }
        check(compiled, "every cue of the running order compiles from the projection alone");
        check(docs.currentSongId() == cursorSong && docs.currentSlide() == cursorSlide,
              "compiling an arbitrary show never moves the editing cursor");

        // Surviving cues keep their identities across a move and a removal.
        const QString lastEntry = cues[2].toMap().value(QStringLiteral("entryId")).toString();
        const QString middleEntry = cues[1].toMap().value(QStringLiteral("entryId")).toString();
        check(docs.movePlaylistEntry(lastEntry, 0) &&
                  QString::fromStdString(mapEntryIds(docs.runningOrderCues())[0]) == lastEntry,
              "a moved cue keeps its entry id");
        check(docs.removePlaylistEntry(middleEntry) &&
                  docs.runningOrderCues().size() == 2 &&
                  mapEntryIds(docs.runningOrderCues()) ==
                      std::vector<std::string>{cueEntryIds[2], cueEntryIds[0]},
              "removing one cue leaves the other cues' identities alone");
        docs.selectPlaylist(service);
    }

    // The domain rule: removing a show drops the entries naming it.
    docs.selectSong(second);
    check(docs.removeCurrentSong(), "the show is removed");
    check(docs.playlistEntries().size() == 2,
          "playlist rows naming a deleted show go with it");

    // File picker URLs must preserve spaces and fragment characters as
    // local file names, rather than passing an encoded URL to storage.
    const QString mediaPath = QString::fromStdWString((root / "loop #1.mp4").wstring());
    check(docs.importMediaUrl(QUrl::fromLocalFile(mediaPath)) && docs.assets().size() == 1 &&
              docs.assets().front().toMap().value(QStringLiteral("name")) == QStringLiteral("loop #1.mp4") &&
              docs.assets().front().toMap().value(QStringLiteral("sourcePath")) == mediaPath,
          "file picker import registers the decoded local path and file name");
    check(!docs.importMediaUrl(QUrl(QStringLiteral("https://example.invalid/media.mp4")))
              && docs.assets().size() == 1,
          "a non-local picker URL cannot create a library entry");
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
        // Three by now: the service, Evening, and the UI.3a cue fixture.
        check(reloaded.playlists().size() == 3 && reloaded.playlistEntries().size() == 2,
              "playlists and their rows persist through save/load");
    }
}

// --- the theme panel (task 4.4) ---------------------------------------------

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

// Task 6.17a2 (review R17): the operator's half — selecting, creating,
// editing, moving, resizing and reordering boxes through the class the panel
// binds to. The model has had these operations since 6.17a1; what this
// fixture pins is the SESSION's rule on top of them: which box the inspector,
// the fields and the canvas are looking at, and that every stack command is
// exactly one undo step.
void exerciseBoxAuthoring(const std::filesystem::path& root) {
    const std::filesystem::path service = root / "service";
    writeMultiBoxService(service);
    DocumentController docs(nullptr, root);
    docs.selectSong(QStringLiteral("show1"));
    docs.selectSlide(0);
    const QString slide1 = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();

    // The canvas's data: one entry per box, in draw order, with geometry.
    const QVariantList outlines = docs.currentSlideBoxes();
    check(outlines.size() == 3 &&
              outlines.at(2).toMap().value(QStringLiteral("x")).toDouble() == 1400.0,
          "the canvas is handed every box of the stacked slide");
    check(docs.currentBox() == 0, "with nothing selected the bottom box is the selection");

    // Topmost hit: box 1 (100,40,800,120) overlaps box 0 (160,100,1600,880)
    // in the band y=100..160, and the point in the overlap must answer with
    // the box PAINTED LAST — the one the operator can see and therefore
    // meant to click. A front-to-back scan answers with box 0 there and
    // silently steals every click in that band from the title strip.
    check(docs.boxAt(400.0, 120.0) == 1, "a point in two boxes hits the one on top");
    check(docs.boxAt(400.0, 60.0) == 1, "a point in only the upper box hits it");
    check(docs.boxAt(1000.0, 500.0) == 0, "a point clear of the strip still hits the base box");
    check(docs.boxAt(1500.0, 1000.0) == 2, "the credit line is hittable where it is drawn");
    check(docs.boxAt(50.0, 900.0) == -1, "empty canvas hits nothing");

    // Selection by index, clamped like selectSlide; the inspector's words
    // move WITH the selection (and it dirties nothing — see the dirty checks).
    int boxSignals = 0;
    int textSignals = 0;
    int docSignals = 0;
    QObject::connect(&docs, &DocumentController::boxSelectionChanged,
                     [&] { ++boxSignals; });
    QObject::connect(&docs, &DocumentController::slideTextChanged,
                     [&] { ++textSignals; });
    QObject::connect(&docs, &DocumentController::documentsChanged,
                     [&] { ++docSignals; });
    check(docs.selectTextBox(1) && docs.currentBox() == 1, "the operator selects the second box");
    check(boxSignals == 1 && textSignals == 1 && docSignals == 0,
          "selection announces itself as chrome: box and inspector repaint, the workspace does not");
    check(docs.currentSlideText() == QStringLiteral("TITLE"),
          "the inspector now shows the SELECTED box's words");
    check(docs.selectTextBox(99) && docs.currentBox() == 2,
          "a selection past the top clamps to the top box");
    check(docs.selectTextBox(-3) && docs.currentBox() == 0, "and a negative one clamps to the bottom");

    // Navigation is a new context: the selection belongs to a slide. The id
    // must be picked just before crossing so a session that CARRIED it would
    // find that very box again on the way back - falling back to 0 here
    // because it was cleared, not because the lookup missed.
    docs.selectTextBox(1);
    docs.selectSlide(1);
    check(docs.currentBox() == -1 && docs.currentSlideBoxes().isEmpty(),
          "the box-less slide has no selection to show");
    check(!docs.selectTextBox(0), "selecting on a slide with no boxes is refused");
    check(docs.status().contains(QStringLiteral("no text box to select")),
          "and the refusal says why");
    // Back across while the (mutably carried) id still names THIS slide's
    // title strip: a controller that kept the id across navigation would
    // find that very box again, and only the clear can land on 0 here.
    docs.selectSlide(0);
    check(docs.currentBox() == 0,
          "navigation clears the selection rather than carrying an id this slide can still resolve");
    docs.selectSlide(1);
    check(docs.addTextBox() && docs.currentBox() == 0,
          "adding to the box-less slide creates one and selects it");
    docs.selectSlide(0);  // and back: the stack sections below work the slide

    // Typing follows the SELECTION, and moving the selection flushes first:
    // a burst typed into box 2 must not land in box 0 because the operator
    // tabbed away mid-sentence.
    docs.selectTextBox(2);
    docs.setSlideText(QStringLiteral("credit changed"));
    docs.selectTextBox(0);  // the flush-before-move is the whole claim
    openshow::scene::Scene afterSwitch = compiledFor(docs, slide1);
    check(afterSwitch.items[2].lines == std::vector<std::string>{"credit changed"} &&
              afterSwitch.items[0].lines == std::vector<std::string>{"Verse line"},
          "keystrokes typed into the third box land there, not in the box tabbed to");
    docs.undo();
    check(compiledFor(docs, slide1).items[2].lines == std::vector<std::string>{"photo: someone"},
          "that burst is exactly one undo step");

    // One undo step each, for every stack command — with the selection's
    // id-rule visible in the middle: undo of an ADD drops the box, the
    // selection falls back; REDO brings the box back and the selection
    // resolves onto it again, because the id names it.
    check(docs.addTextBox(), "a new box is added on top");
    check(docs.currentBox() == 3, "and the new box is the selection - it was added to be edited");
    check(compiledFor(docs, slide1).items.size() == 4 &&
              compiledFor(docs, slide1).items[3].x == 160.0,
          "the off-air payload gains the new box at the default place");
    docs.undo();
    check(compiledFor(docs, slide1).items.size() == 3 && docs.currentBox() == 0,
          "one undo drops the added box and the selection falls back to the bottom");
    docs.redo();
    check(compiledFor(docs, slide1).items.size() == 4 && docs.currentBox() == 3,
          "redo restores the box and the selection resolves onto the same id again");
    docs.undo();

    // Delete by selection: the middle box goes, the outer two keep order;
    // undo restores the box AND the selection lands back on it.
    docs.selectTextBox(1);
    check(docs.removeCurrentTextBox(), "the selected middle box is deleted");
    openshow::scene::Scene drilled = compiledFor(docs, slide1);
    check(drilled.items.size() == 2 &&
              drilled.items[0].lines == std::vector<std::string>{"Verse line"} &&
              drilled.items[1].lines == std::vector<std::string>{"photo: someone"},
          "deleting the middle box leaves the outer two in draw order");
    check(docs.currentBox() == 0, "with its box gone the selection falls back");
    docs.undo();
    check(compiledFor(docs, slide1).items.size() == 3 && docs.currentBox() == 1 &&
              docs.currentSlideText() == QStringLiteral("TITLE"),
          "undo brings the box back with its identity - and the selection with it");

    // Reorder: the selection follows identity, so the handles stay on the
    // box that moved, and the wire order - the paint order - changes.
    docs.selectTextBox(0);
    check(docs.moveCurrentTextBox(2), "the bottom box is moved to the top");
    openshow::scene::Scene reordered = compiledFor(docs, slide1);
    check(reordered.items[2].lines == std::vector<std::string>{"Verse line"} &&
              reordered.items[0].lines == std::vector<std::string>{"TITLE"},
          "the compiled scene paints in the new stack order");
    check(docs.currentBox() == 2 && docs.currentSlideText() == QStringLiteral("Verse line"),
          "the selection followed its box to its new position");
    check(!docs.moveCurrentTextBox(-1), "a negative destination is refused, not clamped");
    check(docs.status().contains(QStringLiteral("negative")),
          "and the status names the controller's own guard, not a cast past the end");
    docs.undo();
    check(compiledFor(docs, slide1).items[0].lines == std::vector<std::string>{"Verse line"},
          "one undo puts the stack back");

    // Geometry now addresses the selected box, and a refusal is invisible to
    // history: undo after a refused resize goes back to the PREVIOUS edit.
    docs.selectTextBox(1);
    check(docs.setTextBoxGeometry(10.0, 20.0, 30.0, 40.0), "the selected box is resized");
    openshow::scene::Scene sized = compiledFor(docs, slide1);
    check(sized.items[1].x == 10.0 && sized.items[0].x == 160.0,
          "the edit moved the selected box and no other");
    check(!docs.setTextBoxGeometry(10.0, 20.0, 0.0, 40.0),
          "a box with no width is refused");
    check(compiledFor(docs, slide1).items[1].width == 30.0,
          "the refusal changed nothing");
    docs.undo();
    check(compiledFor(docs, slide1).items[1].x != 10.0,
          "undo steps past the refusal to the last real edit - the refusal added no step");

    // The ceiling: a slide at eight boxes refuses the ninth, by name. The
    // box-less slide picked up one box earlier, so drain it first - and
    // note each filled box is TWO history entries (the add, then the typed
    // name), which is what makes the refusal's invisibility checkable:
    // after the ninth add is refused, the next undo reverts the LAST TYPED
    // NAME. A refusal that had pushed an entry would spend this undo on
    // nothing instead.
    docs.selectSlide(1);
    for (int i = 0; i < 10 && docs.currentBox() >= 0; ++i) {
        docs.removeCurrentTextBox();
    }
    check(docs.currentBox() == -1, "the slide is drained before the ceiling is tested");
    const QString blank = docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();
    const char* names[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
    for (int i = 0; i < 8; ++i) {
        docs.addTextBox();
        docs.setSlideText(QString::fromLatin1(names[i]));
        docs.flushText();
    }
    check(compiledFor(docs, blank).items.size() == 8, "eight boxes fill a slide");
    check(!docs.addTextBox(), "the ninth box is refused");
    check(docs.status().contains(QStringLiteral("8")), "and the refusal names the limit");
    check(compiledFor(docs, blank).items.size() == 8, "the stack is unchanged by the refusal");
    docs.undo();
    check(compiledFor(docs, blank).items.size() == 8 &&
              compiledFor(docs, blank).items[7].lines != std::vector<std::string>{"H"},
          "the next undo reverts the last typed name - the refusal left no step behind");

    // And a slide drained to nothing is a backdrop, not an error: deleting
    // the last box is legal, and the box-scoped edits say so instead of
    // inventing a box.
    for (int i = 0; i < 10 && docs.currentBox() >= 0; ++i) {
        docs.removeCurrentTextBox();
    }
    check(docs.currentBox() == -1 && compiledFor(docs, blank).items.empty(),
          "deleting the last box leaves a backdrop with no items");
    check(!docs.setTextBoxGeometry(10.0, 20.0, 30.0, 40.0),
          "resizing a box-less slide is refused");
    check(docs.status().contains(QStringLiteral("no text box to resize")), "naming what is missing");
    check(!docs.removeCurrentTextBox(), "and there is nothing left to delete");

    // Selection alone never dirties the session: a click is chrome. This has
    // to be measured on a session that starts CLEAN - a second controller on
    // `root` would recover the autosave scratch and be dirty BEFORE the
    // click, and "still dirty after" would then be true of any controller,
    // chrome or not. So: the same service bytes, copied somewhere no autosave
    // has ever been written.
    const auto pristine = root / "pristine";
    std::filesystem::create_directories(pristine / "service");
    std::filesystem::copy_file(root / "service" / "service.json",
                               pristine / "service" / "service.json",
                               std::filesystem::copy_options::overwrite_existing);
    DocumentController quiet(nullptr, pristine);
    quiet.selectSong(QStringLiteral("show1"));
    quiet.selectSlide(0);
    check(!quiet.dirty(), "a session that loaded a clean package is not dirty");
    check(quiet.selectTextBox(2) && !quiet.dirty(),
          "and selecting a box is not an edit - it did not make the session dirty");
    check(quiet.currentSlideText() == QStringLiteral("photo: someone"),
          "while still moving the inspector onto the box it selected");
}

void exerciseMultiBoxSession(const std::filesystem::path& root) {
    const std::filesystem::path service = root / "service";
    writeMultiBoxService(service);
    DocumentController docs(nullptr, root);
    check(docs.songs().size() == 1, "the multi-box service loads at startup");
    docs.selectSong(QStringLiteral("show1"));
    check(docs.slides().size() == 2, "both of its slides are listed");

    // The row the list paints: the FIRST box. The canvas now outlines every
    // box and gestures address the selected one; a row still NAMES a slide,
    // and its bottom box is what represents it to someone scanning the list.
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

    // With nothing selected the inspector shows box 0 — the selection's
    // fallback, which is the box every pre-6.17a slide has alone. The
    // selection itself (and thus the inspector moving to box 1 or 2) is
    // exercised by exerciseBoxAuthoring().
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
            for (const openshow::domain::SlideItem& box : slide.items) {
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

// Task TH.3: a service that ALREADY wears a current theme, with a slide
// that carries its own customization, loads into a session whose panel and
// compiled scenes follow the whole chain — not one layer's assignment.
void writeWornService(const std::filesystem::path& service) {
    std::filesystem::create_directories(service);
    std::ofstream out(service / "service.json");
    out << R"svc({"format":6,"nextOrdinal":90,"currentThemeId":"theme2","shows":[{"id":"show1","name":"Worn","notes":"","defaultThemeId":"theme1","slides":[{"id":"slide1","group":"","items":[{"id":"text1","kind":"text","lines":["Verse one"],"x":160,"y":100,"width":1600,"height":880}],"notes":"","themeId":"theme3","backgroundAssetId":"","fit":"cover","customizations":{"textColor":"#00ffff"}},{"id":"slide2","group":"","items":[{"id":"text2","kind":"text","lines":["Verse two"],"x":160,"y":100,"width":1600,"height":880}],"notes":"","themeId":"","backgroundAssetId":"","fit":"cover","customizations":{}}]}],"themes":[{"id":"theme1","name":"House","textColor":"#111111","backgroundColor":"#222222","backgroundGradient":"","fontFamily":"Show Serif","fontSize":40,"align":"left","lineSpacing":1.1},{"id":"theme2","name":"Current","textColor":"#ffe000","backgroundColor":"#200000","backgroundGradient":"#602020","fontFamily":"Current Sans","fontSize":72,"align":"center","lineSpacing":1.5},{"id":"theme3","name":"Own","textColor":"#ff0000","backgroundColor":"#00ff00","backgroundGradient":"","fontFamily":"Impact","fontSize":96,"align":"right","lineSpacing":1.25}],"assets":[],"playlists":[]})svc";
    out.close();
}

void exerciseWornChain(const std::filesystem::path& root) {
    const std::filesystem::path service = root / "service";
    writeWornService(service);
    DocumentController docs(nullptr, root);
    check(docs.songs().size() == 1, "the worn service loads at startup");
    docs.selectSong(QStringLiteral("show1"));
    docs.selectSlide(0);
    const QString slide1 = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    const QString slide2 = docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();

    // The panel shows the chain's top live layer: the current theme, and
    // the slide's own (silenced) themeId is no override.
    const QVariantMap worn = docs.currentTheme();
    check(worn.value(QStringLiteral("id")).toString() == QStringLiteral("theme2") &&
              worn.value(QStringLiteral("name")).toString() == QStringLiteral("Current"),
          "the panel shows the current theme, not the silenced slide theme");
    check(!docs.currentThemeIsOverride(), "a silenced slide theme is not an override");

    // The compile follows the chain: customization over current theme.
    const openshow::scene::Scene a = compiledFor(docs, slide1);
    check(a.items.size() == 1 && a.items[0].color == 0x00FFFFu &&
              a.items[0].fontFamily == QStringLiteral("Current Sans") &&
              a.items[0].fontSize == 72.0 && a.items[0].align == QStringLiteral("center"),
          "the customized colour beats the current theme and the rest falls through");
    check(a.background.color == 0x200000u && a.background.hasGradient &&
              a.background.gradient == 0x602020u,
          "and the current theme lays its ground under the customized word");
    const openshow::scene::Scene b = compiledFor(docs, slide2);
    check(b.items.size() == 1 && b.items[0].color == 0xFFE000u,
          "the uncustomized slide wears the current theme whole");

    docs.selectSlide(1);
    check(!docs.currentThemeIsOverride() &&
              docs.currentTheme().value(QStringLiteral("id")).toString() ==
                  QStringLiteral("theme2"),
          "the second slide reads the same current theme with no override");

    // "New theme from current" snapshots the RESOLVED look (task TH.3), not
    // one layer's fields: the customized colour and the worn ground travel
    // into the created theme, and the slide's picture does not move.
    docs.selectSlide(0);
    check(docs.createThemeFromCurrent(QStringLiteral("FromChain")),
          "a theme created from the customized slide is accepted");
    QVariantMap made;
    for (const QVariant& row : docs.themes()) {
        if (row.toMap().value(QStringLiteral("name")).toString() ==
            QStringLiteral("FromChain")) {
            made = row.toMap();
        }
    }
    check(made.value(QStringLiteral("textColor")).toString() == QStringLiteral("#00ffff") &&
              made.value(QStringLiteral("backgroundColor")).toString() ==
                  QStringLiteral("#200000") &&
              made.value(QStringLiteral("fontFamily")).toString() ==
                  QStringLiteral("Current Sans") &&
              made.value(QStringLiteral("fontSize")).toDouble() == 72.0 &&
              made.value(QStringLiteral("lineSpacing")).toDouble() == 1.5,
          "the new theme snapshots the whole chain, customization included");
    check(compiledFor(docs, slide1).items[0].color == 0x00FFFFu,
          "the created theme wears what the slide already drew — the compile does not move");
}

void exerciseThemePanel(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Consistency"));
    docs.addSlide();
    docs.addSlide();
    docs.selectSlide(0);  // addSlide leaves slide 2 current; start from slide 1
    const QString slideA = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    const QString slideB = docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();

    check(docs.currentTheme().isEmpty() && !docs.currentThemeIsOverride(),
          "a song with no styles has no current style to show");

    check(docs.createThemeFromCurrent(QStringLiteral("Chorus")),
          "a style is created from the current slide's built-in look");
    const QVariantMap chorus = docs.currentTheme();
    const QString chorusId = chorus.value(QStringLiteral("id")).toString();
    check(chorus.value(QStringLiteral("name")).toString() == QStringLiteral("Chorus") &&
              chorus.value(QStringLiteral("textColor")).toString() == QStringLiteral("#ffffff") &&
              chorus.value(QStringLiteral("backgroundColor")).toString() ==
                  QStringLiteral("#000000") &&
              chorus.value(QStringLiteral("fontSize")).toDouble() == 0.0 &&
              chorus.value(QStringLiteral("align")).toString() == QStringLiteral("center") &&
              chorus.value(QStringLiteral("lineSpacing")).toDouble() == 1.25 &&
              docs.currentThemeIsOverride(),
          "the new style wears the built-in look and overrides only this slide");
    // The seeded Default theme (amendment 2026-11-02) is always in the
    // library: Chorus is the operator's one, Default the floor beneath it.
    check(docs.themes().size() == 2, "the styles view lists the new style (and the floor)");

    check(docs.setShowDefaultTheme(chorusId), "the style can become the show's default");
    docs.selectSlide(1);
    check(!docs.currentThemeIsOverride() &&
              docs.currentTheme().value(QStringLiteral("id")).toString() == chorusId,
          "the second slide follows the show default with no override of its own");

    check(docs.editTheme(chorusId, QStringLiteral("Chorus"), QStringLiteral("#ff0000"),
                         QStringLiteral("#101010"), QString(), QStringLiteral("DejaVu Serif"), 72.0,
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

    // Per-slide override: fork the followed theme for one slide.
    check(docs.createThemeFromCurrent(QStringLiteral("Whisper")),
          "a slide can fork the style it follows into its own");
    const QString whisperId = docs.currentTheme().value(QStringLiteral("id")).toString();
    check(!whisperId.isEmpty() && whisperId != chorusId && docs.currentThemeIsOverride(),
          "the fork overrides only this slide");
    check(docs.editTheme(whisperId, QStringLiteral("Whisper"), QStringLiteral("#00ff00"),
                         QStringLiteral("#000000"), QString(), QString(), 0.0, QStringLiteral("right"),
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
    // Default is back too (the seeded floor), but only one theme is the
    // slide's own again.
    check(docs.themes().size() == 2 && !docs.currentThemeIsOverride() &&
              compiledFor(docs, slideB).items[0].color == 0xFF0000u,
          "undoing the fork removes the extra style and hands the slide back");

    // The background gradient (task 6.17c3): one edit adds the second
    // colour, the panel's map and the compiled scene both carry it, undo
    // flattens it, a non-colour is refused naming its role, and a save
    // round trip keeps it.
    check(docs.editTheme(chorusId, QStringLiteral("Chorus"), QStringLiteral("#ff0000"),
                         QStringLiteral("#101010"), QStringLiteral("#20c050"), QString(),
                         72.0, QStringLiteral("left"), 1.5),
          "a style gains a second background colour");
    check(docs.currentTheme().value(QStringLiteral("backgroundGradient")).toString() ==
              QStringLiteral("#20c050"),
          "the currentTheme map carries it for the panel");
    check(compiledFor(docs, slideA).background.hasGradient &&
              compiledFor(docs, slideA).background.gradient == 0x20C050u &&
              compiledFor(docs, slideA).background.color == 0x101010u,
          "the compiled scene carries the pair: top colour, bottom colour");
    docs.undo();  // the gradient edit back — the theme is flat again
    check(!compiledFor(docs, slideA).background.hasGradient &&
              docs.currentTheme()
                  .value(QStringLiteral("backgroundGradient"))
                  .toString()
                  .isEmpty(),
          "undo flattens the background again, map included");
    check(!docs.editTheme(chorusId, QStringLiteral("Chorus"), QStringLiteral("#ff0000"),
                          QStringLiteral("#101010"), QStringLiteral("dusk"), QString(), 72.0,
                          QStringLiteral("left"), 1.5) &&
              docs.status().contains(QStringLiteral("background gradient")),
          "a non-colour gradient is refused on the status line, naming its role");
    check(!compiledFor(docs, slideA).background.hasGradient,
          "the refused edit left the flat background alone");
    check(docs.editTheme(chorusId, QStringLiteral("Chorus"), QStringLiteral("#ff0000"),
                         QStringLiteral("#101010"), QStringLiteral("#20c050"), QString(),
                         72.0, QStringLiteral("left"), 1.5),
          "the gradient set again, so the save below carries it");
    if (check(docs.save(), "saving a graded style succeeds")) {
        DocumentController reloaded(nullptr, root);
        check(reloaded.load() &&
                  reloaded.currentTheme().value(QStringLiteral("backgroundGradient")).toString() ==
                      QStringLiteral("#20c050"),
              "a reload returns the gradient through the controller");
    }
    // And a fork of a graded slide starts WITH the gradient — "wear what
    // the slide draws now" includes the second colour.
    check(docs.createThemeFromCurrent(QStringLiteral("Shaded")),
          "a slide wearing the graded style forks it");
    check(docs.currentTheme().value(QStringLiteral("backgroundGradient")).toString() ==
              QStringLiteral("#20c050"),
          "createThemeFromCurrent snapshots the gradient too");
    docs.undo();  // the fork (and its override) back

    // Explicitly point a slide at a theme, then hand it back (empty id).
    check(docs.applyThemeToCurrentSlide(chorusId) && docs.currentThemeIsOverride(),
          "a slide can be pinned to a style by id");
    check(docs.applyThemeToCurrentSlide(QString()) && !docs.currentThemeIsOverride() &&
              docs.currentTheme().value(QStringLiteral("id")).toString() == chorusId,
          "the empty id follows the show default again");

    // Refusals name themselves and leave everything alone.
    check(!docs.editTheme(QStringLiteral("style999"), QStringLiteral("Ghost"),
                          QStringLiteral("#ffffff"), QStringLiteral("#000000"), QString(), QString(), 0.0,
                          QStringLiteral("center"), 1.25) &&
              docs.status().contains(QStringLiteral("edit theme")),
          "editing an unknown theme is refused on the status line");
    check(!docs.editTheme(chorusId, QStringLiteral("   "), QStringLiteral("#ffffff"),
                          QStringLiteral("#000000"), QString(), QString(), 0.0,
                          QStringLiteral("center"), 1.25),
          "a blank name is refused before anything is recorded");
    check(!docs.applyThemeToCurrentSlide(QStringLiteral("style999")) &&
              !docs.currentThemeIsOverride(),
          "an unknown theme id is refused and the follow-show state survives");
    check(!docs.setShowDefaultTheme(QStringLiteral("style999")) &&
              docs.currentTheme().value(QStringLiteral("id")).toString() == chorusId,
          "an unknown show default is refused too");
    check(!docs.applyThemeToCurrentSlide(QStringLiteral("Chorus")) &&
              !docs.currentThemeIsOverride(),
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
    check(!bare.createThemeFromCurrent(QStringLiteral("Orphan")) &&
              !bare.setSlideBackground(QString()) &&
              !bare.editTheme(QStringLiteral("style1"), QStringLiteral("x"),
                              QStringLiteral("#fff"), QStringLiteral("#000"), QString(), QString(), 0.0,
                              QStringLiteral("center"), 1.25),
          "with no current slide the style panel has nothing to act on");
}

// --- slide clipboard --------------------------------------------------------

// --- a picture in the stack, seen from the panel (task 6.17b) ---------------

// The editor that AUTHORS a foreground picture is 6.17b2, so this fixture's
// service is a FILE: one slide, a verse under a picture, the picture's bytes
// in the package. Everything below is what the panel does with a stack it did
// not build — the canvas's list, the paste, the asset's removal, the save —
// none of which has an "add picture" button to lean on yet.
void writeMixedService(const std::filesystem::path& service) {
    std::filesystem::create_directories(service / "assets");
    std::ofstream plate(service / "assets" / "asset1-plate.png", std::ios::binary);
    plate << "PLATEBYTES";
    plate.close();
    std::ofstream out(service / "service.json");
    out << R"svc({"format":3,"nextOrdinal":90,"shows":[{"id":"show1","name":"Mixed","notes":"","defaultStyleId":"","slides":[{"id":"slide1","group":"","items":[{"id":"text1","kind":"text","lines":["Verse"],"x":160,"y":100,"width":1600,"height":880},{"id":"image2","kind":"image","assetId":"asset1","x":1000,"y":80,"width":800,"height":450}],"notes":"","styleId":"","backgroundAssetId":""}]}],"styles":[],"assets":[{"id":"asset1","name":"plate.png","file":"assets/asset1-plate.png"}],"playlists":[]})svc";
    out.close();
}

void exerciseImageItemSession(const std::filesystem::path& root) {
    const std::filesystem::path service = root / "service";
    writeMixedService(service);
    DocumentController docs(nullptr, root);
    docs.selectSong(QStringLiteral("show1"));
    docs.selectSlide(0);
    const QString slide1 = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    const std::string packaged = (service / "assets" / "asset1-plate.png").string();

    // The canvas chrome has to tell a picture's outline from a text box's.
    // Guessing from "no words" is the inference that breaks the first time a
    // slide has an empty verse, so the kind is handed to it.
    const QVariantList outlines = docs.currentSlideBoxes();
    check(outlines.size() == 2 &&
              outlines.at(0).toMap().value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("text") &&
              outlines.at(1).toMap().value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("image") &&
              outlines.at(1).toMap().value(QStringLiteral("assetId")).toString() ==
                  QStringLiteral("asset1") &&
              outlines.at(1).toMap().value(QStringLiteral("x")).toDouble() == 1000.0,
          "the canvas is handed the picture AS a picture: its kind, its asset, its box");

    const openshow::scene::Scene mixed = compiledFor(docs, slide1);
    check(mixed.items.size() == 2 && mixed.items[1].isImage &&
              mixed.items[1].assetPath == packaged && mixed.items[1].lines.empty(),
          "and the scene the panel hands the worker resolves that asset to its file");

    // Typing into a selected picture is refused by the model. What the session
    // owns is what does NOT happen: no words in the picture, none in the box
    // under it, and no dirty flag — so the autosave and the close prompt both
    // still describe the deck the operator has.
    check(docs.selectTextBox(1) && docs.currentBox() == 1, "the picture can be selected");
    docs.setSlideText(QStringLiteral("words into a picture"));
    docs.flushText();
    const openshow::scene::Scene afterTyping = compiledFor(docs, slide1);
    check(afterTyping.items[1].lines.empty() &&
              afterTyping.items[0].lines == std::vector<std::string>{"Verse"} && !docs.dirty(),
          "typing on a selected picture writes nowhere — not into the picture, not into "
          "the box under it — and leaves the session clean");

    // --- the paste re-identifies a picture exactly as it does a text box ---
    docs.selectSlide(0);
    check(docs.copyCurrentSlide() && docs.pasteSlide(), "the mixed slide copies and pastes");
    check(docs.slides().size() == 2, "paste inserted one slide");
    const QString pastedId =
        docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();
    const openshow::scene::Scene pasted = compiledFor(docs, pastedId);
    check(pasted.items.size() == 2 && pasted.items[1].isImage &&
              pasted.items[1].assetPath == packaged && pasted.items[1].x == 1000.0 &&
              pasted.items[0].lines == std::vector<std::string>{"Verse"},
          "the pasted slide carries the picture as well as the words, in the same order");
    const QVariantList pastedBoxes = docs.currentSlideBoxes();
    const QString pastedPicture =
        pastedBoxes.at(1).toMap().value(QStringLiteral("id")).toString();
    check(!pastedPicture.isEmpty() && pastedPicture != QStringLiteral("image2"),
          "and the pasted picture wears a fresh id: one asset placed twice is two stack "
          "entries, never one edited into the other");
    check(docs.pasteSlide() && docs.slides().size() == 3, "the same slide pastes a second time");
    const QVariantList twice = docs.currentSlideBoxes();
    check(twice.size() == 2 &&
              twice.at(1).toMap().value(QStringLiteral("id")).toString() != pastedPicture,
          "the second paste gets its own picture id too");

    check(docs.save(), "the session with pasted pictures saves");
    openshow::domain::Documents fromDisk;
    std::string loadError;
    check(openshow::domain::loadService(&fromDisk, service, &loadError),
          "and the package with three copies of one picture loads");
    if (const openshow::domain::Show* show = fromDisk.findShow("show1")) {
        std::size_t pictures = 0;
        std::set<std::string> itemIds;
        for (const openshow::domain::Slide& slide : show->slides) {
            for (const openshow::domain::SlideItem& item : slide.items) {
                itemIds.insert(item.id);
                pictures += item.kind == openshow::domain::ItemKind::Image ? 1u : 0u;
            }
        }
        check(pictures == 3 && itemIds.size() == 6,
              "three pictures, six items, six identities — a pasted deck that reused an "
              "item id would write a file no loader accepts");
    } else {
        check(false, "the saved package still holds its show");
    }

    // --- deleting the asset takes every copy of its picture with it ---
    check(docs.removeAsset(QStringLiteral("asset1")), "the asset is removed");
    check(docs.currentSlideBoxes().size() == 1 &&
              compiledFor(docs, pastedId).items.size() == 1,
          "every slide loses its picture and keeps its words: an item without its file is "
          "not a half-drawn stack entry");
    docs.undo();
    check(docs.currentSlideBoxes().size() == 2 &&
              compiledFor(docs, pastedId).items[1].isImage,
          "one undo brings the asset back with its pictures beside it, in their own slides");

    // A clip captured with a picture, pasted AFTER the asset is gone: the
    // paste is not refused — the operator's words still arrive — and the
    // picture is what falls away, exactly as a deleted theme or background
    // reference has since 4.5.
    docs.selectSlide(0);
    check(docs.copyCurrentSlide(), "the mixed slide is captured again");
    check(docs.removeAsset(QStringLiteral("asset1")),
          "and its asset is deleted from under the clipboard");
    check(docs.pasteSlide() && docs.slides().size() == 4,
          "the paste still succeeds: a stale picture is not a reason to lose the words");
    const QString staleId =
        docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();
    const openshow::scene::Scene stale = compiledFor(docs, staleId);
    check(stale.slideId != slide1.toStdString() && stale.items.size() == 1 &&
              !stale.items[0].isImage &&
              stale.items[0].lines == std::vector<std::string>{"Verse"},
          "and the pasted slide carries the words with no picture item in it");
}

// --- authoring pictures from the panel (task 6.17b2) ------------------------

// The other half of 6.17b: the controller surface the editor drives — the
// drawer's placement command, the kind/asset views the inspector reads, and
// the honest refusal where a text-only edit lands on a picture. Everything
// the MIXED stack does after that (nudge, reorder, delete, undo, navigation)
// is asserted here too, because the acceptance is that the stack is ONE: the
// same commands, the same selection, for both kinds.
void exerciseImageAuthoring(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Composer"));
    docs.addSlide();
    docs.selectSlide(0);
    const QString slideId = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    docs.setSlideText(QStringLiteral("Hello"));
    docs.flushText();

    // A still to place and a video that must be refused, written as files
    // and imported the way the drawer's dialog hands them over.
    { std::ofstream f(root / "plate.png"); f << "PNGBYTES"; }
    { std::ofstream f(root / "reel.mov"); f << "MOVBYTES"; }
    const QString pngPath = QString::fromStdWString((root / "plate.png").wstring());
    const QString movPath = QString::fromStdWString((root / "reel.mov").wstring());
    check(docs.importMediaFile(pngPath) && docs.importMediaFile(movPath) &&
              docs.assets().size() == 2,
          "a still and a video are imported for the drawer to place");
    const QString plateId =
        docs.assets().at(0).toMap().value(QStringLiteral("id")).toString();
    const QString reelId =
        docs.assets().at(1).toMap().value(QStringLiteral("id")).toString();

    // What the inspector shows before any picture exists: a text selection,
    // no asset, and no pictureInfo to be seen.
    check(docs.currentBox() == 0 && docs.currentBoxKind() == QStringLiteral("text") &&
              docs.currentBoxAsset().isEmpty(),
          "a fresh slide selects its text box and the asset view is empty");

    // --- place: on top, selected, half-canvas, and in the scene ----------
    check(docs.placeAssetOnSlide(plateId), "the drawer can place the still on the slide");
    QVariantList boxes = docs.currentSlideBoxes();
    check(boxes.size() == 2 &&
              boxes.at(1).toMap().value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("image"),
          "the picture joins the TOP of the stack — it is what the audience sees "
          "over the words, and the next Tab will reach it");
    check(docs.currentBox() == 1 && docs.currentBoxKind() == QStringLiteral("image"),
          "placed means selected: the handles go straight to the new picture");
    check(docs.currentBoxAsset().value(QStringLiteral("id")).toString() == plateId &&
              docs.currentBoxAsset().value(QStringLiteral("name")).toString() ==
                  QStringLiteral("plate.png"),
          "the inspector is handed the picture's asset — id and name — instead of a "
          "text field's contents");
    const QVariantMap placed = boxes.at(1).toMap();
    check(placed.value(QStringLiteral("x")).toDouble() == 480.0 &&
              placed.value(QStringLiteral("y")).toDouble() == 270.0 &&
              placed.value(QStringLiteral("width")).toDouble() == 960.0 &&
              placed.value(QStringLiteral("height")).toDouble() == 540.0,
          "the picture arrives as a centred half-canvas box: visibly a new box, not a "
          "full-frame outline coinciding with the default one");
    const openshow::scene::Scene staged = compiledFor(docs, slideId);
    check(staged.items.size() == 2 && staged.items[1].isImage &&
              !staged.items[1].assetPath.empty() && staged.items[0].lines.size() == 1,
          "and the slide compiles with the picture over the words for the worker to draw");

    // --- one stack, one set of commands -----------------------------------
    check(docs.setTextBoxGeometry(490, 280, 960, 540) &&
              compiledFor(docs, slideId).items[1].x == 490.0,
          "a nudge or a drag moves the picture through the same geometry commit a "
          "text box gets");
    check(docs.moveCurrentTextBox(0) && docs.currentBox() == 0 &&
              docs.currentBoxKind() == QStringLiteral("image"),
          "Back sends the picture BEHIND the words and the selection rides along — "
          "reorder follows identity, not kind");
    check(compiledFor(docs, slideId).items[0].isImage,
          "the compiled stack really is re-ordered: the picture draws under the verse now");
    check(docs.selectTextBox(1) && docs.currentBoxKind() == QStringLiteral("text") &&
              docs.currentBoxAsset().isEmpty(),
          "Tab reaches the text box above the picture, and the asset view empties");
    check(docs.selectTextBox(0) && docs.currentBoxKind() == QStringLiteral("image"),
          "Tab back onto the picture — the walk covers the stack in draw order "
          "whatever the kind");
    check(docs.removeCurrentTextBox() && docs.currentSlideBoxes().size() == 1,
          "the same Delete rule takes the picture off the slide");

    // --- undo and redo carry the picture and the selection ----------------
    docs.undo();  // bring the delete back
    check(docs.currentSlideBoxes().size() == 2 && docs.currentBox() == 0 &&
              docs.currentBoxKind() == QStringLiteral("image"),
          "undo restores the deleted picture and the selection follows its id back "
          "onto it, index and all");
    docs.redo();  // delete it again
    check(docs.currentSlideBoxes().size() == 1 &&
              docs.currentBoxKind() == QStringLiteral("text"),
          "redo deletes it again and the selection that named the vanished picture "
          "falls back to the words");
    // Re-place for the remaining scenes; then prove placement itself is ONE
    // history entry by undoing it and watching the slide drop back to words.
    check(docs.placeAssetOnSlide(plateId), "the picture is placed a second time");
    docs.undo();
    check(docs.currentSlideBoxes().size() == 1 &&
              docs.currentBox() == 0 &&
              docs.currentBoxKind() == QStringLiteral("text"),
          "one undo removes one placement, and the selection that named the vanished "
          "picture falls back to the words exactly like a vanished text box would");
    docs.redo();
    check(docs.currentSlideBoxes().size() == 2 && docs.currentBox() == 1 &&
              docs.currentBoxKind() == QStringLiteral("image"),
          "redo brings the picture back still selected — its id survived in the "
          "selection because the selection names identities");

    // Navigation belongs to the slide, not the session: leaving and coming
    // back resets the selection to the bottom of the mixed stack.
    docs.addSlide();
    docs.selectSlide(0);
    check(docs.currentBox() == 0 && docs.currentBoxKind() == QStringLiteral("text"),
          "navigating back to the mixed slide clears the picture selection with the slide");

    // --- a text-only edit on a picture refuses OUT LOUD ---------------------
    check(docs.save(), "a save draws the clean/dirty line the refusal is measured against");
    check(docs.selectTextBox(1) && docs.currentBoxKind() == QStringLiteral("image"),
          "the picture is selected again to be typed on");
    const bool dirtyBefore = docs.dirty();
    docs.setSlideText(QStringLiteral("words on a picture"));
    docs.flushText();
    const openshow::scene::Scene typed = compiledFor(docs, slideId);
    check(docs.status().contains(QStringLiteral("shows a picture")),
          "typing into a selected picture is refused on the status line, not "
          "swallowed into a refused edit()");
    check(typed.items[1].lines.empty() &&
              typed.items[0].lines == std::vector<std::string>{"Hello"},
          "the words went nowhere: not into the picture, not into the box under it");
    check(!dirtyBefore && !docs.dirty(),
          "and the refused typing burst dirties nothing — the deck is what the save left");

    // A burst typed into the WORDS box, with the operator tabbing onto the
    // picture before the debounce lands: the flush-first rule (6.17a2) owns
    // it, so the words belong to the box they were typed into.
    check(docs.selectTextBox(0) && docs.currentBoxKind() == QStringLiteral("text"),
          "the words box is selected for fresh typing");
    docs.setSlideText(QStringLiteral("Verse two"));
    check(docs.selectTextBox(1) && docs.currentBoxKind() == QStringLiteral("image"),
          "the operator tabs onto the picture mid-burst");
    const openshow::scene::Scene flushed = compiledFor(docs, slideId);
    check(flushed.items[0].lines == std::vector<std::string>{"Verse two"} &&
              flushed.items[1].lines.empty(),
          "the burst commits into the words box it was typed into — the selection "
          "moving to a picture neither loses it nor relocates it");

    // Placing a picture while a burst is still in the debounce: the flush a
    // placement opens with is what keeps the words in the box they were
    // typed into, instead of letting the new selection carry them onto the
    // picture — where the guard would then honestly refuse them away.
    check(docs.selectTextBox(0) && docs.currentBoxKind() == QStringLiteral("text"),
          "the words box is selected for a burst that a placement will interrupt");
    docs.setSlideText(QStringLiteral("Verse three"));
    check(docs.placeAssetOnSlide(plateId), "a placement while typing is pending still lands");
    const openshow::scene::Scene placedLate = compiledFor(docs, slideId);
    check(placedLate.items.size() == 3 &&
              placedLate.items[0].lines == std::vector<std::string>{"Verse three"} &&
              placedLate.items[2].isImage && placedLate.items[2].lines.empty(),
          "the pending words commit into the words box before the picture takes the "
          "selection — a placement neither drops nor relocates them");

    // --- the refusals the drawer will hit -----------------------------------
    check(!docs.placeAssetOnSlide(reelId) &&
              docs.status().contains(QStringLiteral("video")) &&
              docs.currentSlideBoxes().size() == 3,
          "a video cannot become a foreground picture, the status says why, and the "
          "stack is untouched");
    check(!docs.placeAssetOnSlide(QStringLiteral("asset999")) &&
              docs.status().contains(QStringLiteral("unknown asset")),
          "an unknown asset id is refused by name");
    check(docs.currentBoxKind() == QStringLiteral("image") && docs.currentBox() == 2,
          "a refused placement does not move the selection either");

    // The ceiling is the stack's own: the remaining placements take the slide
    // to its eight items, and the ninth of any kind lands on the status line.
    for (int i = 0; i < 6 && docs.currentSlideBoxes().size() < 8; ++i) {
        check(docs.placeAssetOnSlide(plateId), "another placement fits under the ceiling");
    }
    check(docs.currentSlideBoxes().size() == 8, "the mixed stack reached the scene's ceiling");
    check(!docs.placeAssetOnSlide(plateId) &&
              docs.status().contains(QStringLiteral("items a scene can carry")),
          "a ninth picture is refused through the model's own ceiling rule — text and "
          "pictures share one budget, so does this door");

    // With nothing under the cursor there is nothing to place onto.
    DocumentController bare(nullptr, root / "bare");
    check(bare.currentBoxKind().isEmpty() && bare.currentBoxAsset().isEmpty(),
          "with no slide there is no kind and no asset for the inspector to show");
    check(!bare.placeAssetOnSlide(plateId) &&
              bare.status().contains(QStringLiteral("no current slide")),
          "with no current slide the drawer's button explains itself and records nothing");
}

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

    // Give slide 1 a full presentation: words, a box, a pinned theme and
    // a background image.
    docs.setSlideText(QStringLiteral("Amazing grace"));
    check(docs.setSlideGeometry(240.0, 120.0, 1400.0, 800.0),
          "the source slide carries a box");
    check(docs.createThemeFromCurrent(QStringLiteral("Bright")),
          "the source slide gets a style of its own");
    const QString themeId = docs.currentTheme().value(QStringLiteral("id")).toString();
    check(docs.editTheme(themeId, QStringLiteral("Bright"), QStringLiteral("#ffd23f"),
                         QStringLiteral("#101828"), QString(), QString(), 64.0,
                         QStringLiteral("left"), 1.5),
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
    check(docs.currentThemeIsOverride() &&
              docs.currentTheme().value(QStringLiteral("id")).toString() == themeId,
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

// --- task 6.17c1 (review R17): the fit is an operator-editable, saved fact --

void exerciseBackgroundFit(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    check(docs.currentBackgroundFit().isEmpty(),
          "with no slide there is no fit to report");
    check(!docs.setSlideBackgroundFit(QStringLiteral("cover")) &&
              docs.status().contains(QStringLiteral("background fit")) &&
              docs.status().contains(QStringLiteral("no current slide")),
          "setting a fit with no slide refuses and names the page it missed");

    docs.createSong(QStringLiteral("Fitted"));
    docs.addSlide();
    check(docs.currentBackgroundFit() == QStringLiteral("cover"),
          "a new slide reports cover — the default every file before format 4 drew");
    check(docs.setSlideBackgroundFit(QStringLiteral("contain")),
          "the operator picks contain from the style panel's names");
    check(docs.currentBackgroundFit() == QStringLiteral("contain"),
          "the property follows the choice");
    const QString slideId =
        docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    check(docs.slides().at(0).toMap().value(QStringLiteral("backgroundFit")).toString() ==
              QStringLiteral("contain"),
          "the slides() view carries the fit per row — the QML panel can show it");
    check(compiledFor(docs, slideId).background.fit ==
              openshow::domain::BackgroundFit::Contain,
          "the compiled scene carries the slide's fit to the worker");

    docs.undo();
    check(docs.currentBackgroundFit() == QStringLiteral("cover"),
          "one undo rolls the fit back");
    docs.redo();
    check(docs.currentBackgroundFit() == QStringLiteral("contain"),
          "one redo brings it forward");

    // The refusal discipline: the domain's name list is the only one the
    // editor speaks, a refused name says itself on the status line, leaves
    // the property where it stood, and — because it never reaches the edit
    // seam — leaves no history entry behind.
    check(!docs.setSlideBackgroundFit(QStringLiteral("fill")) &&
              docs.status().contains(QStringLiteral("background fit")) &&
              docs.status().contains(QStringLiteral("'fill'")),
          "'fill' is refused HERE naming itself — the distorting stretch is not in "
          "the agreed set, and no reader will grow it silently");
    check(docs.currentBackgroundFit() == QStringLiteral("contain"),
          "the refused name changed nothing");
    docs.undo();
    check(docs.currentBackgroundFit() == QStringLiteral("cover"),
          "and left no history entry: one undo still reverts the fit itself");
    docs.redo();

    // The file, not just the session: a save writes the choice (and by the
    // 6.17b1 rule EVERY slide's fit), and a reopened controller reads it
    // back and compiles it.
    check(docs.save(), "the service saves");
    {
        std::ifstream manifest(root / "service" / "service.json");
        const std::string text{std::istreambuf_iterator<char>(manifest),
                               std::istreambuf_iterator<char>()};
        check(text.find("\"format\":7") != std::string::npos &&
                  text.find("\"fit\":\"contain\"") != std::string::npos,
              "the manifest is this build's format (7 since TF.1) and spells the slide's fit");
    }
    {
        DocumentController reopened(nullptr, root);
        check(reopened.currentBackgroundFit() == QStringLiteral("contain"),
              "the reopened service answers with the fit it was saved with");
        const QString reopenedId =
            reopened.slides().at(0).toMap().value(QStringLiteral("id")).toString();
        check(compiledFor(reopened, reopenedId).background.fit ==
                  openshow::domain::BackgroundFit::Contain,
              "and its compiled scene carries it, so the audience gets the letterbox");
    }

    // The clipboard carries the fit the way it carries the box and the
    // theme: a pasted contain slide is a contain slide.
    check(docs.copyCurrentSlide() && docs.pasteSlide(),
          "copy and paste run on the fitted slide");
    check(docs.slides().at(1).toMap().value(QStringLiteral("backgroundFit")).toString() ==
              QStringLiteral("contain"),
          "the pasted row carries the fit — the choice travels with the slide "
          "that authored it");
    check(compiledFor(docs, docs.slides().at(1).toMap().value(QStringLiteral("id")).toString())
              .background.fit == openshow::domain::BackgroundFit::Contain,
          "and the pasted slide compiles contained");
    docs.addSlide();
    check(docs.currentBackgroundFit() == QStringLiteral("cover"),
          "a fresh slide is cover again: the clip does not leak into new slides");

    // --- blur (task 6.17c2): accepted like every other fit on a slide a
    //     blur can be built for, and refused on a VIDEO background before
    //     the edit seam, naming itself ---
    check(docs.setSlideBackgroundFit(QStringLiteral("blur")),
          "the operator picks the blurred backdrop on a plain slide");
    check(docs.currentBackgroundFit() == QStringLiteral("blur"),
          "the property follows the choice");
    const int blurRow = docs.slides().size() - 1;
    check(docs.slides().at(blurRow).toMap().value(QStringLiteral("backgroundFit")).toString() ==
              QStringLiteral("blur"),
          "the slides() view carries blur per row — the combo can select it");
    docs.undo();
    check(docs.currentBackgroundFit() == QStringLiteral("cover"),
          "blur is one undo step like every fit before it");
    docs.redo();
    check(docs.setSlideBackgroundFit(QStringLiteral("contain")),
          "and blur slides move again — it is a stored value, not a trap");
    // The video pairing: refused at the seam, not at present time. The
    // refusal leaves the fit where it stood and leaves no history entry
    // (the same discipline the unknown-name refusal is pinned with).
    const QString videoPath = QString::fromStdWString((root / "loop-video.mp4").wstring());
    check(docs.importMediaFile(videoPath) && docs.assets().size() == 1,
          "a video file is registered for the background picker");
    const QString videoAssetId =
        docs.assets().front().toMap().value(QStringLiteral("id")).toString();
    check(docs.setSlideBackground(videoAssetId), "the slide takes the video background");
    check(!docs.setSlideBackgroundFit(QStringLiteral("blur")) &&
              docs.status().contains(QStringLiteral("background fit")) &&
              docs.status().contains(QStringLiteral("does not blur playing frames")),
          "blur on a video background is refused at the seam, saying why");
    check(docs.currentBackgroundFit() == QStringLiteral("contain"),
          "the refused pairing changed nothing");
    docs.undo();
    check(docs.currentBackgroundFit() == QStringLiteral("contain") &&
              docs.slides().at(blurRow).toMap().value(QStringLiteral("backgroundAssetId"))
                  .toString() != videoAssetId,
          "and the refusal left no history entry: one undo is still the video adoption");
    check(docs.setSlideBackgroundFit(QStringLiteral("cover")),
          "the video slide can still take the fits it can draw");
    // Paste honours the pairing too: a blurred slide copied from elsewhere
    // pastes blurred (the clip carries the fit), and adopting a video onto
    // a blurred slide stays blurred in the model — it is the COMPILE that
    // refuses it at present time, loudly, which the scene tests pin.
    docs.addSlide();
    const QString plainId =
        docs.slides().at(docs.slides().size() - 1).toMap().value(QStringLiteral("id")).toString();
    docs.selectSlide(docs.slides().size() - 1);
    check(docs.currentBackgroundFit() == QStringLiteral("cover"),
          "the presenter moves to a fresh cover slide");
    check(docs.setSlideBackgroundFit(QStringLiteral("blur")) &&
              docs.copyCurrentSlide() && docs.pasteSlide(),
          "a blurred plain slide copies and pastes");
    check(docs.slides().at(docs.slides().size() - 1)
              .toMap()
              .value(QStringLiteral("backgroundFit"))
              .toString() == QStringLiteral("blur"),
          "the pasted row carries blur — the choice travels with the slide");
    check(compiledFor(docs, plainId).background.fit == openshow::domain::BackgroundFit::Blur,
          "and a blurred still compiles blurred for the worker to backdrop");
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

// --- task UI.4a: what the media bin reads ---------------------------------
// The bin's rows and its import filter both come out of the controller, and
// both have to be the domain's own answer about a name. A row carrying a kind
// the pipeline disagrees with would let the operator trust a label nothing
// enforces, and a filter offering an extension the bin then calls "other"
// would be a second place to change when the media pipeline learns one.
void exerciseMediaBinViews(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Bin"));
    docs.addSlide();
    check(docs.assets().isEmpty(), "a service starts with no media to list");

    // Registered the way the import dialog hands names over: what a name IS
    // is decided from the name, not from bytes, so these four are listed
    // whether or not anything is behind them (the second half of that —
    // whether the file is there at all — is pinned just below).
    check(docs.importMediaFile(QStringLiteral("/fixtures/loop.mp4"))
              && docs.importMediaFile(QStringLiteral("/fixtures/hymn.MP3"))
              && docs.importMediaFile(QStringLiteral("/fixtures/plate.jpeg"))
              && docs.importMediaFile(QStringLiteral("/fixtures/notes.txt")),
          "video, audio, a picture and an unsupported name are all registered");
    QVariantMap kinds;
    for (const QVariant& entry : docs.assets()) {
        const QVariantMap row = entry.toMap();
        kinds.insert(row.value(QStringLiteral("name")).toString(),
                     row.value(QStringLiteral("kind")).toString());
    }
    check(kinds.value("loop.mp4") == QLatin1String("video")
              && kinds.value("hymn.MP3") == QLatin1String("audio")
              && kinds.value("plate.jpeg") == QLatin1String("picture")
              && kinds.value("notes.txt") == QLatin1String("other"),
          "every asset row carries the kind its name means to the pipeline");

    // And whether the file is where that name says (task UI.4b) — the row the
    // bin shows as "file is not here". These four came from a dialog and were
    // never written, so "not here" is the honest answer for all of them; and
    // because the answer is taken each time the rows are projected, the file
    // that turns up afterwards, or is moved away mid-service, is what the row
    // then says. That is the whole of what a view may claim about a file: one
    // stat. Whether the bytes DECODE is the worker's probe's answer, and the
    // bin waits for it.
    auto existsOf = [&docs](const char* name) {
        for (const QVariant& entry : docs.assets()) {
            const QVariantMap row = entry.toMap();
            if (row.value(QStringLiteral("name")).toString() == QLatin1String(name)) {
                return row.value(QStringLiteral("exists")).toBool();
            }
        }
        return false;
    };
    check(!existsOf("loop.mp4") && !existsOf("notes.txt"),
          "a registered name with nothing behind it reports itself absent");
    const std::filesystem::path bed = root / "bed.mp3";
    { std::ofstream out(bed); out << "AUDIOBYTES"; }
    check(docs.importMediaFile(QString::fromStdString(bed.string())) && existsOf("bed.mp3"),
          "a file that is really there reports itself present");
    std::filesystem::rename(bed, root / "bed-moved-away.mp3");
    check(!existsOf("bed.mp3"),
          "and a file moved out from under the service says so without any edit");

    const QStringList filters = docs.mediaImportNameFilters();
    check(filters.size() == 2 && filters.at(0).startsWith(QStringLiteral("Media files ("))
              && filters.at(1) == QLatin1String("All files (*)"),
          "the import dialog offers one media filter, and everything else");
    auto listed = [&filters](const char* const* names) {
        for (; *names != nullptr; ++names) {
            if (!filters.at(0).contains(QLatin1String(*names))) {
                return false;
            }
        }
        return true;
    };
    check(listed(openshow::domain::kPictureExtensions)
              && listed(openshow::domain::kVideoExtensions)
              && listed(openshow::domain::kAudioExtensions),
          "the filter offers every extension the bin can label a kind");
    check(!filters.at(0).contains(QStringLiteral("txt")),
          "and does not offer a name the bin would have to call other");
}

// Task TH.5's surfaces, at the controller the QML binds: field edits on
// the Edit page write per-slide customizations and leave the shared theme
// entity untouched (the footgun the plan names and removes); the badge's
// revert and the slide's restore refuse when there is nothing to restore;
// and the cue-grid / sidebar bulks are each a single mutation the single
// undo undoes whole.
void exerciseThemeSurfaces(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Alpha");
    docs.addSlide();
    docs.addSlide();
    docs.addSlide();
    docs.selectSlide(0);
    const QString showA = docs.currentSongId();

    check(docs.createBlankTheme("House") && docs.createBlankTheme("Loud"),
          "two blank themes exist");
    QString house;
    QString loud;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("House")) {
            house = theme.toMap().value(QStringLiteral("id")).toString();
        }
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Loud")) {
            loud = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    // Loud gets colours of its own so "which layer is drawing" is visible
    // in the compiled scene however bare the slide is. (House keeps the
    // blank white-on-black.)
    check(docs.editTheme(loud, QStringLiteral("Loud"), QStringLiteral("#ff0000"),
                         QStringLiteral("#100000"), QString(), QString(), 0.0,
                         QStringLiteral("center"), 1.25),
          "Loud is restyled red-on-maroon through the entity edit");
    check(docs.applyThemeToCurrentSlide(house), "the first slide wears House");

    // --- customization, not entity restyle (the plan's footgun, gone) ---
    check(docs.customizeCurrentSlide(QStringLiteral("textColor"),
                                     QStringLiteral("#00ff00")),
          "the Edit page customizes the slide's text colour");
    QVariantMap look = docs.slideAppearance();
    check(look.value(QStringLiteral("textColor")).toMap().value(QStringLiteral("value"))
                  .toString() == QStringLiteral("#00ff00") &&
              look.value(QStringLiteral("textColor")).toMap()
                  .value(QStringLiteral("customized")).toBool() &&
              !look.value(QStringLiteral("fontSize")).toMap()
                   .value(QStringLiteral("customized")).toBool(),
          "the appearance view shows the drawn value and flags the touched field only");
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("id")).toString() == house) {
            check(theme.toMap().value(QStringLiteral("textColor")).toString() ==
                      QStringLiteral("#ffffff"),
                  "the shared House entity is untouched — no silent restyle of its other slides");
        }
    }
    docs.undo();
    look = docs.slideAppearance();
    check(!look.value(QStringLiteral("textColor")).toMap()
                 .value(QStringLiteral("customized")).toBool() &&
              look.value(QStringLiteral("textColor")).toMap()
                  .value(QStringLiteral("value")).toString() == QStringLiteral("#ffffff"),
          "one undo removes the customization and the theme speaks again");

    // Answer-empties and refusals at the panel's level.
    check(docs.customizeCurrentSlide(QStringLiteral("backgroundGradient"), QString()) &&
              docs.customizeCurrentSlide(QStringLiteral("fontSize"), 77.0),
          "an explicit flat and a size are customized");
    look = docs.slideAppearance();
    check(look.value(QStringLiteral("backgroundGradient")).toMap()
                 .value(QStringLiteral("customized")).toBool() &&
              look.value(QStringLiteral("fontSize")).toMap()
                  .value(QStringLiteral("value")).toDouble() == 77.0,
          "the empties that ARE answers customize like any value");
    check(docs.revertCurrentSlideField(QStringLiteral("fontSize")) &&
              !docs.slideAppearance()
                   .value(QStringLiteral("fontSize")).toMap()
                   .value(QStringLiteral("customized")).toBool(),
          "the badge reverts its own field");
    check(!docs.revertCurrentSlideField(QStringLiteral("fontSize")) &&
              docs.status().contains(QStringLiteral("revert")),
          "reverting a field that was never customized is refused on the status line");
    check(!docs.customizeCurrentSlide(QStringLiteral("textColor"),
                                      QStringLiteral("green")) &&
              !docs.customizeCurrentSlide(QStringLiteral("bold"), true),
          "an unusable colour and a field that does not exist are both refused");
    check(docs.slideAppearance().value(QStringLiteral("backgroundGradient")).toMap()
              .value(QStringLiteral("customized")).toBool(),
          "the refusals changed nothing: the flat customization stands");

    // --- the whole-slide restore ---
    check(docs.removeCurrentSlideCustomizations() &&
              docs.slideAppearance().value(QStringLiteral("backgroundGradient")).toMap()
                  .value(QStringLiteral("customized"))
                  .toBool() == false &&
              docs.slideAppearance().isEmpty() == false,
          "Remove customizations sends the slide back to its themes");
    check(!docs.removeCurrentSlideCustomizations() &&
              docs.status().contains(QStringLiteral("no customizations")),
          "restoring a clean slide refuses — nothing was restored");

    // --- cue-grid apply on a BROWSED show's slide ---
    docs.createSong("Beta");
    docs.addSlide();
    docs.setSlideText(QStringLiteral("Beta one"));
    docs.flushText();
    docs.addSlide();
    docs.setSlideText(QStringLiteral("Beta two"));
    docs.flushText();
    const QString showB = docs.currentSongId();
    const QString bSlide1 =
        docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    const QString bSlide2 =
        docs.slides().at(1).toMap().value(QStringLiteral("id")).toString();
    check(docs.applyThemeToSlide(bSlide2, loud),
          "a theme applied to one browsed slide is owned from the slide, not the cursor");
    check(compiledFor(docs, bSlide2).items[0].color == 0xFF0000u,
          "the chosen slide wears Loud while the cursor never moved");
    docs.selectSong(showA);
    check(docs.applyThemeToSlide(bSlide1, loud),
          "the cue grid can theme the OTHER show while Alpha is under edit");
    check(compiledFor(docs, bSlide1).items[0].color == 0xFF0000u, "and it took");

    // --- bulks: one mutation, one undo ---
    check(docs.applyThemeToShow(showB, house), "Apply theme to show themes every slide");
    check(compiledFor(docs, bSlide1).items[0].color == 0xFFFFFFu &&
              compiledFor(docs, bSlide2).items[0].color == 0xFFFFFFu,
          "every slide of the show now follows House");
    docs.undo();
    check(compiledFor(docs, bSlide1).items[0].color == 0xFF0000u &&
              compiledFor(docs, bSlide2).items[0].color == 0xFF0000u,
          "one undo removed the WHOLE bulk — both slides are back at Loud, not one");

    // A bulk over an empty show is a wrong click, refused.
    docs.createSong("Empty");
    check(!docs.applyThemeToShow(docs.currentSongId(), house) &&
              !docs.applyThemeToShow(QStringLiteral("show-gone"), house),
          "no slides and no show are both refused as bulks");

    // --- sidebar restore: all customizations in a show, one undo ---
    docs.selectSong(showA);
    docs.selectSlide(0);
    check(docs.customizeCurrentSlide(QStringLiteral("textColor"),
                                     QStringLiteral("#123456")),
          "Alpha's first slide customizes");
    // Slide 2 needs words to compile with an item colour.
    docs.selectSlide(1);
    docs.setSlideText(QStringLiteral("Second"));
    docs.flushText();
    check(docs.customizeCurrentSlide(QStringLiteral("backgroundColor"),
                                     QStringLiteral("#654321")),
          "and the second slide customizes its own field");
    check(docs.removeShowCustomizationsFor(showA) &&
              docs.slideAppearance().value(QStringLiteral("backgroundColor")).toMap()
                  .value(QStringLiteral("customized"))
                  .toBool() == false,
          "Remove all customizations in show clears every slide");
    docs.selectSlide(0);
    check(!docs.slideAppearance().value(QStringLiteral("textColor")).toMap()
                .value(QStringLiteral("customized")).toBool(),
          "the other slide's customization went too");
    check(!docs.removeShowCustomizationsFor(showA) &&
              docs.status().contains(QStringLiteral("restore")),
          "restoring a clean show refuses");
    docs.undo();
    docs.selectSlide(0);
    check(docs.slideAppearance().value(QStringLiteral("textColor")).toMap()
              .value(QStringLiteral("customized")).toBool() &&
              docs.slideAppearance().value(QStringLiteral("textColor")).toMap()
                  .value(QStringLiteral("value")).toString() == QStringLiteral("#123456"),
          "one undo brings the whole bulk restore back");

    // --- header selector: the default of the show BEING VIEWED ---
    docs.selectSong(showA);
    check(docs.setShowDefaultThemeFor(showB, loud),
          "the header sets a browsed show's default without touching the cursor");
    QVariant rowB;
    QVariant rowA;
    for (const QVariant& song : docs.songs()) {
        if (song.toMap().value(QStringLiteral("id")).toString() == showB) rowB = song;
        if (song.toMap().value(QStringLiteral("id")).toString() == showA) rowA = song;
    }
    check(rowB.toMap().value(QStringLiteral("defaultThemeId")).toString() == loud &&
              rowA.toMap().value(QStringLiteral("defaultThemeId")).toString().isEmpty(),
          "only the browsed show's default changed — the view names it");
    check(!docs.setShowDefaultThemeFor(QString(), house),
          "no show selected is refused");
}

// The themes plan amendment (2026-11-01) through the controller: the Theme
// Editor's two writes and the rows they show, and the Edit page's routing —
// a box the chain places CUSTOMIZES its spot, every other box edits its
// authored rectangle exactly as it always did.
void exercisePlacementSurfaces(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Placed");
    docs.addSlide();
    docs.addSlide();
    docs.selectSlide(0);
    check(docs.createBlankTheme("House"), "a blank theme exists to place with");
    // Looked up BY NAME: row 0 is the seeded Default floor now (amendment
    // 2026-11-02), and the placement below belongs to the operator's House.
    QString house;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("House")) {
            house = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!house.isEmpty() && house != QStringLiteral("default"),
          "the placement test's theme is House, not the seeded floor");
    const auto rowFor = [&docs](const QString& id) {
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("id")).toString() == id) {
                return theme.toMap();
            }
        }
        return QVariantMap{};
    };
    // Rows by index, RE-READ each time: currentSlideBoxes() is a fresh list
    // whenever the document moves, so a handle kept across an edit would be
    // a snapshot of the past.
    const auto appliedBox = [&docs](int index) {
        return docs.currentSlideBoxes().at(index).toMap();
    };
    const auto slideIdAt = [&docs](int index) {
        return docs.slides().at(index).toMap().value(QStringLiteral("id")).toString();
    };
    const auto placementValue = [&docs](const char* key) {
        return docs.slideAppearance().value(QStringLiteral("placement")).toMap()
            .value(QStringLiteral("value")).toMap().value(QLatin1String(key)).toDouble();
    };
    const auto placementCustomized = [&docs]() {
        return docs.slideAppearance().value(QStringLiteral("placement")).toMap()
            .value(QStringLiteral("customized")).toBool();
    };
    // Words, so the compile has an item to place; and the slide WEARS the
    // theme, because a theme that nothing wears has nowhere to place anything.
    docs.setSlideText(QStringLiteral("Placed one"));
    docs.flushText();
    check(docs.applyThemeToCurrentSlide(house), "the first slide wears House");

    // A new theme has no opinion, and the row says so in both shapes: a
    // flag and an empty rectangle, because a rectangle has no honest zero.
    check(rowFor(house).value(QStringLiteral("placementSet")).toBool() == false &&
              rowFor(house).value(QStringLiteral("placement")).toMap().isEmpty(),
          "a theme with no placement reads placementSet false with no rectangle");
    const QVariantMap before = appliedBox(0);
    check(before.value(QStringLiteral("placed")).toBool() == false,
          "and its slides' boxes are not placed — the canvas draws the authored box");

    // The Theme Editor's exact-number path.
    check(docs.setThemePlacement(house, 300.0, 400.0, 1200.0, 300.0),
          "the Theme Editor sets a placement");
    check(rowFor(house).value(QStringLiteral("placementSet")).toBool() &&
              rowFor(house).value(QStringLiteral("placement")).toMap()
                  .value(QStringLiteral("width")).toDouble() == 1200.0,
          "the themes row carries the rectangle the canvas is drawing");
    check(!docs.setThemePlacement(house, 10.0, 10.0, 0.0, 300.0) &&
              docs.status().contains(QStringLiteral("theme placement")),
          "a box with no width is refused on the status line, in the page's own words");
    check(rowFor(house).value(QStringLiteral("placement")).toMap()
                  .value(QStringLiteral("width")).toDouble() == 1200.0,
          "and the refusal left the theme's rectangle alone");

    // What the placement does to the slide under the cursor: the row the
    // canvas outlines moves, the appearance row says who is driving, and the
    // hit test follows the DRAWN box rather than the authored one.
    check(placementValue("x") == 300.0 && placementValue("height") == 300.0 &&
              !placementCustomized(),
          "the appearance view resolves the theme's rectangle with nothing customized");
    check(appliedBox(0).value(QStringLiteral("x")).toDouble() == 300.0 &&
              appliedBox(0).value(QStringLiteral("placed")).toBool(),
          "the box row carries the applied rectangle and says it is applied");
    check(docs.boxAt(301.0, 401.0) == 0,
          "a point inside the themed box hits it");
    check(docs.boxAt(before.value(QStringLiteral("x")).toDouble() + 1.0,
                      before.value(QStringLiteral("y")).toDouble() + 1.0) == -1,
          "and the authored corner it no longer occupies is empty canvas — the hit "
          "rule reads the audience's picture, not the history underneath");
    check(compiledFor(docs, docs.slides().at(0).toMap().value(QStringLiteral("id")).toString())
              .items[0].x == 300.0,
          "the wire scene draws it there too — chrome and audience cannot disagree");

    // The sample the Theme Editor previews wears the same rectangle.
    openshow::scene::Scene sample;
    const QString sampleJson = docs.themeSampleSceneJson(house);
    check(!sampleJson.isEmpty() &&
              openshow::scene::fromJson(boost::json::parse(sampleJson.toStdString()), &sample) &&
              !sample.items.empty() && sample.items[0].x == 300.0 &&
              sample.items[0].y == 400.0,
          "the theme sample shows the placement, so the operator drags what the "
          "audience will get");

    // Dragging a PLACED box customizes: one unit, the authored rectangle
    // survives underneath, and the revert hands the box back to the theme.
    check(docs.selectTextBox(0) && docs.setTextBoxGeometry(200.0, 500.0, 900.0, 200.0),
          "a drag on the placed box commits");
    check(placementCustomized() && placementValue("y") == 500.0,
          "the spot is now a placement CUSTOMIZATION of this slide");
    check(appliedBox(0).value(QStringLiteral("x")).toDouble() == 200.0,
          "and the canvas follows the slide's own spot over the theme's");
    check(docs.revertCurrentSlideField(QStringLiteral("placement")) &&
              !placementCustomized() &&
              appliedBox(0).value(QStringLiteral("x")).toDouble() == 300.0,
          "the badge reverts the whole rectangle back to the theme in one step");
    check(!docs.revertCurrentSlideField(QStringLiteral("placement")) &&
              docs.status().contains(QStringLiteral("revert")),
          "reverting a placement the slide never wrote is refused on the status line");
    // The authored rectangle is still there underneath. First proof: with
    // the slide's customization gone, an explicit write to the placed box
    // lands as a customization of EXACTLY those four numbers, not as a move
    // of the authored box.
    check(docs.setTextBoxGeometry(210.0, 510.0, 910.0, 210.0) && placementCustomized() &&
              placementValue("width") == 910.0,
          "and the next drag is a customization again, as one unit");
    check(docs.revertCurrentSlideField(QStringLiteral("placement")),
          "the box is handed back to the theme once more");
    // Real proof, on the saved file: restore the slide's AUTHORED rectangle
    // (the one operation the routing never touches) and check what the file
    // says about the box underneath.
    const QString slide1 = slideIdAt(0);
    check(docs.setSlideGeometry(123.0, 45.0, 1200.0, 300.0) &&
              compiledFor(docs, slide1).items[0].x == 300.0,
          "moving the SLIDE's box leaves the placed box drawn where the theme says");
    check(docs.save(), "the service saves");
    // The authored rectangle is read out of the file's own JSON, not hunted
    // as text: boost writes doubles in shortest form (123 as 1.23E2), so a
    // substring would be a guess about the serializer's spelling.
    {
        const boost::json::value manifest =
            boost::json::parse(readManifest(root / "service" / "service.json"));
        const boost::json::array& items = manifest.at("shows").at(0).at("slides").at(0)
                                             .at("items").as_array();
        check(items[0].at("x").as_double() == 123.0 && items[0].at("y").as_double() == 45.0,
              "and the file records the authored rectangle the routing preserved");
    }
    // The theme's placement then travels through the file: reload and the
    // same slide draws the theme's rectangle again.
    check(docs.setThemePlacement(house, 350.0, 450.0, 1100.0, 280.0) && docs.save(),
          "the theme's placement is written too");
    check(readManifest(root / "service" / "service.json").find("\"placement\"") !=
              std::string::npos,
          "the manifest carries the theme's placement object");
    {
        DocumentController reloadedController(nullptr, root);
        check(reloadedController.load(), "the saved package reloads");
        reloadedController.selectSong(docs.currentSongId());
        reloadedController.selectSlide(0);
        const QVariantMap reloaded = reloadedController.currentSlideBoxes().at(0).toMap();
        check(reloaded.value(QStringLiteral("x")).toDouble() == 350.0 &&
                  reloaded.value(QStringLiteral("placed")).toBool(),
              "and the reloaded slide draws the theme's rectangle over its authored box");
    }

    // An EXTRA box is geometry, never a placement: the theme drives one box.
    check(docs.addTextBox() && docs.setTextBoxGeometry(60.0, 70.0, 300.0, 120.0) &&
              appliedBox(1).value(QStringLiteral("x")).toDouble() == 60.0 &&
              appliedBox(1).value(QStringLiteral("placed")).toBool() == false &&
              docs.slideAppearance().value(QStringLiteral("placement")).toMap()
                  .value(QStringLiteral("customized")).toBool() == false,
          "a second box keeps authored geometry and customizes no placement");

    // Placement is one undo like every other edit.
    check(docs.setThemePlacement(house, 400.0, 200.0, 1000.0, 400.0) &&
              appliedBox(0).value(QStringLiteral("x")).toDouble() == 400.0,
          "the theme's new rectangle is drawn at once");
    docs.undo();
    check(appliedBox(0).value(QStringLiteral("x")).toDouble() == 350.0,
          "one undo steps the theme's rectangle back to the one before it");
    docs.redo();
    check(appliedBox(0).value(QStringLiteral("x")).toDouble() == 400.0,
          "and redo puts it back");

    // Duplicate carries the placement, and "wear what this slide draws"
    // snapshots WHERE it draws as well as what.
    check(docs.duplicateTheme(house), "the theme is duplicated");
    QString copy;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() ==
            QStringLiteral("House copy")) {
            copy = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!copy.isEmpty() &&
              rowFor(copy).value(QStringLiteral("placementSet")).toBool() &&
              rowFor(copy).value(QStringLiteral("placement")).toMap()
                  .value(QStringLiteral("height")).toDouble() == 400.0,
          "the duplicate places the same rectangle — a starting point to edit, "
          "not a look with its position quietly missing");
    check(docs.createThemeFromCurrent(QStringLiteral("From slide")),
          "a theme made from the current slide snapshots its look");
    QString fromSlide;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() ==
            QStringLiteral("From slide")) {
            fromSlide = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(rowFor(fromSlide).value(QStringLiteral("placementSet")).toBool() &&
              rowFor(fromSlide).value(QStringLiteral("placement")).toMap()
                  .value(QStringLiteral("x")).toDouble() == 400.0,
          "and it snapshots the placement the slide draws through the chain");

    // A slide with no theme anywhere keeps its authored box: the whole
    // amendment is silent until someone draws a rectangle.
    docs.selectSlide(1);
    check(docs.slideAppearance().value(QStringLiteral("placement")).toMap()
              .value(QStringLiteral("value")).toMap().isEmpty() &&
              appliedBox(0).value(QStringLiteral("placed")).toBool() == false,
          "the second slide, wearing nothing, resolves no placement at all");

    // TH.9's headline routing: with the chain silent the PRIMARY box no
    // longer edits its authored rectangle either — a drag there records a
    // placement CUSTOMIZATION, and the authored numbers stay the legacy
    // fallback that no surface write touches any more.
    docs.setSlideText(QStringLiteral("Silent one"));
    docs.flushText();
    const double authoredX = appliedBox(0).value(QStringLiteral("x")).toDouble();
    check(appliedBox(0).value(QStringLiteral("primary")).toBool() && !placementCustomized(),
          "the silent slide starts authored, and its first row says it is the primary box");
    check(docs.selectTextBox(0) && docs.setTextBoxGeometry(260.0, 120.0, 1500.0, 700.0) &&
              placementCustomized() && placementValue("x") == 260.0,
          "a drag on the UNPLACED primary box records a placement customization");
    check(appliedBox(0).value(QStringLiteral("x")).toDouble() == 260.0 &&
              appliedBox(0).value(QStringLiteral("placed")).toBool(),
          "the row applies the slide's own spot — the chain speaks through the customization");
    check(docs.save(), "the silent-customized service saves");
    {
        const boost::json::value manifest =
            boost::json::parse(readManifest(root / "service" / "service.json"));
        const boost::json::array& slides = manifest.at("shows").at(0).at("slides").as_array();
        check(slides[1].at("items").as_array()[0].at("x").as_double() == authoredX,
              "the file's authored rectangle is exactly what it was — the drag landed "
              "in customizations, not on the slide's own record");
    }
    check(docs.revertCurrentSlideField(QStringLiteral("placement")) && !placementCustomized() &&
              appliedBox(0).value(QStringLiteral("x")).toDouble() == authoredX &&
              appliedBox(0).value(QStringLiteral("placed")).toBool() == false,
          "one revert hands the silent slide back to its authored box");
}

// Task BI.3: the importer's single destination action, seen from the
// workspace. One Add writes show + slides + schedule row as ONE history
// unit; the item answers to its playlist and to nothing in the Library;
// save and reopen keep exactly the selected content and the provenance;
// and an add whose target vanished is invalidated rather than retargeted.
void exerciseReadingContexts(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Contexted");
    docs.addSlide();
    docs.selectSlide(0);
    check(docs.createBlankTheme("House"), "a blank theme exists to carry a reading context");
    QString house;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("House")) {
            house = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!house.isEmpty(), "the reading-context test's theme is House");
    // The passage's chain runs through the CURRENT theme, so wear House
    // before anything else here — and before the context edits, so the
    // undo arithmetic below counts only the context's own entries.
    check(docs.useThemeAsCurrent(house), "the service wears House");
    // A theme that carries nothing answers `set` false — and so do the
    // wrong ids, because the honest answer to "which context?" when there
    // is no such thing is silence, not a half-map.
    check(docs.themeContext(house, QStringLiteral("scripture"))
                  .value(QStringLiteral("set"))
                  .toBool() == false,
          "a theme with no reading context answers set false");
    check(docs.themeContext(QStringLiteral("theme-nope"), QStringLiteral("scripture"))
                          .value(QStringLiteral("set"))
                          .toBool() == false,
          "an unknown theme answers the same way, not with numbers");

    // The editor's create: every field it shows, stated. The reply is the
    // same shape the entry has — a flag per property, silence flaggable.
    QVariantMap create;
    create.insert(QStringLiteral("fontSize"), 32.0);
    create.insert(QStringLiteral("lineSpacing"), 1.3);
    create.insert(QStringLiteral("align"), QStringLiteral("left"));
    QVariantMap box;
    box.insert(QStringLiteral("x"), 200.0);
    box.insert(QStringLiteral("y"), 120.0);
    box.insert(QStringLiteral("width"), 1200.0);
    box.insert(QStringLiteral("height"), 760.0);
    create.insert(QStringLiteral("box"), box);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), create),
          "the editor creates the scripture context with size, spacing, alignment and box");
    const QVariantMap made = docs.themeContext(house, QStringLiteral("scripture"));
    check(made.value(QStringLiteral("set")).toBool() &&
              made.value(QStringLiteral("name")).toString() == QStringLiteral("scripture") &&
              made.value(QStringLiteral("fontSize")).toDouble() == 32.0 &&
              made.value(QStringLiteral("lineSpacing")).toDouble() == 1.3 &&
              made.value(QStringLiteral("align")).toString() == QStringLiteral("left") &&
              made.value(QStringLiteral("boxSet")).toBool() &&
              made.value(QStringLiteral("height")).toDouble() == 760.0 &&
              made.value(QStringLiteral("fontFamilySet")).toBool() == false,
          "the entry reads back field for field, with the untouched font flagged silent");

    // The merge contract, stated the only way the file states it: absent
    // keys are SILENCE. A commit that carries only the spacing replaces the
    // entry with one that says nothing about size — which is exactly why
    // the page commits the whole merged entry, not the one edited field.
    QVariantMap spacingOnly;
    spacingOnly.insert(QStringLiteral("lineSpacing"), 1.4);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), spacingOnly),
          "a commit that carries one field replaces the entry as the file does");
    const QVariantMap merged = docs.themeContext(house, QStringLiteral("scripture"));
    check(merged.value(QStringLiteral("lineSpacing")).toDouble() == 1.4 &&
              merged.value(QStringLiteral("fontSizeSet")).toBool() == false &&
              merged.value(QStringLiteral("boxSet")).toBool() == false,
          "absent keys became silence — the old size and box did not survive the rewrite");
    docs.undo();
    check(docs.themeContext(house, QStringLiteral("scripture"))
                      .value(QStringLiteral("fontSize"))
                      .toDouble() == 32.0,
          "and the entry is exactly what the create wrote");

    // The vetting is the loader's: what the editor accepts, the next save
    // cannot refuse.
    QVariantMap crooked;
    crooked.insert(QStringLiteral("align"), QStringLiteral("sideways"));
    check(!docs.applyThemeContext(house, QStringLiteral("scripture"), crooked) &&
              docs.status().contains(QStringLiteral("reading context")),
          "an alignment no file may carry is refused in the page's own words");
    check(docs.themeContext(house, QStringLiteral("scripture"))
                      .value(QStringLiteral("align"))
                      .toString() == QStringLiteral("left"),
          "and the refusal left the entry standing");
    check(!docs.applyThemeContext(QStringLiteral("theme-nope"), QStringLiteral("scripture"),
                                  spacingOnly) &&
              docs.status().contains(QStringLiteral("reading context")),
          "applying to a theme that does not exist is refused the same way");

    // What the context does for the passage's own lookup: the style the
    // Bible page measures against names WHICH context answered — the same
    // word the plan sentence will print — and answers the context's numbers.
    const QVariantMap style = docs.scriptureBoxStyle();
    check(style.value(QStringLiteral("context")).toString() == QStringLiteral("scripture") &&
              style.value(QStringLiteral("fontSize")).toDouble() == 32.0 &&
              style.value(QStringLiteral("boxWidth")).toDouble() == 1200.0,
          "the scripture style answers through the context and names it");
    docs.undo();  // the create itself; refused edits never entered History
    check(docs.themeContext(house, QStringLiteral("scripture"))
                      .value(QStringLiteral("set"))
                      .toBool() == false,
          "...and the theme has no reading context at all — the create was ONE undo");
    check(docs.scriptureBoxStyle().value(QStringLiteral("context")).toString().isEmpty(),
          "and with no context the style names nothing — the plan sentence stays plain");

    // Removal: refuses when there is nothing to remove, and one undo brings
    // the whole entry — every field — back.
    QVariantMap again;
    again.insert(QStringLiteral("fontSize"), 28.0);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), again),
          "the context is created a second time, from one field only");
    check(docs.removeThemeContext(house, QStringLiteral("scripture")),
          "remove takes the entry off");
    check(docs.themeContext(house, QStringLiteral("scripture"))
                      .value(QStringLiteral("set"))
                      .toBool() == false,
          "and the theme carries nothing again");
    check(!docs.removeThemeContext(house, QStringLiteral("scripture")) &&
              docs.status().contains(QStringLiteral("reading context")),
          "removing a context that is not there is refused, not silently true");
    docs.undo();  // of the removal
    const QVariantMap restored = docs.themeContext(house, QStringLiteral("scripture"));
    check(restored.value(QStringLiteral("set")).toBool() &&
              restored.value(QStringLiteral("fontSize")).toDouble() == 28.0 &&
              restored.value(QStringLiteral("alignSet")).toBool() == false,
          "the whole entry returns exactly as it was — fields and all");

    // And it survives the disk: what the editor wrote, the next session
    // reads without a single field drifting.
    check(docs.save(), "the service saves with the context");
    const std::filesystem::path service = root / "service" / "service.json";
    openshow::domain::Documents reopened;
    std::string loadError;
    check(openshow::domain::loadService(&reopened, service.parent_path(), &loadError),
          "the saved service loads");
    const openshow::domain::Theme* diskTheme = reopened.findTheme(house.toStdString());
    check(diskTheme != nullptr && diskTheme->contexts.size() == 1 &&
              diskTheme->contexts[0].name == "scripture" &&
              diskTheme->contexts[0].fontSize.has_value() &&
              *diskTheme->contexts[0].fontSize == 28.0 &&
              !diskTheme->contexts[0].box.has_value(),
          "the file carries the context field for field");
}

// The sample gains a VIEW (task TV.1): the Theme Editor's Songs view is the
// theme's own look and its Bible view is the theme's `scripture` context —
// both compiled through a fixture that walks the SAME chain the audience
// would (a copy of the live records: the edited Default floor in, whatever
// the service happens to be wearing out). Browsing leaves no mark: no
// context appears, no history entry opens, nothing dirties.
void exerciseThemeSampleViews(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Viewed");
    docs.addSlide();
    docs.selectSlide(0);
    check(docs.createBlankTheme("House") && docs.createBlankTheme("Quiet"),
          "two blank themes exist to sample");
    QString house;
    QString quiet;
    QString floorId;
    for (const QVariant& theme : docs.themes()) {
        const QVariantMap map = theme.toMap();
        const QString name = map.value(QStringLiteral("name")).toString();
        if (name == QStringLiteral("House")) {
            house = map.value(QStringLiteral("id")).toString();
        }
        if (name == QStringLiteral("Quiet")) {
            quiet = map.value(QStringLiteral("id")).toString();
        }
        if (map.value(QStringLiteral("builtin")).toBool()) {
            floorId = map.value(QStringLiteral("id")).toString();
        }
    }
    check(!house.isEmpty() && !quiet.isEmpty() && !floorId.isEmpty(),
          "the sample test names House, Quiet and the service's Default floor");

    const QVariantMap songs = docs.themeSampleAppearance(house);
    const QVariantMap inherited = docs.themeSampleAppearance(house,
                                                            QStringLiteral("scripture"));
    check(songs.value(QStringLiteral("ok")).toBool()
              && songs.value(QStringLiteral("context")).toString().isEmpty()
              && songs.value(QStringLiteral("boxX")).toDouble() == 160.0
              && songs.value(QStringLiteral("boxWidth")).toDouble() == 1600.0,
          "the Songs sample of an unplaced theme offers the built-in lyric box");
    check(inherited.value(QStringLiteral("ok")).toBool()
              && inherited.value(QStringLiteral("context")).toString().isEmpty()
              && inherited.value(QStringLiteral("boxWidth")).toDouble()
                     == songs.value(QStringLiteral("boxWidth")).toDouble()
              && inherited.value(QStringLiteral("fontSize")).toDouble()
                     == songs.value(QStringLiteral("fontSize")).toDouble(),
          "a Bible view no theme carries answers as no layer — the Songs look stands");

    // The selected theme speaks through the sample: its own placement and
    // size (as the throwaway show's default, exactly what the one-argument
    // sample compiled before views existed), and its own context for the
    // Bible view only.
    check(docs.setThemePlacement(house, 300, 400, 1200, 600)
              && docs.editTheme(house, QStringLiteral("House"), QStringLiteral("#ffffff"),
                                QStringLiteral("#101010"), QString(), QString(), 40.0,
                                QStringLiteral("center"), 1.25),
          "House is placed and sized");
    QVariantMap lectern;
    lectern.insert(QStringLiteral("fontSize"), 28.0);
    QVariantMap ctxBox;
    ctxBox.insert(QStringLiteral("x"), 480.0);
    ctxBox.insert(QStringLiteral("y"), 120.0);
    ctxBox.insert(QStringLiteral("width"), 960.0);
    ctxBox.insert(QStringLiteral("height"), 840.0);
    lectern.insert(QStringLiteral("box"), ctxBox);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), lectern),
          "House gains a scripture context with its own size and lectern box");
    const QVariantMap songLook = docs.themeSampleAppearance(house);
    const QVariantMap bibleLook = docs.themeSampleAppearance(house,
                                                             QStringLiteral("scripture"));
    check(songLook.value(QStringLiteral("boxX")).toDouble() == 300.0
              && songLook.value(QStringLiteral("fontSize")).toDouble() == 40.0
              && songLook.value(QStringLiteral("context")).toString().isEmpty(),
          "the Songs view wears the theme's own placement and size; the context says nothing there");
    check(bibleLook.value(QStringLiteral("boxX")).toDouble() == 480.0
              && bibleLook.value(QStringLiteral("boxHeight")).toDouble() == 840.0
              && bibleLook.value(QStringLiteral("fontSize")).toDouble() == 28.0
              && bibleLook.value(QStringLiteral("context")).toString()
                     == QStringLiteral("scripture"),
          "the Bible view resolves the context's lectern and names the context that answered");

    // The two views' sample PICTURES differ in words as well as boxes: the
    // song cue carries the pangram, the passage carries a reference,
    // running text and Unicode — the shapes each view is for.
    openshow::scene::Scene cue;
    openshow::scene::Scene passage;
    const QString songJson = docs.themeSampleSceneJson(house);
    const QString bibleJson = docs.themeSampleSceneJson(house, QStringLiteral("scripture"));
    bool cueIsSong = false;
    bool passageHasGreek = false;
    bool passageHasReference = false;
    if (!songJson.isEmpty() && !bibleJson.isEmpty()
            && openshow::scene::fromJson(boost::json::parse(songJson.toStdString()), &cue)
            && openshow::scene::fromJson(boost::json::parse(bibleJson.toStdString()), &passage)
            && !cue.items.empty() && !passage.items.empty()) {
        cueIsSong = std::any_of(cue.items[0].lines.begin(), cue.items[0].lines.end(),
                                [](const std::string& line) {
                                    return line.find("quick brown fox") != std::string::npos;
                                });
        for (const std::string& line : passage.items[0].lines) {
            if (line.find("χάρις") != std::string::npos) {
                passageHasGreek = true;
            }
            if (line.find("1 Corinthians") != std::string::npos) {
                passageHasReference = true;
            }
        }
    }
    check(cueIsSong, "the Songs sample compiles the song cue");
    check(passageHasGreek && passageHasReference,
          "the Bible sample compiles the READING — reference, running text and Unicode — "
          "not the song's pangram");

    // Chain honesty (review R4): the fixture is a copy of the LIVE records
    // — a floor context reaches a theme that carries none, and a theme
    // that carries one answers wholesale.
    QVariantMap floorContext;
    floorContext.insert(QStringLiteral("fontSize"), 24.0);
    check(docs.applyThemeContext(floorId, QStringLiteral("scripture"), floorContext),
          "the Default floor gains a scripture size of its own");
    const QVariantMap through = docs.themeSampleAppearance(house,
                                                            QStringLiteral("scripture"));
    check(through.value(QStringLiteral("fontSize")).toDouble() == 28.0
              && through.value(QStringLiteral("boxX")).toDouble() == 480.0,
          "a theme that carries the context outranks the floor's WHOLESALE — fields never mix");
    const QVariantMap quietBible = docs.themeSampleAppearance(quiet,
                                                               QStringLiteral("scripture"));
    check(quietBible.value(QStringLiteral("fontSize")).toDouble() == 24.0
              && quietBible.value(QStringLiteral("context")).toString()
                     == QStringLiteral("scripture")
              && quietBible.value(QStringLiteral("boxWidth")).toDouble() == 1600.0,
          "a theme that carries nothing takes the floor's size and the inherited box");

    // A view nobody carries is silence, not a guess.
    const QVariantMap unknown = docs.themeSampleAppearance(house, QStringLiteral("anthem"));
    check(unknown.value(QStringLiteral("ok")).toBool()
              && unknown.value(QStringLiteral("context")).toString().isEmpty()
              && unknown.value(QStringLiteral("fontSize")).toDouble() == 40.0
              && unknown.value(QStringLiteral("boxX")).toDouble() == 300.0,
          "a context name no theme carries resolves as no layer — the theme's own numbers stand");

    // The worn theme cannot bleed into a BROWSED theme's picture: the
    // fixture clears the current theme on its copy, so the sample answers
    // for the selected row and not for whatever the service wears.
    check(docs.useThemeAsCurrent(floorId), "the service now wears the Default floor");
    const QVariantMap browsed = docs.themeSampleAppearance(house);
    check(browsed.value(QStringLiteral("fontSize")).toDouble() == 40.0
              && browsed.value(QStringLiteral("boxX")).toDouble() == 300.0,
          "the browsed theme's sample is not out-resolved by the worn one");

    // And browsing marks nothing: the sample verbs are const, so nothing
    // dirties and the last recorded change is STILL the wear above — one
    // undo rolls back the wear itself, with no context left behind.
    const bool dirtyBefore = docs.dirty();
    docs.themeSampleAppearance(quiet, QStringLiteral("scripture"));
    docs.themeSampleSceneJson(house, QStringLiteral("scripture"));
    check(docs.dirty() == dirtyBefore, "sampling the views never dirties the document");
    docs.undo();
    check(docs.wornThemeId().isEmpty()
              && docs.themeContext(quiet, QStringLiteral("scripture"))
                             .value(QStringLiteral("set"))
                             .toBool() == false,
          "one undo rolls back the WEAR — the browsing left no history and no context behind");
}

void exerciseScripturePassage(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    check(docs.createSong("Ordinary Hymn"), "the Library has one ordinary show");
    const QString service = docs.currentPlaylistId();
    check(!service.isEmpty(), "the seeded Service playlist stands in for today's schedule");
    const int libraryCount = docs.songs().size();
    check(docs.collectionOutline().at(0).toMap().value(QStringLiteral("count")).toInt() ==
              libraryCount,
          "the Library row counts the Library alone");

    const QVariantList slides{
        QVariantList{QStringLiteral("Grace to you and peace"),
                     QStringLiteral("from God our Father.")},
        QVariantList{QStringLiteral("I give thanks to my God")},
    };
    check(docs.addScripturePassage(service, QStringLiteral("1 Corinthians 1:1-2"),
                                   QStringLiteral("ESV"),
                                   QStringLiteral("ESV test licence line."), slides),
          "the importer's Add lands once on today's schedule");
    // The schedule row appeared...
    check(docs.playlistEntries().size() == 1, "the passage is the schedule's first row");
    const QVariantMap row = docs.playlistEntries().at(0).toMap();
    const QString passageShow = row.value(QStringLiteral("showId")).toString();
    check(!passageShow.isEmpty() && row.value(QStringLiteral("slideCount")).toInt() == 2,
          "the row names the new two-slide presentation");
    // ...and nowhere else.
    check(docs.songs().size() == libraryCount,
          "the Library list gained no row — no reusable show was created");
    check(docs.collectionOutline().at(0).toMap().value(QStringLiteral("count")).toInt() ==
              libraryCount,
          "the Library count is unchanged too");
    check(docs.searchShows(QStringLiteral("grace")).isEmpty() &&
              docs.searchShows(QStringLiteral("corinthians")).isEmpty(),
          "the drawer search cannot find the schedule-owned passage");

    // One Undo removes the WHOLE item: row and presentation together.
    docs.undo();
    check(docs.playlistEntries().isEmpty() && docs.songs().size() == libraryCount,
          "one undo removes the schedule row and the presentation with it");
    docs.redo();
    check(docs.playlistEntries().size() == 1 &&
              docs.playlistEntries().at(0).toMap().value(QStringLiteral("showId")).toString() ==
                  passageShow,
          "redo brings the whole item back with the same identity");

    // Save and reopen: the file carries the selected content and the
    // provenance, and the reopened document still keeps it out of the
    // Library.
    check(docs.save(), "the service with its passage saves");
    {
        const boost::json::value manifest =
            boost::json::parse(readManifest(root / "service" / "service.json"));
        const boost::json::array& shows = manifest.at("shows").as_array();
        const boost::json::object* found = nullptr;
        for (const boost::json::value& element : shows) {
            if (element.at("id").as_string() == passageShow.toStdString()) {
                found = &element.as_object();
            }
        }
        check(found != nullptr && found->at("scheduleOwned").as_bool() &&
                  found->at("scripture").at("reference").as_string() ==
                      "1 Corinthians 1:1-2" &&
                  found->at("scripture").at("translation").as_string() == "ESV" &&
                  found->at("scripture").at("attribution").as_string() ==
                      "ESV test licence line.",
              "the file carries the passage as a schedule-owned item with its provenance");
    }
    DocumentController reopened(nullptr, root);
    check(reopened.load(), "the service reopens");
    reopened.selectPlaylist(service);
    check(reopened.playlistEntries().size() == 1, "the reopened schedule still has the passage");
    check(reopened.songs().size() == 1, "and the reopened Library still does not");
    const QVariantList kept = reopened.slidesOf(passageShow);
    check(kept.size() == 2 &&
              kept.at(0).toMap().value(QStringLiteral("preview")).toString() ==
                  QStringLiteral("Grace to you and peace"),
          "the reopened passage shows the selected words");

    // Removal leaves nothing behind: the schedule loses the row and the
    // Library never had the show, so no orphan copy exists anywhere.
    check(reopened.removePlaylistEntry(
              reopened.playlistEntries().at(0).toMap()
                  .value(QStringLiteral("entryId"))
                  .toString()),
          "the schedule row is removed");
    check(reopened.playlistEntries().isEmpty(), "the schedule is empty again");

    // The invalidated target: if today's schedule is gone by the time Add
    // runs, the add refuses — it does not pick a different target.
    DocumentController vanished(nullptr, root);
    check(!vanished.addScripturePassage(QStringLiteral("no-such-playlist"),
                                        QStringLiteral("Psalm 23"), QStringLiteral("NIV"),
                                        QStringLiteral(""), slides),
          "an Add to a schedule that does not exist is refused");
    check(vanished.status().contains(QStringLiteral("schedule")),
          "and the refusal names the missing target");
}

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
    exerciseThemePanel(tempRoot / "styles");
    exerciseWornChain(tempRoot / "wornchain");
    exerciseThemeSurfaces(tempRoot / "surfaces");
    exercisePlacementSurfaces(tempRoot / "placementsurfaces");
    exerciseReadingContexts(tempRoot / "readingcontexts");
    exerciseThemeSampleViews(tempRoot / "sampleviews");
    exerciseScripturePassage(tempRoot / "scripturepassage");
    exerciseMultiBoxSession(tempRoot / "multibox");
    exerciseBoxAuthoring(tempRoot / "boxauthoring");
    exerciseClipboard(tempRoot / "clipboard");
    exerciseBackgroundFit(tempRoot / "fit");
    exerciseImageItemSession(tempRoot / "mixeditem");
    exerciseImageAuthoring(tempRoot / "imageauthoring");
    exerciseMediaBinViews(tempRoot / "mediabin");
    exerciseAutosaveSurfacing(tempRoot / "surfacing");
    exerciseBackgroundPersistence(tempRoot / "background");
    std::filesystem::remove_all(tempRoot);

    if (gFailed) {
        return 1;
    }
    std::printf("document_controller_tests: all fixtures passed\n");
    return 0;
}
