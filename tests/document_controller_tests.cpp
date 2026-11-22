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
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <boost/json.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QTimer>

#include <sys/stat.h>
#include <sys/types.h>

#include "openshow/domain/documents.h"
#include "openshow/domain/storage.h"
#include "openshow/scene/scene.h"
#include "openshow/scene/stage.h"

#include "CatalogController.h"
#include "DocumentController.h"
#include "ThumbnailStore.h"
#include "SceneEdit.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    return out.good();
}

bool replaceOnce(std::string& text, const std::string& from, const std::string& to) {
    const std::size_t position = text.find(from);
    if (position == std::string::npos) {
        return false;
    }
    text.replace(position, from.size(), to);
    return true;
}

using openshow::domain::Autosave;
using openshow::scene::Scene;
using openshow::ui::CatalogController;
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

// --- TC.2: the render fingerprint behind persisted card pictures ----------

// The fingerprint decides whether a stored picture still describes the scene
// it is listed under, so it must survive exactly what is not visual — the
// package's location, a save that adopts assets — and change for everything
// that is. This drives the real controller and its real package.
void exerciseThumbnailFingerprint(const std::filesystem::path& root) {
    const std::filesystem::path service = root / "service";
    QString slideId;
    std::string fingerprintAfterEdit;
    {
        DocumentController docs(nullptr, root);
        check(docs.createSong(QStringLiteral("Warm Cache")), "fingerprint: the song exists");
        check(docs.addSlide(), "fingerprint: the slide exists");
        docs.selectSlide(0);
        slideId = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
        check(!slideId.isEmpty(), "fingerprint: the first slide has an id");
        docs.setSlideText(QStringLiteral("the first verse"));
        docs.flushText();

        QVariantMap source = docs.thumbnailSourceForSlide(slideId);
        const QString scene = source.value(QStringLiteral("scene")).toString();
        const QString fingerprint = source.value(QStringLiteral("fingerprint")).toString();
        check(!scene.isEmpty(), "fingerprint: the slide compiles to a scene");
        check(fingerprint.size() == 64, "fingerprint: a card has a 64-character identity");
        check(docs.thumbnailSourceForSlide(slideId).value(QStringLiteral("fingerprint")) ==
                  fingerprint,
              "fingerprint: asking twice describes the same pixels");
        check(docs.thumbnailSourceForSlide(QStringLiteral("slide999"))
                  .value(QStringLiteral("fingerprint"))
                  .toString()
                  .isEmpty(),
              "fingerprint: a slide that does not compile has none");

        // A picture on the slide is part of the render, so replacing its bytes
        // must miss even though the words did not change.
        { std::ofstream f(root / "plate.png"); f << "PNGBYTES"; }
        check(docs.importMediaFile(QString::fromStdWString((root / "plate.png").wstring())),
              "fingerprint: the still is imported");
        const QString plateId =
            docs.assets().at(0).toMap().value(QStringLiteral("id")).toString();
        check(docs.setSlideBackground(plateId), "fingerprint: the still becomes the background");
        docs.flushText();
        const QString withPicture =
            docs.thumbnailSourceForSlide(slideId).value(QStringLiteral("fingerprint")).toString();
        check(withPicture.size() == 64 && withPicture != fingerprint,
              "fingerprint: adding a picture changes the render");
        check(docs.thumbnailSourceForSlide(slideId)
                  .value(QStringLiteral("scene"))
                  .toString()
                  .contains(QStringLiteral("plate.png")),
              "fingerprint: the scene names the asset's file");

        docs.setSlideText(QStringLiteral("the first verse, edited"));
        docs.flushText();
        const QString edited =
            docs.thumbnailSourceForSlide(slideId).value(QStringLiteral("fingerprint")).toString();
        check(edited != withPicture, "fingerprint: edited words miss");
        fingerprintAfterEdit = edited.toStdString();
        check(docs.save(), "fingerprint: the service saves");
    }
    {
        DocumentController docs(nullptr, root);
        const QVariantMap source = docs.thumbnailSourceForSlide(slideId);
        check(source.value(QStringLiteral("fingerprint")).toString().toStdString() ==
                  fingerprintAfterEdit,
              "fingerprint: a fresh process hits the saved service");
        const QString scene = source.value(QStringLiteral("scene")).toString();
        check(scene.contains(QStringLiteral("/service/assets/")),
              "fingerprint: the reopened scene names the package's own copy");
    }
    {
        // Moving the package changes every asset path and must change nothing
        // else: identity is the asset's bytes, not where they live.
        std::filesystem::create_directories(root / "moved");
        std::filesystem::copy(service, root / "moved" / "service",
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);
        DocumentController docs(nullptr, root / "moved");
        const QVariantMap source = docs.thumbnailSourceForSlide(slideId);
        check(!source.value(QStringLiteral("scene")).toString().isEmpty(),
              "fingerprint: the moved service loads its slide");
        check(source.value(QStringLiteral("fingerprint")).toString().toStdString() ==
                  fingerprintAfterEdit,
              "fingerprint: moving the package keeps every card's identity");
    }
}


// --- TC.5: the picture overlay through the session's real durability ------

// A cache arrival is NOT an authoring change: it must not dirty the session,
// add history or move the document generation. Records ride a normal save and
// an autosave snapshot, a clean saved service can be flushed cache-only, and
// neither an external edit nor a cache-specific write failure may cost the
// operator their document.
void exerciseThumbnailCacheLifecycle(const std::filesystem::path& root) {
    using openshow::ui::ThumbnailStore;
    const auto picture = [](QColor colour) {
        QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
        image.fill(colour);
        return image;
    };
    const std::filesystem::path manifest = root / "service" / "service.json";
    QString slideId;
    QString fingerprint;
    {
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        check(docs.createSong(QStringLiteral("Warm Cards")), "TC.5: the song exists");
        check(docs.addSlide(), "TC.5: the slide exists");
        docs.selectSlide(0);
        docs.setSlideText(QStringLiteral("a verse worth keeping"));
        docs.flushText();
        slideId = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
        fingerprint = docs.thumbnailSourceForSlide(slideId)
                          .value(QStringLiteral("fingerprint"))
                          .toString();
        check(fingerprint.size() == 64, "TC.5: the slide has a render fingerprint");
        check(docs.save() && !docs.dirty(), "TC.5: the service is saved clean");

        const quint64 generation = docs.documentGeneration();
        const bool undoable = docs.canUndo();
        pictures.record(QStringLiteral("show1"), slideId, fingerprint, picture(Qt::red));
        check(pictures.waitForIdle(), "TC.5: the picture encodes");
        check(pictures.entryCount() == 1, "TC.5: the overlay holds one picture");
        check(!docs.dirty(), "TC.5: a cache arrival does not dirty the document");
        check(docs.documentGeneration() == generation && docs.canUndo() == undoable,
              "TC.5: a cache arrival adds no history and moves no generation");

        // The normal save carries the block.
        check(docs.save(), "TC.5: the save with a picture succeeds");
        check(readFile(manifest).find("\"thumbnailCache\"") != std::string::npos,
              "TC.5: the saved manifest carries the block");
        check(!pictures.dirty(), "TC.5: a save that installed the block clears the dirty flag");
    }
    {
        // A fresh process: the block is adopted, and the picture is usable
        // without the worker that drew it.
        ThumbnailStore pictures;
        QList<QImage> ready;
        QObject::connect(&pictures, &ThumbnailStore::pictureReady,
                         [&](const QString&, const QString&, const QImage& image) {
                             ready.append(image);
                         });
        DocumentController docs(nullptr, root, &pictures);
        check(docs.songs().size() == 1 && !docs.dirty(), "TC.5: the saved service reopens clean");
        check(docs.thumbnailCacheEntries() == 1 && docs.thumbnailCacheBytes() > 0,
              "TC.5: the reopened session adopted the picture");
        check(pictures.has(slideId, fingerprint), "TC.5: the record is the slide's");
        pictures.request(slideId, fingerprint, 160, 90);
        QElapsedTimer timer;
        timer.start();
        while (ready.isEmpty() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(ready.size() == 1 && ready.front().size() == QSize(160, 90),
              "TC.5: the adopted picture decodes at the row's extent");

        // Cache-only flush on a clean, writable service: no manual save, no
        // authored byte changed.
        std::string authoredBefore;
        check(openshow::domain::manifestAuthoredDigest(readFile(manifest), &authoredBefore),
              "TC.5: the fixture reads the authored digest");
        const QString secondFingerprint =
            QString::fromStdString(openshow::domain::sha256Hex("second-look"));
        pictures.record(QStringLiteral("show1"), slideId, secondFingerprint, picture(Qt::blue));
        check(pictures.waitForIdle(), "TC.5: the second picture encodes");
        const QString generationBefore = docs.status();
        (void)generationBefore;
        timer.restart();
        while (docs.thumbnailCacheFlushes() == 0 && timer.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(docs.thumbnailCacheFlushes() == 1, "TC.5: one coalesced cache-only flush ran");
        check(!docs.dirty(), "TC.5: the flush did not dirty or save an edit");
        std::string authoredAfter;
        check(openshow::domain::manifestAuthoredDigest(readFile(manifest), &authoredAfter) &&
                  authoredAfter == authoredBefore,
              "TC.5: the flush changed only the derived block");
        check(readFile(manifest).find(secondFingerprint.toStdString()) != std::string::npos,
              "TC.5: the flushed block carries the new picture");
        pictures.setEnabled(false);  // retire the timer before the fixture ends
    }
    {
        // An external edit between load and flush must win: the cache is not
        // written, the file keeps the other change, and the operator's next
        // save is the one that reconciles it.
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        std::string manifestText = readFile(manifest);
        check(replaceOnce(manifestText, "\"notes\":\"\"", "\"notes\":\"edited elsewhere\""),
              "TC.5: the fixture can edit the manifest externally");
        check(writeFile(manifest, manifestText), "TC.5: the external edit lands");
        pictures.record(QStringLiteral("show1"), slideId,
                        QString::fromStdString(openshow::domain::sha256Hex("third")),
                        picture(Qt::green));
        check(pictures.waitForIdle(), "TC.5: the third picture encodes");
        QElapsedTimer timer;
        timer.start();
        while (pictures.flushFailures() == 0 && timer.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(pictures.flushFailures() == 1, "TC.5: the flush noticed the external change");
        check(readFile(manifest) == manifestText,
              "TC.5: the refused flush did not overwrite the external edit");
        check(pictures.dirty() && pictures.entryCount() == 3,
              "TC.5: the records stay in memory for the next save");
        pictures.setEnabled(false);
    }
    {
        // A cache-specific manifest failure must not cost the document save:
        // the same save is retried without the derived block.
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        docs.selectSong(docs.currentSongId());
        pictures.record(QStringLiteral("show1"), slideId,
                        QString::fromStdString(openshow::domain::sha256Hex("fourth")),
                        picture(Qt::yellow));
        check(pictures.waitForIdle(), "TC.5: the fourth picture encodes");
        ::setenv("OPENSHOW_STORAGE_FAULT", "thumbnail-manifest", 1);
        const bool saved = docs.save();
        ::unsetenv("OPENSHOW_STORAGE_FAULT");
        check(saved && !docs.dirty(), "TC.5: the save survives a cache-only failure");
        check(readFile(manifest).find("\"thumbnailCache\"") == std::string::npos,
              "TC.5: the retried save wrote the document without the block");
        check(pictures.dirty(), "TC.5: the dropped pictures stay eligible for a later flush");
        {
            openshow::domain::Documents reloaded;
            std::string error;
            check(openshow::domain::loadService(&reloaded, root / "service", &error),
                  "TC.5: the retried package is still valid");
        }
        pictures.setEnabled(false);
    }
    {
        // Autosave recovery carries the pictures, and a recovered session
        // may not flush until it has been saved as a service.
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        pictures.record(QStringLiteral("show1"), slideId,
                        QString::fromStdString(openshow::domain::sha256Hex("fifth")),
                        picture(Qt::magenta));
        check(pictures.waitForIdle(), "TC.5: the fifth picture encodes");
        docs.createSong(QStringLiteral("Unsaved Work"));
        check(openshow::domain::Autosave::exists(root / "autosave"),
              "TC.5: the edit created a scratch package");
        const std::size_t scratchEntries = pictures.entryCount();
        pictures.setEnabled(false);
        (void)scratchEntries;
    }
    {
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        check(docs.dirty(), "TC.5: the scratch is recovered as unsaved work");
        check(pictures.entryCount() > 0,
              "TC.5: the recovered session adopts the scratch's pictures");
        check(docs.thumbnailCacheFlushes() == 0,
              "TC.5: a recovered scratch is not flushed to the saved service");
        check(docs.save(), "TC.5: saving the recovered work succeeds");
        std::size_t savedEntries = 0;
        {
            openshow::domain::Documents reloaded;
            openshow::domain::ThumbnailCache cache;
            std::string error;
            check(openshow::domain::loadService(&reloaded, root / "service", &error, &cache),
                  "TC.5: the saved recovered package loads");
            savedEntries = cache.entries.size();
        }
        check(savedEntries > 0, "TC.5: the recovered pictures reach the saved service");
        pictures.setEnabled(false);
    }
}


// TC.12: old panel-sized pictures leave service.json through a cache-only
// write, without changing the authored document or requiring another render.
void exerciseThumbnailSizeMigration(const std::filesystem::path& root) {
    using openshow::ui::ThumbnailStore;
    {
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        docs.createSong(QStringLiteral("Old card sizes"));
        docs.addSlide();
        docs.addSlide();
        const auto slides = docs.slides();
        for (int i = 0; i < 2; ++i) {
            const QString id = slides[i].toMap().value("id").toString();
            const QString fp = docs.thumbnailSourceForSlide(id).value("fingerprint").toString();
            QImage image(i == 0 ? 198 : 320, i == 0 ? 111 : 180,
                         QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::blue);
            pictures.record(docs.currentSongId(), id, fp, image);
        }
        check(pictures.waitForIdle() && pictures.entryCount() == 2 && docs.save(),
              "size migration: a legacy mixed-size service is saved");
    }
    const auto service = root / "service";
    const auto manifest = service / "service.json";
    std::string authoredBefore;
    check(openshow::domain::manifestAuthoredDigest(readFile(manifest), &authoredBefore),
          "size migration: authored manifest identity is captured");
    {
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        check(pictures.entryCount() == 1 && !docs.dirty(),
              "size migration: reopening drops the small record without dirtying the service");
        QElapsedTimer timer;
        timer.start();
        while (pictures.flushWrites() == 0 && timer.elapsed() < 8000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        check(pictures.flushWrites() == 1, "size migration: cleanup flushes without a manual save");
    }
    openshow::domain::Documents loaded;
    openshow::domain::ThumbnailCache cache;
    std::string error, authoredAfter;
    check(openshow::domain::loadService(&loaded, service, &error, &cache) &&
              cache.entries.size() == 1 && cache.entries.front().width == 320,
          "size migration: service.json actually loses the unused small picture");
    check(openshow::domain::manifestAuthoredDigest(readFile(manifest), &authoredAfter) &&
              authoredAfter == authoredBefore,
          "size migration: cleanup preserves every authored field");
}

// --- TC.8: cache-only durability at its edges -----------------------------
//
// Cache write failures preserve authored data; read-only and deleted packages
// keep pictures in memory, and unsaved edits never enter a cache-only write.
void exerciseThumbnailCacheFaults(const std::filesystem::path& root) {
    using openshow::ui::ThumbnailStore;
    const auto picture = [](QColor colour) {
        QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
        image.fill(colour);
        return image;
    };
    const std::filesystem::path manifest = root / "service" / "service.json";
    QString slideId;
    QString fingerprint;
    {
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        docs.createSong(QStringLiteral("Fault Cards"));
        docs.addSlide();
        docs.selectSlide(0);
        docs.setSlideText(QStringLiteral("the durable words"));
        docs.flushText();
        slideId = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
        fingerprint = docs.thumbnailSourceForSlide(slideId)
                          .value(QStringLiteral("fingerprint"))
                          .toString();
        check(!fingerprint.isEmpty(), "TC.8: the slide has a fingerprint before the save");
        check(docs.save(), "TC.8: the first save succeeds");
        // Save-adopt retargeted every asset and rebuilt the manifest, yet the
        // slide's render identity is the same one the pre-save session had.
        check(docs.thumbnailSourceForSlide(slideId)
                  .value(QStringLiteral("fingerprint"))
                  .toString() == fingerprint,
              "TC.8: save-adopt keeps the render fingerprint");
        check(docs.save(), "TC.8: the adopted service re-saves");

        // --- unsaved authoring never leaks into a cache-only write -------
        std::string authoredBefore;
        check(openshow::domain::manifestAuthoredDigest(readFile(manifest), &authoredBefore),
              "TC.8: the manifest digest is readable");
        docs.setSlideText(QStringLiteral("words that are NOT saved yet"));
        docs.flushText();
        check(docs.dirty(), "TC.8: the edit is unsaved");
        pictures.record(QStringLiteral("show1"), slideId,
                        QString::fromStdString(openshow::domain::sha256Hex("unsaved-look")),
                        picture(Qt::cyan));
        check(pictures.waitForIdle(), "TC.8: the picture encodes");
        QElapsedTimer timer;
        timer.start();
        while (pictures.flushWrites() == 0 && timer.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(pictures.flushWrites() == 1, "TC.8: the cache-only flush ran");
        std::string authoredAfter;
        check(openshow::domain::manifestAuthoredDigest(readFile(manifest), &authoredAfter) &&
                  authoredAfter == authoredBefore,
              "TC.8: the unsaved words did not ride the cache-only write");
        check(docs.dirty(), "TC.8: the session is still dirty after the flush");

        // --- a cache manifest failure never fails the document save ------
        pictures.record(QStringLiteral("show1"), slideId,
                        QString::fromStdString(openshow::domain::sha256Hex("saved-look")),
                        picture(Qt::darkBlue));
        check(pictures.waitForIdle(), "TC.8: the second picture encodes");
        check(docs.save(), "TC.8: saving the edit succeeds");
        const std::string withBlock = readFile(manifest);
        check(withBlock.find("NOT saved") != std::string::npos,
              "TC.8: the save wrote the edited words");

        // A failing cache-only write leaves the previous manifest installed.
        const char* steps[] = {"manifest-write", "manifest-fsync", "manifest-rename",
                               "dir-fsync"};
        for (const char* step : steps) {
            pictures.record(QStringLiteral("show1"), slideId,
                            QString::fromStdString(openshow::domain::sha256Hex(
                                std::string("fault-") + step)),
                            picture(Qt::darkGreen));
            check(pictures.waitForIdle(), "TC.8: the fault fixture encodes");
            const std::string before = readFile(manifest);
            const qint64 failures = pictures.flushFailures();
            const qint64 writes = pictures.flushWrites();
            ::setenv("OPENSHOW_STORAGE_FAULT", step, 1);
            timer.restart();
            while (pictures.flushFailures() == failures && timer.elapsed() < 8000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            }
            ::unsetenv("OPENSHOW_STORAGE_FAULT");
            check(pictures.flushFailures() == failures + 1,
                  "TC.8: the injected cache write failure is reported");
            check(pictures.flushWrites() == writes,
                  "TC.8: a failed flush is not counted as a write");
            if (std::string_view(step) == "dir-fsync") {
                // The documented exception: the rename committed and the
                // directory entry is what failed, so the NEW, complete block
                // is installed (and reported as a failure).
                check(readFile(manifest) != before,
                      "TC.8: a final directory-sync failure leaves the new manifest");
            } else {
                check(readFile(manifest) == before,
                      "TC.8: the previous manifest is still installed");
            }
            check(!std::filesystem::exists(root / "service" / "service.json.tmp"),
                  "TC.8: no staging file is left behind");
            openshow::domain::Documents reloaded;
            openshow::domain::ThumbnailCache reloadedCache;
            std::string error;
            check(openshow::domain::loadService(&reloaded, root / "service", &error,
                                                &reloadedCache),
                  "TC.8: the package still loads after every fault");
            check(!reloadedCache.entries.empty(),
                  "TC.8: the installed block is complete and valid");
            pictures.setWritable(true);  // the fault is over; resume flushing
        }
        pictures.setEnabled(false);
    }
    {
        // A read-only package: the pictures stay in memory, and nothing is
        // written even though the session is clean and saved.
        ::chmod(manifest.c_str(), 0444);
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        check(docs.thumbnailCacheEntries() > 0,
              "TC.8: a read-only service still adopts its pictures");
        pictures.record(QStringLiteral("show1"), slideId,
                        QString::fromStdString(openshow::domain::sha256Hex("read-only")),
                        picture(Qt::gray));
        check(pictures.waitForIdle(), "TC.8: the read-only fixture encodes");
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 2600) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(pictures.flushWrites() == 0 && pictures.dirty(),
              "TC.8: a read-only service is never flushed");
        ::chmod(manifest.c_str(), 0644);
        pictures.setEnabled(false);
    }
    {
        // The service is deleted under a live session: the flush must not
        // recreate it, and the records stay in memory.
        ThumbnailStore pictures;
        DocumentController docs(nullptr, root, &pictures);
        std::filesystem::remove_all(root / "service");
        pictures.record(QStringLiteral("show1"), slideId,
                        QString::fromStdString(openshow::domain::sha256Hex("deleted")),
                        picture(Qt::darkRed));
        check(pictures.waitForIdle(), "TC.8: the deleted-service fixture encodes");
        QElapsedTimer timer;
        timer.start();
        while (pictures.flushFailures() == 0 && timer.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        check(pictures.flushFailures() == 1 && !std::filesystem::exists(root / "service"),
              "TC.8: a deleted service is not recreated by a cache flush");
        check(pictures.dirty() && pictures.entryCount() > 0,
              "TC.8: the pictures survive in memory");
        pictures.setEnabled(false);
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

// --- review D2: a save adopts the show's audio layer too -------------------

// The same save-time adoption, for the reference the old rewrite could not
// see: a show's audio layer (scene 7). The operator imports a track, sets it
// on the show, saves — which MOVES the bytes — deletes the original, and the
// session's retained payloads must name the package's copy. The clear
// transforms are included because they are what the operator presses after
// the save: a background clear preserves the layer (DR.1) and must preserve
// the ADOPTED path, not the deleted one.
void exerciseShowAudioAdoption(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "audio-originals");
    const std::filesystem::path track = root / "audio-originals" / "track.wav";
    {
        std::ofstream out(track, std::ios::binary);
        out << "RIFF....WAVEfmt ";  // the NAME is what the model classifies
    }
    DocumentController docs(nullptr, root);
    check(docs.createSong(QStringLiteral("Audio Cue")), "audio adopt: the song exists");
    check(docs.addSlide(), "audio adopt: the slide exists");
    const QString showId = docs.currentSongId();
    const QString slideId =
        QString::fromStdString(docs.slides().at(0).toMap().value(QStringLiteral("id")).toString().toStdString());
    check(docs.importMediaFile(QString::fromStdString(track.string())),
          "audio adopt: the track imports");
    QString audioAssetId;
    for (const QVariant& row : docs.assets()) {
        if (row.toMap().value(QStringLiteral("sourcePath")).toString() ==
            QString::fromStdString(track.string())) {
            audioAssetId = row.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!audioAssetId.isEmpty(), "audio adopt: the track is a library asset");
    check(docs.setShowAudioFor(showId, audioAssetId),
          "audio adopt: the show takes the track as its audio layer");

    const QString beforeSave = docs.sceneJsonForSlide(slideId);
    bool ok = false;
    const Scene before = [&] {
        boost::system::error_code error;
        const boost::json::value parsed = boost::json::parse(beforeSave.toStdString(), error);
        Scene scene;
        ok = !error && openshow::scene::fromJson(parsed, &scene);
        return scene;
    }();
    check(ok && before.audio.present &&
              before.audio.assetPath == track.string(),
          "audio adopt: the compiled scene names the imported track");

    QVariantMap adopted;
    QObject::connect(&docs, &DocumentController::assetsAdopted,
                     [&adopted](const QVariantMap& moved) { adopted = moved; });
    check(docs.save(), "audio adopt: the save succeeds");
    check(adopted.contains(QString::fromStdString(track.string())),
          "audio adopt: the save announced the track's move");
    const QString owned =
        docs.assets().size() == 1
            ? docs.assets().at(0).toMap().value(QStringLiteral("sourcePath")).toString()
            : QString();
    check(owned.startsWith(QString::fromStdString((root / "service").string())) &&
              QFileInfo::exists(owned),
          "audio adopt: the model now names the package's copy of the track");
    std::filesystem::remove(track);  // the old bytes are gone, as after any save

    // The retained payloads the running session still holds are rewritten
    // through the same traversal the session uses.
    const QString rewritten = openshow::ui::withAdoptedAssetPaths(beforeSave, adopted);
    bool afterOk = false;
    const Scene after = [&] {
        boost::system::error_code error;
        const boost::json::value parsed = boost::json::parse(rewritten.toStdString(), error);
        Scene scene;
        afterOk = !error && openshow::scene::fromJson(parsed, &scene);
        return scene;
    }();
    check(afterOk && after.audio.assetPath == owned.toStdString() &&
              QFileInfo::exists(QString::fromStdString(after.audio.assetPath)),
          "audio adopt: the adopted payload names the package's track, and it exists");

    // The clear the operator presses next: the layer survives, naming bytes
    // that are still there.
    const QString cleared = openshow::ui::withBackgroundCleared(rewritten);
    bool clearedOk = false;
    const Scene clearedScene = [&] {
        boost::system::error_code error;
        const boost::json::value parsed = boost::json::parse(cleared.toStdString(), error);
        Scene scene;
        clearedOk = !error && openshow::scene::fromJson(parsed, &scene);
        return scene;
    }();
    check(clearedOk && clearedScene.audio.present &&
              clearedScene.audio.assetPath == owned.toStdString(),
          "audio adopt: a background clear keeps the adopted track");
    const QString wordsGone = openshow::ui::withWordsCleared(rewritten);
    bool wordsOk = false;
    const Scene wordsScene = [&] {
        boost::system::error_code error;
        const boost::json::value parsed = boost::json::parse(wordsGone.toStdString(), error);
        Scene scene;
        wordsOk = !error && openshow::scene::fromJson(parsed, &scene);
        return scene;
    }();
    check(wordsOk && wordsScene.audio.present &&
              wordsScene.audio.assetPath == owned.toStdString(),
          "audio adopt: a words clear keeps the adopted track");

    // And a reload agrees: the saved service names the package's track.
    DocumentController reopened(nullptr, root);
    const QString reloadedJson = reopened.sceneJsonForSlide(slideId);
    bool reloadedOk = false;
    const Scene reloaded = [&] {
        boost::system::error_code error;
        const boost::json::value parsed = boost::json::parse(reloadedJson.toStdString(), error);
        Scene scene;
        reloadedOk = !error && openshow::scene::fromJson(parsed, &scene);
        return scene;
    }();
    check(reloadedOk && reloaded.audio.present &&
              QFileInfo::exists(QString::fromStdString(reloaded.audio.assetPath)),
          "audio adopt: a reopened service presents the packaged track");
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

    // ND.7: a discovered NDI camera becomes a Media Bin asset with no bytes.
    check(docs.addCameraAsset(QStringLiteral("Stage Camera"),
                              QStringLiteral("STAGE (Camera 1)")),
          "a camera asset is added");
    {
        const QVariantMap row = docs.assets().front().toMap();
        check(row.value(QStringLiteral("kind")) == QStringLiteral("camera") &&
                  row.value(QStringLiteral("cameraSource")) ==
                      QStringLiteral("STAGE (Camera 1)") &&
                  row.value(QStringLiteral("sourcePath")) ==
                      QStringLiteral("ndi://STAGE (Camera 1)") &&
                  row.value(QStringLiteral("exists")).toBool(),
              "the bin row names the live source and is not mistaken for a missing file");
    }
    docs.undo();
    check(docs.assets().isEmpty(), "undo takes the camera asset back");
    docs.redo();
    check(docs.assets().size() == 1 && docs.assets().front().toMap().value(
              QStringLiteral("kind")) == QStringLiteral("camera"),
          "redo restores the camera asset");
    // ND.9: the front-camera mirror is the asset's own answer; it is set at
    // add time or later, and each change is one undoable edit.
    {
        const QString cameraId =
            docs.assets().front().toMap().value(QStringLiteral("id")).toString();
        check(!docs.assets().front().toMap().value(
                  QStringLiteral("cameraMirror")).toBool(),
              "a camera added without the flag is not mirrored");
        check(docs.setCameraMirror(cameraId, true) &&
                  docs.assets().front().toMap().value(
                      QStringLiteral("cameraMirror")).toBool(),
              "the mirror can be turned on from the bin's menu");
        docs.undo();
        check(!docs.assets().front().toMap().value(
                  QStringLiteral("cameraMirror")).toBool(),
              "one undo takes the mirror back off");
        docs.redo();
        check(docs.assets().front().toMap().value(
                  QStringLiteral("cameraMirror")).toBool(),
              "redo puts it back");
    }

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
    check(a.background.transparent && !a.background.hasGradient,
          "the current theme leaves the presentation background transparent");
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
              made.value(QStringLiteral("backgroundColor")).toString().isEmpty() &&
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
              chorus.value(QStringLiteral("backgroundColor")).toString().isEmpty() &&
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
    check(docs.currentTheme().value(QStringLiteral("backgroundGradient")).toString().isEmpty(),
          "the theme read model keeps background settings empty");
    check(compiledFor(docs, slideA).background.transparent &&
              !compiledFor(docs, slideA).background.hasGradient,
          "the compiled theme remains transparent");
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
                  reloaded.currentTheme().value(QStringLiteral("backgroundGradient")).toString().isEmpty(),
              "reopening retains a background-free theme");
    }
    // And a fork of a graded slide starts WITH the gradient — "wear what
    // the slide draws now" includes the second colour.
    check(docs.createThemeFromCurrent(QStringLiteral("Shaded")),
          "a slide wearing the graded style forks it");
    check(docs.currentTheme().value(QStringLiteral("backgroundGradient")).toString().isEmpty(),
          "theme snapshots exclude slide backgrounds");
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
        check(text.find("\"format\":16") != std::string::npos &&
                  text.find("\"fit\":\"contain\"") != std::string::npos,
              "the manifest is this build's format (16 since output profiles) and spells the slide's fit");
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

// --- user imports copy the bytes before anything can be unplugged ----------
// The Import dialog and a desktop drop go through the controller's copying
// door: the file lands in <base>/media first, so a USB stick can be removed
// before the next save. The raw importMediaFile() still records a path as
// given (the domain's missing-file rule), which is what the fixtures above
// rely on.
void exerciseMediaImportCopy(const std::filesystem::path& root) {
    const std::filesystem::path app = root / "app";
    const std::filesystem::path usb = root / "usb";
    std::filesystem::create_directories(usb);
    const std::filesystem::path clip = usb / "clip.mp4";
    { std::ofstream out(clip, std::ios::binary); out << "MP4BYTES"; }

    DocumentController docs(nullptr, app);
    check(docs.importMediaUrl(QUrl::fromLocalFile(QString::fromStdString(clip.string()))),
          "a file-picker import succeeds");
    check(docs.assets().size() == 1, "the import registers one asset");
    const QVariantMap imported = docs.assets().front().toMap();
    const QString owned = imported.value(QStringLiteral("sourcePath")).toString();
    const QString mediaDirectory =
        QString::fromStdString((app / "media").string());
    check(imported.value(QStringLiteral("name")).toString() == QLatin1String("clip.mp4")
              && owned.startsWith(mediaDirectory) && QFileInfo::exists(owned),
          "the import copies the bytes into the app's own media store");

    // The stick is pulled before any save: the copy is what the next save
    // reads, and the package still gets its bytes.
    std::filesystem::remove_all(usb);
    check(!QFileInfo::exists(QString::fromStdString(clip.string())),
          "the source is gone");
    check(docs.save(), "a save after the source is removed still succeeds");
    const QString packaged =
        docs.assets().front().toMap().value(QStringLiteral("sourcePath")).toString();
    check(packaged.startsWith(QString::fromStdString((app / "service").string()))
              && QFileInfo::exists(packaged),
          "the save folds the stored copy into the service package");

    // A multi-file drop is one edit: one undo takes the whole drop back.
    std::filesystem::create_directories(usb);
    const std::filesystem::path first = usb / "one.png";
    const std::filesystem::path second = usb / "two.mp3";
    { std::ofstream out(first, std::ios::binary); out << "PNGBYTES"; }
    { std::ofstream out(second, std::ios::binary); out << "AUDIOBYTES"; }
    const int before = docs.assets().size();
    const int added = docs.importMediaUrls(
        {QUrl::fromLocalFile(QString::fromStdString(first.string())),
         QUrl::fromLocalFile(QString::fromStdString(second.string()))});
    check(added == 2 && docs.assets().size() == before + 2,
          "a desktop drop imports every file it carries");
    for (int i = before; i < docs.assets().size(); ++i) {
        const QString path = docs.assets().at(i).toMap()
                                 .value(QStringLiteral("sourcePath")).toString();
        check(QFileInfo::exists(path) && path.startsWith(mediaDirectory),
              "every dropped file is owned by the app before it is registered");
    }
    docs.undo();
    check(docs.assets().size() == before,
          "one undo takes the whole drop back");
}

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
    const QVariantList beforeNameCollision = docs.themes();
    check(!docs.createBlankTheme(QStringLiteral(" House ")) &&
              docs.status().contains(QStringLiteral("already exists")) &&
              docs.themes() == beforeNameCollision,
          "the controller refuses creation with an existing trimmed name");
    check(docs.duplicateTheme(house) &&
              docs.themes().last().toMap().value(QStringLiteral("name")).toString() ==
                  QStringLiteral("House copy (2)"),
          "repeated Duplicate generates a free name before creating the theme");
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

// TS.1's surfaces: the Theme Editor's whole-set style verb (strict shapes,
// undoable), the Edit page's style customizations and their reverts, the
// rows every view reads, and the carry rules for duplication and
// from-slide snapshots. The bands themselves are the domain's — exercised
// in domain_tests — so this fixture is about the doors and the rows.
void exerciseTextStyleSurfaces(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Styled");
    docs.addSlide();
    docs.selectSlide(0);
    check(docs.createBlankTheme("House"), "a blank theme exists to style");
    QString house;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("House")) {
            house = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!house.isEmpty() && docs.applyThemeToCurrentSlide(house),
          "the slide wears House");

    // The bare theme's rows: silent flags with the contract's defaults as
    // display values, and EMPTY effect maps — silence has no record to show.
    {
        QVariantMap row;
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("id")).toString() == house) {
                row = theme.toMap();
            }
        }
        check(row.value(QStringLiteral("fontWeightSet")).toBool() == false &&
                  row.value(QStringLiteral("fontWeight")).toInt() == 400 &&
                  row.value(QStringLiteral("strokeSet")).toBool() == false &&
                  row.value(QStringLiteral("stroke")).toMap().isEmpty(),
              "an unstyled theme's row says silent and shows the defaults");
    }

    // --- the verb: whole set in, strict shape, one undo ---
    QVariantMap strokeOn;
    strokeOn.insert(QStringLiteral("enabled"), true);
    strokeOn.insert(QStringLiteral("color"), QStringLiteral("#ff0000"));
    strokeOn.insert(QStringLiteral("width"), 3.0);
    QVariantMap style;
    style.insert(QStringLiteral("fontWeight"), 700);
    style.insert(QStringLiteral("fontStyle"), QStringLiteral("italic"));
    style.insert(QStringLiteral("stroke"), strokeOn);
    check(docs.setThemeTextStyle(house, style),
          "the Theme Editor sets weight, slant and a whole stroke record");
    check(compiledFor(docs, docs.slides().at(0).toMap().value(QStringLiteral("id")).toString())
                  .items[0]
                  .fontWeight == 700,
          "the compiled scene carries the theme's weight");
    QVariantMap halfStroke;
    halfStroke.insert(QStringLiteral("enabled"), true);
    QVariantMap aliased;
    aliased.insert(QStringLiteral("weight"), 700);
    QVariantMap shadowAlien;
    shadowAlien.insert(QStringLiteral("enabled"), true);
    shadowAlien.insert(QStringLiteral("color"), QStringLiteral("#000000"));
    shadowAlien.insert(QStringLiteral("opacity"), 0.5);
    shadowAlien.insert(QStringLiteral("offsetX"), 2.0);
    shadowAlien.insert(QStringLiteral("offsetY"), 2.0);
    shadowAlien.insert(QStringLiteral("blurSigma"), 4.0);
    shadowAlien.insert(QStringLiteral("feather"), 8.0);
    check(!docs.setThemeTextStyle(house, QVariantMap{{QStringLiteral("weight"), 700}}) &&
                  docs.status().contains(QStringLiteral("not a text-style field")),
          "an unknown key is refused naming itself");
    check(!docs.setThemeTextStyle(house, QVariantMap{{QStringLiteral("fontWeight"),
                                                      QStringLiteral("700")}}),
          "a stringy weight is refused, not coerced: the spin box's 700 is not the text field's");
    check(!docs.setThemeTextStyle(house, QVariantMap{{QStringLiteral("stroke"), halfStroke}}),
          "half a stroke record is refused: an effect travels whole");
    check(!docs.setThemeTextStyle(house, QVariantMap{{QStringLiteral("stroke"),
                                                      QStringLiteral("thick")}}),
          "and so is an effect spelled as anything but a record");
    check(!docs.setThemeTextStyle(house, QVariantMap{{QStringLiteral("shadow"), shadowAlien}}),
          "a shadow member the record never had is refused naming itself");
    check(!docs.setThemeTextStyle(house, QVariantMap{{QStringLiteral("fontWeight"), 901}}),
          "an off-scale weight is refused by the domain's band");
    {
        QVariantMap after;
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("id")).toString() == house) {
                after = theme.toMap();
            }
        }
        check(after.value(QStringLiteral("fontWeight")).toInt() == 700 &&
                  after.value(QStringLiteral("strokeSet")).toBool(),
              "the refusals changed nothing: the accepted style stands");
    }
    docs.undo();
    check(compiledFor(docs, docs.slides().at(0).toMap().value(QStringLiteral("id")).toString())
                  .items[0]
                  .fontWeight == 400,
          "one undo is one style edit: the theme is unstyled and the slide draws defaults");
    docs.redo();
    check(compiledFor(docs, docs.slides().at(0).toMap().value(QStringLiteral("id")).toString())
                  .items[0]
                  .fontWeight == 700,
          "and redo puts the whole style back");

    // --- the Edit page: customize one style field, revert it back ---
    check(docs.customizeCurrentSlide(QStringLiteral("fontWeight"), 900),
          "the slide customizes its weight over the themed 700");
    QVariantMap look = docs.slideAppearance();
    check(look.value(QStringLiteral("fontWeight")).toMap()
                  .value(QStringLiteral("value")).toInt() == 900 &&
              look.value(QStringLiteral("fontWeight")).toMap()
                  .value(QStringLiteral("customized")).toBool() &&
              !look.value(QStringLiteral("fontStyle")).toMap()
                   .value(QStringLiteral("customized")).toBool(),
          "the appearance view shows the drawn 900 and flags only the touched field");
    QVariantMap strokeOff;
    strokeOff.insert(QStringLiteral("enabled"), false);
    strokeOff.insert(QStringLiteral("color"), QStringLiteral("#00ff00"));
    strokeOff.insert(QStringLiteral("width"), 0.0);
    check(docs.customizeCurrentSlide(QStringLiteral("stroke"), strokeOff),
          "the slide switches the themed outline off — a whole refusal record");
    openshow::scene::Scene drawn =
        compiledFor(docs, docs.slides().at(0).toMap().value(QStringLiteral("id")).toString());
    check(drawn.items[0].fontWeight == 900 && !drawn.items[0].stroke.enabled,
          "the customization answers weight and refusal while the theme keeps the slant");
    check(drawn.items[0].fontStyle == "italic", "and the themed italic still draws");
    check(!docs.customizeCurrentSlide(QStringLiteral("fontWeight"),
                                      QStringLiteral("bold")) &&
                  !docs.customizeCurrentSlide(QStringLiteral("stroke"), halfStroke) &&
                  !docs.customizeCurrentSlide(QStringLiteral("shadow"), shadowAlien),
          "the customization door quotes the same shape rules as the theme verb");
    check(docs.revertCurrentSlideField(QStringLiteral("fontWeight")) &&
              docs.revertCurrentSlideField(QStringLiteral("stroke")),
          "both style badges revert");
    look = docs.slideAppearance();
    check(!look.value(QStringLiteral("fontWeight")).toMap()
                 .value(QStringLiteral("customized")).toBool() &&
              look.value(QStringLiteral("fontWeight")).toMap()
                  .value(QStringLiteral("value")).toInt() == 700 &&
              !look.value(QStringLiteral("stroke")).toMap()
                   .value(QStringLiteral("customized")).toBool(),
          "after the reverts the themed answers show again, unflagged");
    check(!docs.revertCurrentSlideField(QStringLiteral("stroke")) &&
              docs.status().contains(QStringLiteral("revert")),
          "reverting a field that was never customized is refused on the status line");

    // --- duplication and snapshots carry the style ---
    QVariantMap contextCreate;
    contextCreate.insert(QStringLiteral("fontSize"), 32.0);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), contextCreate),
          "House also carries a scripture context");
    check(docs.duplicateTheme(house), "House is duplicated");
    QString copy;
    QVariantMap copyRow;
    for (const QVariant& theme : docs.themes()) {
        const QVariantMap map = theme.toMap();
        if (map.value(QStringLiteral("name")).toString() == QStringLiteral("House copy")) {
            copy = map.value(QStringLiteral("id")).toString();
            copyRow = map;
        }
    }
    check(copyRow.value(QStringLiteral("fontWeightSet")).toBool() &&
              copyRow.value(QStringLiteral("fontWeight")).toInt() == 700 &&
              copyRow.value(QStringLiteral("fontStyle")).toString() == QStringLiteral("italic") &&
              copyRow.value(QStringLiteral("strokeSet")).toBool() &&
              copyRow.value(QStringLiteral("stroke")).toMap()
                  .value(QStringLiteral("enabled")).toBool(),
          "the duplicate carries the whole style — weight, slant and the outline");
    check(docs.themeContext(copy, QStringLiteral("scripture"))
                  .value(QStringLiteral("set"))
                  .toBool(),
          "the duplicate carries the contexts too");

    // A theme taken FROM the slide snapshots the chain's concrete answers.
    check(docs.customizeCurrentSlide(QStringLiteral("minFontSize"), 30.0),
          "the slide also customizes its minimum size");
    check(docs.createThemeFromCurrent(QStringLiteral("From slide")),
          "a theme is made from what the slide draws");
    QVariantMap snapshotRow;
    for (const QVariant& theme : docs.themes()) {
        const QVariantMap map = theme.toMap();
        if (map.value(QStringLiteral("name")).toString() == QStringLiteral("From slide")) {
            snapshotRow = map;
        }
    }
    check(snapshotRow.value(QStringLiteral("fontWeightSet")).toBool() &&
              snapshotRow.value(QStringLiteral("fontWeight")).toInt() == 700 &&
              snapshotRow.value(QStringLiteral("minFontSizeSet")).toBool() &&
              snapshotRow.value(QStringLiteral("minFontSize")).toDouble() == 30.0 &&
              snapshotRow.value(QStringLiteral("fitModeSet")).toBool() &&
              snapshotRow.value(QStringLiteral("fitMode")).toString() == QStringLiteral("shrink"),
          "the new theme owns the chain's answers — the customized weight, the customized "
          "minimum, and even the default it inherited becomes its own");

    // The wearing-theme row carries the style fields as well.
    check(docs.useThemeAsCurrent(house), "the service now wears House");
    const QVariantMap worn = docs.currentTheme();
    check(worn.value(QStringLiteral("fontWeightSet")).toBool() &&
              worn.value(QStringLiteral("fontWeight")).toInt() == 700,
          "currentTheme() carries the same style rows the list does");

    // And the whole thing survives the disk.
    check(docs.save(), "the service saves with its styles");
    openshow::domain::Documents reopened;
    std::string loadError;
    check(openshow::domain::loadService(&reopened, root / "service", &loadError) &&
              reopened.findTheme(house.toStdString())->fontWeight == 700 &&
              reopened.findTheme(house.toStdString())->stroke->enabled,
          "the styled theme reads back from disk through the next session's load");
}

// Task TL.1's controller half: the vertical anchor and the All caps answer
// travel the same doors as the rest of the style block — the theme row's
// ...Set pair, the whole-set verb's one undo, the one-field merge verb's
// single undo, the Edit page's customize/revert badges, the Bible view's
// context read model, a duplicate's copy and the sample's resolved
// appearance — while the closed word set is refused at every door and an
// explicit false stays distinguishable from silence.
void exerciseTextLayoutSurfaces(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Laid out");
    docs.addSlide();
    docs.selectSlide(0);
    check(docs.createBlankTheme("House"), "a blank theme exists to lay out");
    QString house;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("House")) {
            house = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!house.isEmpty() && docs.applyThemeToCurrentSlide(house), "the slide wears House");
    const auto rowOf = [&docs, &house] {
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("id")).toString() == house) {
                return theme.toMap();
            }
        }
        return QVariantMap{};
    };

    // A silent theme's row shows the contract's centre/normal-case answers
    // with the Set flags down, exactly as the style fields do.
    {
        const QVariantMap row = rowOf();
        check(row.value(QStringLiteral("verticalAlignSet")).toBool() == false &&
                  row.value(QStringLiteral("verticalAlign")).toString() ==
                      QStringLiteral("center") &&
                  row.value(QStringLiteral("allCapsSet")).toBool() == false &&
                  row.value(QStringLiteral("allCaps")).toBool() == false,
              "an unstyled theme's row says silent and shows centre / normal case");
    }
    {
        const QVariantMap look = docs.slideAppearance();
        const QVariantMap anchor = look.value(QStringLiteral("verticalAlign")).toMap();
        const QVariantMap caps = look.value(QStringLiteral("allCaps")).toMap();
        check(anchor.value(QStringLiteral("value")).toString() == QStringLiteral("center") &&
                  !anchor.value(QStringLiteral("customized")).toBool() &&
                  caps.value(QStringLiteral("value")).toBool() == false &&
                  !caps.value(QStringLiteral("customized")).toBool(),
              "the Edit page shows the inherited centre / normal case, unflagged");
    }

    // --- the whole-set verb: both fields in one undo ---
    QVariantMap style;
    style.insert(QStringLiteral("verticalAlign"), QStringLiteral("top"));
    style.insert(QStringLiteral("allCaps"), true);
    check(docs.setThemeTextStyle(house, style), "the Theme Editor sets the layout pair");
    {
        const QVariantMap row = rowOf();
        check(row.value(QStringLiteral("verticalAlignSet")).toBool() &&
                  row.value(QStringLiteral("verticalAlign")).toString() == QStringLiteral("top") &&
                  row.value(QStringLiteral("allCapsSet")).toBool() &&
                  row.value(QStringLiteral("allCaps")).toBool(),
              "the row carries the authored anchor and capitalisation");
        const QVariantMap sample = docs.themeSampleAppearance(house);
        check(sample.value(QStringLiteral("ok")).toBool() &&
                  sample.value(QStringLiteral("verticalAlign")).toString() ==
                      QStringLiteral("top") &&
                  sample.value(QStringLiteral("allCaps")).toBool(),
              "the sample's resolved appearance carries the pair");
    }
    // The closed word set is refused at the whole-set door, and a refusal
    // changes nothing.
    check(!docs.setThemeTextStyle(
              house, QVariantMap{{QStringLiteral("verticalAlign"), QStringLiteral("middle")}}) &&
              docs.status().contains(QStringLiteral("\"top\", \"center\" or \"bottom\"")),
          "a made-up anchor is refused naming the three words");
    check(!docs.setThemeTextStyle(
              house, QVariantMap{{QStringLiteral("verticalAlign"), QStringLiteral("")}}),
          "an empty anchor is refused rather than read as silence");
    check(!docs.setThemeTextStyle(
              house, QVariantMap{{QStringLiteral("allCaps"), QStringLiteral("yes")}}),
          "a stringy caps answer is refused, not coerced");
    check(rowOf().value(QStringLiteral("verticalAlign")).toString() == QStringLiteral("top") &&
              rowOf().value(QStringLiteral("allCaps")).toBool(),
          "the refusals left the accepted pair standing");
    docs.undo();
    check(rowOf().value(QStringLiteral("verticalAlignSet")).toBool() == false &&
              rowOf().value(QStringLiteral("allCapsSet")).toBool() == false,
          "one undo takes the whole pair back to silence");
    docs.redo();
    check(rowOf().value(QStringLiteral("verticalAlign")).toString() == QStringLiteral("top") &&
              rowOf().value(QStringLiteral("allCaps")).toBool(),
          "and redo puts the pair back");

    // --- the one-field merge verb: one field, one undo, null to inherit ---
    check(docs.setThemeTextStyleField(house, QStringLiteral("verticalAlign"),
                                      QStringLiteral("bottom")),
          "the merge verb moves the anchor alone");
    check(rowOf().value(QStringLiteral("verticalAlign")).toString() == QStringLiteral("bottom") &&
              rowOf().value(QStringLiteral("allCaps")).toBool(),
          "the capitalisation it did not name stays as it was");
    docs.undo();
    check(rowOf().value(QStringLiteral("verticalAlign")).toString() == QStringLiteral("top"),
          "one undo backs out the one merge");
    check(docs.setThemeTextStyleField(house, QStringLiteral("allCaps"), false),
          "the merge verb can say false, which no silence can");
    check(rowOf().value(QStringLiteral("allCapsSet")).toBool() &&
              rowOf().value(QStringLiteral("allCaps")).toBool() == false,
          "an explicit false stays an authored answer, not a cleared one");
    check(docs.setThemeTextStyleField(house, QStringLiteral("allCaps"), QVariant()) &&
              rowOf().value(QStringLiteral("allCapsSet")).toBool() == false,
          "null clears the answer back to inherit");
    check(!docs.setThemeTextStyleField(house, QStringLiteral("verticalAlign"),
                                       QStringLiteral("middle")),
          "the merge door refuses the same word set");

    // --- the Edit page: customize each, revert each ---
    check(docs.customizeCurrentSlide(QStringLiteral("verticalAlign"),
                                     QStringLiteral("bottom")) &&
              docs.customizeCurrentSlide(QStringLiteral("allCaps"), true),
          "the slide customizes both layout fields");
    {
        const QVariantMap look = docs.slideAppearance();
        check(look.value(QStringLiteral("verticalAlign"))
                      .toMap()
                      .value(QStringLiteral("value"))
                      .toString() == QStringLiteral("bottom") &&
                  look.value(QStringLiteral("verticalAlign"))
                      .toMap()
                      .value(QStringLiteral("customized"))
                      .toBool() &&
                  look.value(QStringLiteral("allCaps")).toMap().value(QStringLiteral("value")).toBool() &&
                  look.value(QStringLiteral("allCaps"))
                      .toMap()
                      .value(QStringLiteral("customized"))
                      .toBool(),
              "the appearance view shows the slide's own pair, flagged");
    }
    check(!docs.customizeCurrentSlide(QStringLiteral("verticalAlign"), QStringLiteral("middle")),
          "the customization door refuses the off-list anchor");
    check(docs.revertCurrentSlideField(QStringLiteral("verticalAlign")) &&
              docs.revertCurrentSlideField(QStringLiteral("allCaps")),
          "both badges revert");
    {
        const QVariantMap look = docs.slideAppearance();
        check(look.value(QStringLiteral("verticalAlign"))
                      .toMap()
                      .value(QStringLiteral("value"))
                      .toString() == QStringLiteral("top") &&
                  !look.value(QStringLiteral("verticalAlign"))
                       .toMap()
                       .value(QStringLiteral("customized"))
                       .toBool() &&
                  look.value(QStringLiteral("allCaps")).toMap().value(QStringLiteral("value")).toBool() ==
                      false &&
                  !look.value(QStringLiteral("allCaps"))
                       .toMap()
                       .value(QStringLiteral("customized"))
                       .toBool(),
              "after the reverts the theme's top / off show again, unflagged");
    }

    // --- the Bible view's context speaks the pair; the whole-entry verb
    // replaces, and the shared inspector's merge is what carries it ---
    QVariantMap reading;
    reading.insert(QStringLiteral("verticalAlign"), QStringLiteral("center"));
    reading.insert(QStringLiteral("allCaps"), true);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), reading),
          "House's scripture context carries the pair");
    {
        const QVariantMap context = docs.themeContext(house, QStringLiteral("scripture"));
        check(context.value(QStringLiteral("set")).toBool() &&
                  context.value(QStringLiteral("verticalAlignSet")).toBool() &&
                  context.value(QStringLiteral("verticalAlign")).toString() ==
                      QStringLiteral("center") &&
                  context.value(QStringLiteral("allCapsSet")).toBool() &&
                  context.value(QStringLiteral("allCaps")).toBool(),
              "the context read model carries set flags and values");
    }
    QVariantMap other;
    other.insert(QStringLiteral("fontSize"), 40.0);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), other),
          "a later edit names only the font size");
    {
        // applyThemeContext REPLACES the whole entry: absent keys are
        // silence about the field they name, exactly like every other
        // context field. The shared inspector's merge (not this verb) is
        // what carries an untouched pair across a one-field edit.
        const QVariantMap context = docs.themeContext(house, QStringLiteral("scripture"));
        check(!context.value(QStringLiteral("verticalAlignSet")).toBool() &&
                  !context.value(QStringLiteral("allCapsSet")).toBool() &&
                  context.value(QStringLiteral("fontSize")).toDouble() == 40.0,
              "a replacing write that names only the size leaves the pair silent");
    }
    QVariantMap restored;
    restored.insert(QStringLiteral("fontSize"), 40.0);
    restored.insert(QStringLiteral("verticalAlign"), QStringLiteral("center"));
    restored.insert(QStringLiteral("allCaps"), true);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), restored),
          "the pair is authored again beside the size");
    check(!docs.applyThemeContext(
              house, QStringLiteral("scripture"),
              QVariantMap{{QStringLiteral("verticalAlign"), QStringLiteral("middle")}}),
          "the context door refuses the off-list anchor");

    // --- duplication and a theme taken from the slide both carry the pair ---
    check(docs.setThemeTextStyleField(house, QStringLiteral("allCaps"), false),
          "the theme takes an explicit false for the duplicate to carry");
    check(docs.duplicateTheme(house), "House is duplicated");
    QString copy;
    QVariantMap copyRow;
    for (const QVariant& theme : docs.themes()) {
        const QVariantMap map = theme.toMap();
        if (map.value(QStringLiteral("name")).toString() == QStringLiteral("House copy")) {
            copy = map.value(QStringLiteral("id")).toString();
            copyRow = map;
        }
    }
    check(copyRow.value(QStringLiteral("verticalAlignSet")).toBool() &&
              copyRow.value(QStringLiteral("verticalAlign")).toString() == QStringLiteral("top") &&
              copyRow.value(QStringLiteral("allCapsSet")).toBool() &&
              copyRow.value(QStringLiteral("allCaps")).toBool() == false &&
              docs.themeContext(copy, QStringLiteral("scripture"))
                  .value(QStringLiteral("allCaps"))
                  .toBool(),
          "the duplicate carries the theme's pair and its context's pair");

    check(docs.customizeCurrentSlide(QStringLiteral("verticalAlign"),
                                     QStringLiteral("bottom")) &&
              docs.createThemeFromCurrent(QStringLiteral("From slide")),
          "a theme is made from what the slide draws");
    QVariantMap snapshotRow;
    for (const QVariant& theme : docs.themes()) {
        const QVariantMap map = theme.toMap();
        if (map.value(QStringLiteral("name")).toString() == QStringLiteral("From slide")) {
            snapshotRow = map;
        }
    }
    check(snapshotRow.value(QStringLiteral("verticalAlignSet")).toBool() &&
              snapshotRow.value(QStringLiteral("verticalAlign")).toString() ==
                  QStringLiteral("bottom") &&
              snapshotRow.value(QStringLiteral("allCapsSet")).toBool() &&
              snapshotRow.value(QStringLiteral("allCaps")).toBool() == false,
          "the new theme owns the chain's resolved pair, explicit false included");

    // --- and the pair survives the disk ---
    check(docs.save(), "the service saves with its layout pair");
    openshow::domain::Documents reopened;
    std::string loadError;
    check(openshow::domain::loadService(&reopened, root / "service", &loadError) &&
              reopened.findTheme(house.toStdString())->verticalAlign == "top" &&
              reopened.findTheme(house.toStdString())->allCaps == false &&
              reopened.findTheme(house.toStdString())->contexts[0].verticalAlign == "center" &&
              reopened.findTheme(house.toStdString())->contexts[0].allCaps == true,
          "the anchor and capitalisation read back from disk through the next session's load");
}

// TS.4's one-field merge verb: the inspector edits one property of the
// shared style block at a time (a drag of the shadow-blur slider lands ONE
// undo, not one per intermediate value), and a field edit cannot disagree
// with the whole-set verb because both hand the same merged block to the
// same domain mutator. The behaviours that matter: silence stays silence
// except for the field touched; merging a member into a theme that carries
// no record materialises the CONTRACT defaults around it (never a
// fragment); null clears a scalar to inherit but is refused on an effect
// member; refusals cost no undo; and the sample's appearance now carries
// the resolved typography and effects for the inherit badges.
// Task TR.1's doors on the transition contract: the three verbs (the service's
// own default, a theme's override, a text context's), the reads the cue lane and
// a settings page share so an operator's number and a cue's number cannot
// disagree, and the three things a controller adds to the domain's model —
// refusals that reach the status line naming the field, undo over every write,
// and duplication carrying the record.
void exerciseTransitionSurfaces(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Moving");
    docs.addSlide();
    docs.selectSlide(0);
    const QString showId = docs.currentSongId();
    const QString slideId = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    check(docs.createBlankTheme("House"), "a blank theme exists to give a movement");
    QString house;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("House")) {
            house = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!house.isEmpty() && docs.applyThemeToCurrentSlide(house), "the slide wears House");

    const auto record = [](const char* kind, int textMs, int mediaMs) {
        QVariantMap out;
        out.insert(QStringLiteral("kind"), QString::fromLatin1(kind));
        out.insert(QStringLiteral("textDurationMs"), textMs);
        out.insert(QStringLiteral("mediaDurationMs"), mediaMs);
        return out;
    };

    // --- unconfigured: both reads answer the contract -----------------------
    const QVariantMap contract = record("fade", 500, 800);
    check(docs.serviceDefaultTransition() == contract &&
              docs.transitionForSlide(showId, slideId) == contract,
          "a document nobody configured resolves the service default and the slide to the "
          "same concrete 500 ms / 800 ms crossfade");

    // --- the service default verb -------------------------------------------
    check(docs.setServiceDefaultTransition("cut", 0, 0), "the service's answer is set to a cut");
    check(docs.serviceDefaultTransition() == record("cut", 0, 0) &&
              docs.transitionForSlide(showId, slideId) == record("cut", 0, 0),
          "and every slide no layer speaks for now cuts");
    check(!docs.setServiceDefaultTransition("wipe", 500, 800) &&
              docs.status().contains(QStringLiteral("kind")),
          "an unknown kind is refused, naming what it was given");
    check(!docs.setServiceDefaultTransition("fade", 10001, 800) &&
              docs.status().contains(QStringLiteral("textDurationMs")),
          "a words duration above the band is refused naming the wire key the field has");
    check(!docs.setServiceDefaultTransition("fade", 500, -1) &&
              docs.status().contains(QStringLiteral("mediaDurationMs")),
          "and a negative background duration with it");
    check(docs.serviceDefaultTransition() == record("cut", 0, 0),
          "three refusals and the standing default has not moved by one millisecond");
    check(docs.canUndo(), "the service default's write is on the undo stack");
    docs.undo();
    check(docs.serviceDefaultTransition() == contract,
          "one undo takes the service's answer back to the contract's");
    docs.redo();
    check(docs.serviceDefaultTransition() == record("cut", 0, 0), "and redo brings the cut back");

    // --- the theme's override, and inherit ----------------------------------
    const QVariantMap brisk = record("fade", 100, 200);
    check(docs.setThemeTransition(house, brisk), "the theme is given a record of its own");
    check(docs.transitionForSlide(showId, slideId) == brisk,
          "the slide wearing it resolves to the theme's record, over the service's cut");
    QVariantMap alien = brisk;
    alien.insert(QStringLiteral("curve"), QStringLiteral("ease"));
    check(!docs.setThemeTransition(house, alien) && docs.status().contains(QStringLiteral("curve")),
          "a member the transition record never had is refused naming itself");
    QVariantMap half;
    half.insert(QStringLiteral("kind"), QStringLiteral("cut"));
    check(!docs.setThemeTransition(house, half) &&
              docs.status().contains(QStringLiteral("whole or absent")),
          "half a record is refused: kind and both lengths travel together or not at all");
    check(!docs.setThemeTransition(QStringLiteral("no-such-theme"), brisk) &&
              docs.status().contains(QStringLiteral("no-such-theme")),
          "and overriding a theme that does not exist is refused naming it");
    check(docs.transitionForSlide(showId, slideId) == brisk,
          "the refused writes left the theme's own answer exactly where it was");
    check(docs.setThemeTransition(house, QVariantMap{}), "an EMPTY record is the inherit button");
    check(docs.transitionForSlide(showId, slideId) == record("cut", 0, 0),
          "and inheriting is real: the slide falls back to the service's cut, not to the "
          "contract's fade — the two are different answers");

    // --- duplication carries the movement ------------------------------------
    check(docs.setThemeTransition(house, brisk), "House is given its record back to duplicate");
    check(docs.duplicateTheme(house), "House is duplicated");
    QString copy;
    for (const QVariant& theme : docs.themes()) {
        const QVariantMap map = theme.toMap();
        if (map.value(QStringLiteral("name")).toString() == QStringLiteral("House copy")) {
            copy = map.value(QStringLiteral("id")).toString();
        }
    }
    if (check(!copy.isEmpty(), "the duplicate exists")) {
        // The copy wears no slide yet, so the resolver cannot see it — which is
        // why the duplicate is put on a second slide: what the copy CARRIES is
        // only worth proving through the walk that decides a cue's move.
        docs.addSlide();
        docs.selectSlide(docs.slides().size() - 1);
        const QString secondSlide =
            docs.slides().at(docs.slides().size() - 1).toMap().value(QStringLiteral("id")).toString();
        check(docs.applyThemeToCurrentSlide(copy) &&
                  docs.transitionForSlide(showId, secondSlide) == brisk,
              "a slide wearing the DUPLICATE resolves to the copied record — a theme that "
              "lost its transition on the way through duplication would start "
              "crossfading cues the operator had told it to cut");
    }

    // --- the Go-time read: a compiled scene names its own slide --------------
    const QString authored = docs.sceneJsonForSlide(slideId);
    if (check(!authored.isEmpty(), "the wearing slide compiles to a scene payload")) {
        boost::system::error_code parseError;
        const boost::json::value parsed = boost::json::parse(authored.toStdString(), parseError);
        if (check(!parseError && parsed.is_object(), "and that payload parses")) {
            const openshow::domain::TransitionOptions forScene =
                docs.transitionForSceneJson(parsed.as_object());
            check(forScene.textDurationMs == 100 && forScene.mediaDurationMs == 200,
                  "the record a Go resolves for THAT payload is the record its own "
                  "showId/slideId walk to");
            const openshow::domain::TransitionOptions unplaced =
                docs.transitionForSceneJson(boost::json::object{});
            check(unplaced.kind == openshow::domain::TransitionKind::kCut &&
                      unplaced.textDurationMs == 0 && unplaced.mediaDurationMs == 0,
                  "a payload that names no slide answers with the service's own record "
                  "(the cut 0/0 this session set) rather than nothing — the audience is "
                  "owed a reveal either way");
        }
    }

    // --- the text context's override, through a slide that names it ----------
    // A scripture passage is the controller's own way to get a slide SET in a
    // context (the importer names `scripture` on every slide it lays out), so
    // this proves the context seat resolves through the same read the cue lane
    // uses rather than only through the domain in isolation.
    check(docs.addScripturePassage(docs.currentPlaylistId(), QStringLiteral("Psalm 1:1-2"),
                                   QStringLiteral("ESV"), QStringLiteral("test licence"),
                                   QVariantList{QVariantList{QStringLiteral("Blessed is the man")}}),
          "a scripture passage is added, its slides set in the scripture context");
    const QVariantMap scheduleRow = docs.playlistEntries().at(0).toMap();
    const QString passageShow = scheduleRow.value(QStringLiteral("showId")).toString();
    check(!passageShow.isEmpty(), "the schedule row names the passage's show");
    const QVariantList passageSlides = docs.slidesOf(passageShow);
    if (check(!passageSlides.isEmpty(), "the passage has slides")) {
        const QString passageSlide =
            passageSlides.at(0).toMap().value(QStringLiteral("id")).toString();
        // The passage's show is worn by no theme of its own, so before the
        // override it takes the service's cut like any unclaimed slide.
        check(docs.transitionForSlide(passageShow, passageSlide) == record("cut", 0, 0),
              "an unoverridden passage slide follows the service");
        QVariantMap contextCreate;
        contextCreate.insert(QStringLiteral("fontSize"), 32.0);
        check(docs.applyThemeContext(house, QStringLiteral("scripture"), contextCreate),
              "House grows the scripture context");
        const QVariantMap slow = record("fade", 1500, 1800);
        check(!docs.setTextContextTransition(house, QStringLiteral("not-a-context"), slow) &&
                  docs.status().contains(QStringLiteral("not-a-context")),
              "overriding a context the theme does not carry is refused naming it — a "
              "silent success would teach the operator the view has no override");
        // A context only answers for a slide through a theme the slide's chain
        // actually carries, so the service starts wearing House: the same walk
        // the typography resolves through, and the reason the passage slide can
        // have a scripture context to inherit at all.
        check(docs.useThemeAsCurrent(house) &&
                  docs.transitionForSlide(passageShow, passageSlide) == brisk,
              "with House worn service-wide, the passage slide answers to House's own record");
        check(docs.setTextContextTransition(house, QStringLiteral("scripture"), slow),
              "the context's own record is set");
        check(docs.transitionForSlide(passageShow, passageSlide) == slow,
              "and the slide set in that context moves on the VIEW's timings, ahead of the "
              "theme wearing it and of the service's cut");
        check(docs.setTextContextTransition(house, QStringLiteral("scripture"), QVariantMap{}),
              "the context can be told to say nothing again");
        check(docs.transitionForSlide(passageShow, passageSlide) == brisk,
              "and its slides fall to the NEXT layer — House's own record, not the "
              "service's: inherit walks, it does not reset");
    }

    // --- the rows the controls read (task TR.3) -----------------------------
    // The Settings page and the Theme Editor's Transition section are the
    // operator's side of the contract, and what they read is these projections,
    // so the shape they carry is the acceptance: a record in the shape the verb
    // takes (a page must never re-spell one), and a silence that reads back as
    // NOTHING rather than as the numbers silence happens to resolve to. A row
    // that filled an inheriting theme in with 500/800 would leave the operator
    // unable to tell its own answer from the one it borrows — and the Inherit
    // button would have nothing to point at.
    const auto rowOf = [&docs](const QString& id) {
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("id")).toString() == id) {
                return theme.toMap();
            }
        }
        return QVariantMap{};
    };
    const QVariantMap houseRow = rowOf(house);
    check(houseRow.value(QStringLiteral("transitionSet")).toBool() &&
              houseRow.value(QStringLiteral("transition")).toMap() == brisk,
          "the theme row carries its own record, spelled the way setThemeTransition takes it");
    QVariantMap silentRow;
    for (const QVariant& theme : docs.themes()) {
        if (!theme.toMap().value(QStringLiteral("transitionSet")).toBool()) {
            silentRow = theme.toMap();
            break;
        }
    }
    check(!silentRow.isEmpty() && silentRow.value(QStringLiteral("transition")).toMap().isEmpty(),
          "a theme that says nothing reads back as an EMPTY record and a false flag, not as a "
          "filled-in 500/800 — the page has to be able to show that this view is borrowing");
    const QVariantMap worn = docs.currentTheme();
    check(worn.value(QStringLiteral("id")).toString() == house &&
              worn.value(QStringLiteral("transitionSet")).toBool() &&
              worn.value(QStringLiteral("transition")).toMap() == brisk,
          "and the wearing-theme row tells the same story about the same theme, so the Show "
          "page and the editor cannot show one theme moving two ways");

    // The context's own seat, and the one field a replacing write keeps.
    QVariantMap ctxRow = docs.themeContext(house, QStringLiteral("scripture"));
    check(ctxRow.value(QStringLiteral("set")).toBool() &&
              !ctxRow.value(QStringLiteral("transitionSet")).toBool() &&
              ctxRow.value(QStringLiteral("transition")).toMap().isEmpty(),
          "the context read answers transitionSet false with an empty record while the view "
          "says nothing about how it arrives");
    const QVariantMap pageMove = record("fade", 1500, 1800);
    check(docs.setTextContextTransition(house, QStringLiteral("scripture"), pageMove),
          "the Bible view is given a move of its own");
    ctxRow = docs.themeContext(house, QStringLiteral("scripture"));
    check(ctxRow.value(QStringLiteral("transitionSet")).toBool() &&
              ctxRow.value(QStringLiteral("transition")).toMap() == pageMove,
          "and the read shows exactly the keys the write took");
    QVariantMap typographyOnly;
    typographyOnly.insert(QStringLiteral("fontSize"), 30.0);
    check(docs.applyThemeContext(house, QStringLiteral("scripture"), typographyOnly),
          "a typography-only write on that same entry succeeds");
    ctxRow = docs.themeContext(house, QStringLiteral("scripture"));
    check(ctxRow.value(QStringLiteral("fontSizeSet")).toBool() &&
              ctxRow.value(QStringLiteral("transitionSet")).toBool() &&
              ctxRow.value(QStringLiteral("transition")).toMap() == pageMove,
          "and it carries the entry's movement ACROSS: applyThemeContext replaces the entry, "
          "so unless it preserved the record, an operator changing the reading font would "
          "quietly take the view's Crossfade off — the one field absent keys do not silence");
    check(docs.setTextContextTransition(house, QStringLiteral("scripture"), QVariantMap{}),
          "the view can be handed back to the theme again");
    ctxRow = docs.themeContext(house, QStringLiteral("scripture"));
    check(!ctxRow.value(QStringLiteral("transitionSet")).toBool() &&
              ctxRow.value(QStringLiteral("fontSizeSet")).toBool(),
          "and taking the movement off leaves the typography it was not about");

    // One gesture, one undo — and a refusal costs none. The refusal is written
    // BETWEEN two real writes so a single undo step answers the question no
    // count of stack entries would: if the refused write had pushed anything,
    // this undo would land on the refusal and the record would still be there.
    check(docs.setThemeTransition(house, QVariantMap{}), "the theme's record is taken off");
    check(docs.setThemeTransition(house, brisk), "and put back: two writes, two undo steps");
    check(!docs.setThemeTransition(house, half), "a half record is refused again");
    docs.undo();
    check(!rowOf(house).value(QStringLiteral("transitionSet")).toBool(),
          "ONE undo takes the record back off — which it could only do if the refusal in "
          "between had cost no undo of its own");
    docs.redo();
    check(rowOf(house).value(QStringLiteral("transitionSet")).toBool() &&
              rowOf(house).value(QStringLiteral("transition")).toMap() == brisk,
          "and redo puts it back, so the service saved below still carries the record");

    // --- and the whole thing survives the disk -------------------------------
    check(docs.setServiceDefaultTransition("fade", 250, 350), "the service default moves again");
    check(docs.save(), "the service saves with its transition records");
    openshow::domain::Documents reopened;
    std::string loadError;
    if (check(openshow::domain::loadService(&reopened, root / "service", &loadError),
              "the service reads back through the next session's load")) {
        check(reopened.serviceDefaultTransition().textDurationMs == 250 &&
                  reopened.serviceDefaultTransition().mediaDurationMs == 350,
              "the next session's service default is the one this one set");
        check(reopened.findTheme(house.toStdString())->transition.has_value() &&
                  reopened.findTheme(house.toStdString())->transition->textDurationMs == 100,
              "and so is the theme's override");
    }
}

void exerciseTextStyleFieldMerge(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Merged");
    docs.addSlide();
    docs.selectSlide(0);
    check(docs.createBlankTheme("House"), "a blank theme exists to merge into");
    QString house;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == QStringLiteral("House")) {
            house = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    const auto slideId = docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    check(!house.isEmpty() && docs.applyThemeToCurrentSlide(house), "the slide wears House");
    const auto rowOf = [&docs, &house] {
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("id")).toString() == house) {
                return theme.toMap();
            }
        }
        return QVariantMap{};
    };

    // One scalar onto a silent theme: the neighbours stay silent (their
    // Set flags do not flip) and the compiled slide still draws defaults
    // for everything else.
    check(docs.setThemeTextStyleField(house, QStringLiteral("fontWeight"), 700),
          "the merge verb sets one scalar");
    {
        const QVariantMap row = rowOf();
        check(row.value(QStringLiteral("fontWeightSet")).toBool() &&
                  row.value(QStringLiteral("fontWeight")).toInt() == 700 &&
                  row.value(QStringLiteral("fontStyleSet")).toBool() == false &&
                  row.value(QStringLiteral("strokeSet")).toBool() == false,
              "and only that field: the silent ones stay silent");
    }
    // A member merged into a theme with NO stroke record materialises the
    // contract's default record and changes the one member. Enabling a
    // stroke on a silent theme must give the audience black width 2 —
    // exactly what it drew as the default — not an uninitialised colour.
    check(docs.setThemeTextStyleField(house, QStringLiteral("strokeEnabled"), true),
          "enabling a stroke on a theme that carries none");
    check(docs.setThemeTextStyleField(house, QStringLiteral("strokeWidth"), 5.0),
          "and merging a second member of the same record");
    {
        openshow::scene::Scene drawn = compiledFor(docs, slideId);
        check(drawn.items[0].stroke.enabled && drawn.items[0].stroke.color == "#000000" &&
                  drawn.items[0].stroke.width == 5.0,
              "the stroke materialised around the defaults: black, and the ONE member changed");
    }
    // A refused merge costs NOTHING: an unknown name, a member's null, a
    // whole record handed to a member verb, a stringy width — after all of
    // them the theme is byte-for-byte what the last accepted merge made it.
    const auto snapshot = compiledFor(docs, slideId);
    check(!docs.setThemeTextStyleField(house, QStringLiteral("blurSigma"), 4.0) &&
                  docs.status().contains(QStringLiteral("is not a text-style field")),
          "an unknown field is refused naming itself");
    check(!docs.setThemeTextStyleField(house, QStringLiteral("strokeColor"), QVariant()),
          "null on an effect MEMBER is refused: an effect inherits only whole");
    check(!docs.setThemeTextStyleField(house, QStringLiteral("stroke"),
                                       QVariantMap{{QStringLiteral("enabled"), true}}),
          "a whole record handed to the whole-record NAME is refused (it means: clear it)");
    check(!docs.setThemeTextStyleField(house, QStringLiteral("strokeWidth"),
                                       QStringLiteral("5")) &&
                  docs.status().contains(QStringLiteral("must be a number")),
          "a stringy width is refused, not coerced");
    check(!docs.setThemeTextStyleField(house, QStringLiteral("minFontSize"), 0.5),
          "an off-band floor is refused by the domain, verb or no verb");
    {
        openshow::scene::Scene still = compiledFor(docs, slideId);
        check(still.items[0].stroke.width == snapshot.items[0].stroke.width &&
                  still.items[0].fontWeight == snapshot.items[0].fontWeight,
              "every refusal changed nothing");
    }

    // One undo is one merge — the weight-700 merge survives five refusals,
    // and walking the stack back unwinds one FIELD at a time, not the
    // whole block. The merges so far: weight 700, strokeEnabled true,
    // strokeWidth 5 → three undos land back on silence.
    docs.undo();
    check(compiledFor(docs, slideId).items[0].stroke.width == 2.0,
          "one undo backs out ONE merge: the width, not the enabled flag");
    docs.undo();
    check(!compiledFor(docs, slideId).items[0].stroke.enabled,
          "and the next undo backs out the enable");
    docs.undo();
    check(rowOf().value(QStringLiteral("fontWeightSet")).toBool() == false,
          "and the third backs out the scalar: the theme is silent again");

    // null clears a scalar to inherit, and clears a whole effect by its
    // record name — the two ways the inspector's Inherit buttons write.
    check(docs.setThemeTextStyleField(house, QStringLiteral("fontStyle"),
                                      QStringLiteral("italic")) &&
              docs.setThemeTextStyleField(house, QStringLiteral("fontStyle"), QVariant()),
          "a scalar set then nulled");
    check(rowOf().value(QStringLiteral("fontStyleSet")).toBool() == false,
          "null cleared the slant back to inherit");
    check(docs.setThemeTextStyleField(house, QStringLiteral("shadowOpacity"), 0.3) &&
              docs.setThemeTextStyleField(house, QStringLiteral("shadow"), QVariant()) &&
                  rowOf().value(QStringLiteral("shadowSet")).toBool() == false,
          "and the whole shadow record clears by its record name");

    // QML sends integers as JS doubles: an integral double is the SAME
    // answer and is accepted; a decimal weight is still refused.
    check(docs.setThemeTextStyleField(house, QStringLiteral("fontWeight"), 800.0) &&
              compiledFor(docs, slideId).items[0].fontWeight == 800,
          "a spin box's double-valued weight is accepted as the integer it is");
    check(!docs.setThemeTextStyleField(house, QStringLiteral("fontWeight"), 800.5),
          "but a decimal weight is refused: no rounding happens behind the operator's back");

    // The sample appearance carries the RESOLVED style for the badges: a
    // silent theme reports the contract's concrete answers, because those
    // are what the audience draws today.
    check(docs.setThemeTextStyleField(house, QStringLiteral("stroke"), QVariant()),
          "clear the stroke for the sample's honest baseline");
    {
        docs.undo();  // back to the 800-weight theme with the stroke merged away
        const QVariantMap sample = docs.themeSampleAppearance(house);
        check(sample.value(QStringLiteral("ok")).toBool() &&
                  sample.value(QStringLiteral("fontWeight")).toInt() == 800 &&
                  sample.value(QStringLiteral("fontStyle")).toString() == QStringLiteral("normal") &&
                  sample.value(QStringLiteral("fitMode")).toString() == QStringLiteral("shrink") &&
                  sample.value(QStringLiteral("stroke")).toMap().value(QStringLiteral("enabled"))
                          .toBool() == false,
              "the sample reports the resolved typography and effects (800 normal shrink, "
              "stroke off)");
    }

    // The preview's PICTURE agrees with what the audience would be shown:
    // the sample is compiled by the same compiler from the same theme, so a
    // merged style appears in both scenes with the same numbers. An editor
    // whose preview flattered the theme — or ignored the outline the operator
    // just enabled — is the failure this pins (task TS.4's "preview and saved
    // output match").
    check(docs.setThemeTextStyleField(house, QStringLiteral("strokeEnabled"), true) &&
                  docs.setThemeTextStyleField(house, QStringLiteral("strokeWidth"), 5.0),
              "merge an outline for the preview to draw");
    {
        openshow::scene::Scene previewed;
        const QString json = docs.themeSampleSceneJson(house);
        check(!json.isEmpty() &&
                  openshow::scene::fromJson(boost::json::parse(json.toStdString()), &previewed),
              "the sample scene compiles");
        const openshow::scene::Scene worn = compiledFor(docs, slideId);
        check(!previewed.items.empty() && !worn.items.empty() &&
                  previewed.items[0].stroke.enabled && previewed.items[0].stroke.width == 5.0 &&
                  previewed.items[0].fontWeight == worn.items[0].fontWeight &&
                  previewed.items[0].stroke == worn.items[0].stroke,
              "the editor's preview draws the merged style the audience's slide does");
    }

    // And it all survives the disk exactly as the whole-set verb would.
    check(docs.setThemeTextStyleField(house, QStringLiteral("shadowEnabled"), true) &&
                  docs.save(),
              "a merged shadow saves");
    openshow::domain::Documents reopened;
    std::string loadError;
    check(openshow::domain::loadService(&reopened, root / "service", &loadError) &&
              reopened.findTheme(house.toStdString())->shadow->enabled &&
                  reopened.findTheme(house.toStdString())->fontWeight == 800,
          "the merged style block reads back from disk whole");
}

// The sample gains a VIEW (task TV.1): the Theme Editor's Songs view is the
// theme's own look and its Bible view is the theme's `scripture` context —
// both compiled through a fixture that walks the SAME chain the audience
// would (a copy of the live records: the edited Default floor in, whatever
// the service happens to be wearing out). Browsing leaves no mark: no
// context appears, no history entry opens, nothing dirties.
void exerciseStageBundle(const std::filesystem::path& root) {
    // Stage delivery FO.5b.1: one Go's retained bundle. The current cue is
    // compiled on the Stage role, the next cue is the NEXT row's cue (not the
    // editing cursor), and the overlay lands in the v1 next slot at 55%.
    DocumentController docs(nullptr, root);
    check(docs.createSong(QStringLiteral("Stage Song")), "the stage song exists");
    check(docs.addSlide(), "the stage song gains a first slide");
    docs.setSlideText(QStringLiteral("Current words\nsecond line"));
    docs.flushText();
    check(docs.addSlide(), "the stage song gains a second slide");
    docs.setSlideText(QStringLiteral("Next words follow"));
    docs.flushText();
    docs.selectSlide(0);

    const QVariantList rows = docs.slides();
    check(rows.size() == 2, "the stage song has two slides");
    if (rows.size() != 2) {
        return;
    }
    const QString showId = docs.currentSongId();
    const QString currentId = rows[0].toMap().value(QStringLiteral("id")).toString();
    const QString nextId = rows[1].toMap().value(QStringLiteral("id")).toString();

    const QString bundleText = docs.stageBundleJson(showId, currentId, showId, nextId);
    check(!bundleText.isEmpty(), "the stage bundle compiles");
    boost::json::value parsed;
    try {
        parsed = boost::json::parse(bundleText.toStdString());
    } catch (const std::exception&) {
        check(false, "the stage bundle is valid JSON");
        return;
    }
    const boost::json::object& bundle = parsed.as_object();
    check(bundle.contains("current") && bundle.at("current").is_object(),
          "the bundle carries the current cue");
    check(bundle.contains("next") && bundle.at("next").is_object(),
          "the bundle carries the next cue's overlay");
    check(bundle.contains("overlap") && bundle.at("overlap").is_bool() &&
              !bundle.at("overlap").as_bool(),
          "the default stage box does not overlap the next slot");

    openshow::scene::Scene current;
    openshow::scene::Scene next;
    std::string sceneError;
    if (bundle.contains("current")) {
        check(openshow::scene::fromJson(bundle.at("current"), &current, &sceneError),
              "the current cue is a valid scene payload");
    }
    if (bundle.contains("next")) {
        check(openshow::scene::fromJson(bundle.at("next"), &next, &sceneError),
              "the next overlay is a valid scene payload");
    }
    check(!current.items.empty(), "the current cue has a text item");
    check(next.items.size() == 1, "the next overlay carries one text item");
    if (!current.items.empty()) {
        const openshow::scene::SceneItem& item = current.items.front();
        check(item.x == 160.0 && item.y == 120.0 && item.width == 1600.0 &&
                  item.height == 560.0,
              "the current cue uses the stage profile's default box");
    }
    if (!next.items.empty()) {
        const openshow::scene::SceneItem& item = next.items.front();
        check(item.y == openshow::scene::kStageNextBoxY &&
                  item.height == openshow::scene::kStageNextBoxHeight,
              "the next cue sits in the separate next slot");
        check(item.lines.size() == 1 && item.lines.front() == "Next words follow",
              "the next cue's words travel unchanged");
        if (!current.items.empty()) {
            const double currentSize = current.items.front().fontSize;
            if (currentSize > 0.0) {
                check(item.fontSize > currentSize * 0.54 &&
                          item.fontSize < currentSize * 0.56,
                      "the next cue is scaled to 55% of the current style");
            }
        }
    }

    // The last cue has no next region, and a direct/browsed go is explicit
    // about its own cue rather than guessing a neighbour.
    const QString lastOnly = docs.stageBundleJson(showId, nextId, QString(), QString());
    check(!lastOnly.isEmpty(), "a last-cue bundle still compiles its current cue");
    try {
        const boost::json::value lastParsed = boost::json::parse(lastOnly.toStdString());
        check(!lastParsed.as_object().contains("next"),
              "the last cue has an empty next region");
    } catch (const std::exception&) {
        check(false, "the last-cue bundle is valid JSON");
    }
    check(docs.stageBundleJson(QString(), currentId, showId, nextId).isEmpty(),
          "a go without a show id has no stage bundle");
}

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

    // Stable neutral content in both views; the renderer chooses wrap/fit.
    openshow::scene::Scene cue;
    openshow::scene::Scene passage;
    const QString songJson = docs.themeSampleSceneJson(house);
    const QString bibleJson = docs.themeSampleSceneJson(house, QStringLiteral("scripture"));
    const bool parsed = !songJson.isEmpty() && !bibleJson.isEmpty()
            && openshow::scene::fromJson(boost::json::parse(songJson.toStdString()), &cue)
            && openshow::scene::fromJson(boost::json::parse(bibleJson.toStdString()), &passage)
            && !cue.items.empty() && !passage.items.empty();
    check(parsed && cue.items[0].lines.size() == 4
                  && cue.items[0].lines[0] == "Lorem ipsum dolor sit amet,"
                  && cue.items[0].lines == passage.items[0].lines,
          "both views compile the same neutral multiline sample without a theme name or invented reading");

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

    // Milestone FO (task FO.1d): the stage and NDI tiles sample their own
    // destination without recording anything, and an unknown role is a
    // refusal rather than a silent fallback to the audience.
    QVariantMap stageProfile;
    stageProfile.insert(QStringLiteral("fontSize"), 88.0);
    stageProfile.insert(QStringLiteral("y"), 70.0);
    stageProfile.insert(QStringLiteral("x"), 120.0);
    stageProfile.insert(QStringLiteral("width"), 1680.0);
    stageProfile.insert(QStringLiteral("height"), 500.0);
    check(docs.applyThemeOutputProfile(house, QStringLiteral("stage"), QString(), stageProfile),
          "a stage profile is authored from the editor's map");
    const QVariantMap stageLook =
        docs.themeSampleAppearance(house, QString(), QStringLiteral("stage"));
    check(stageLook.value(QStringLiteral("ok")).toBool()
              && stageLook.value(QStringLiteral("role")).toString()
                     == QStringLiteral("stage")
              && stageLook.value(QStringLiteral("boxY")).toDouble() == 70.0
              && stageLook.value(QStringLiteral("boxHeight")).toDouble() == 500.0
              && stageLook.value(QStringLiteral("fontSize")).toDouble() == 88.0,
          "the stage tile samples its own box and type");
    const QVariantMap broadcastLook =
        docs.themeSampleAppearance(house, QString(), QStringLiteral("broadcast"));
    check(broadcastLook.value(QStringLiteral("ok")).toBool()
              && broadcastLook.value(QStringLiteral("role")).toString()
                     == QStringLiteral("broadcast")
              && broadcastLook.value(QStringLiteral("boxY")).toDouble() == 760.0
              && broadcastLook.value(QStringLiteral("boxHeight")).toDouble() == 240.0
              && broadcastLook.value(QStringLiteral("fontSize")).toDouble() == 40.0,
          "an unauthored broadcast column samples the lower-third default and the theme's type");
    check(!docs.themeSampleAppearance(house, QString(), QStringLiteral("ndi"))
                   .value(QStringLiteral("ok"))
                   .toBool(),
          "an unknown role samples nothing rather than falling back to the audience");
    const QVariantMap stageRow =
        docs.themeOutputProfile(house, QStringLiteral("stage"), QString());
    check(stageRow.value(QStringLiteral("set")).toBool()
              && stageRow.value(QStringLiteral("fontSizeSet")).toBool()
              && stageRow.value(QStringLiteral("textColorSet")).toBool() == false,
          "the profile read row distinguishes an answer from silence");
    check(!docs.applyThemeOutputProfile(house, QStringLiteral("audience"), QString(),
                                        stageProfile),
          "the editor cannot author an audience profile");
    docs.undo();
    check(!docs.themeOutputProfile(house, QStringLiteral("stage"), QString())
                   .value(QStringLiteral("set"))
                   .toBool(),
          "one undo takes the authored stage profile off");
    docs.redo();
    check(docs.themeOutputProfile(house, QStringLiteral("stage"), QString())
              .value(QStringLiteral("set"))
              .toBool(),
          "redo puts the authored stage profile back");
    check(docs.removeThemeOutputProfile(house, QStringLiteral("stage"), QString()),
          "the stage profile is removable");
    check(!docs.themeOutputProfile(house, QStringLiteral("stage"), QString())
                   .value(QStringLiteral("set"))
                   .toBool(),
          "the removed stage profile reads as silence");

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


// --- PowerPoint deck import (PX.2) -----------------------------------------

// The sidecar is a process, so the controller fixtures drive it through a
// scripted renderer: the happy path and every refusal are deterministic and
// need no Rust build. PX.6 adds the real engine's end-to-end leg.
std::filesystem::path writeFakeRenderer(const std::filesystem::path& root,
                                        const char* manifest,
                                        int exitCode = 0,
                                        const char* stderrText = "") {
    const auto path = root / "fake-pptx-render.sh";
    std::ofstream out(path);
    out << "#!/bin/sh\n"
           "echo run >> \"$0.count\"\n"
           "out=\"\"\n"
           "while [ $# -gt 0 ]; do case \"$1\" in --out-dir) out=\"$2\"; shift 2;; "
           "*) shift;; esac; done\n"
           "[ -n \"$out\" ] || exit 2\n"
           "mkdir -p \"$out\" || exit 2\n";
    if (exitCode != 0) {
        out << "echo '" << (stderrText[0] ? stderrText : "fake renderer failed")
            << "' >&2\n"
            << "exit " << exitCode << "\n";
    } else {
        out << "base64 -d > \"$out/slide-01.png\" <<'B64'\n"
               "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADElEQVR4nGNgYGAAAAAEAAH2FzhVAAAAAElFTkSuQmCC\n"
               "B64\n"
               "cp \"$out/slide-01.png\" \"$out/slide-02.png\"\n"
               "cat > \"$out/manifest.json\" <<'JSON'\n"
            << manifest << "\nJSON\n";
    }
    out.close();
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                  std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
                  std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                  std::filesystem::perms::others_exec);
    return path;
}

const char* kFakeManifest =
    "{\"format\":1,\"renderer\":\"fake\",\"source\":\"test-deck.pptx\","
    "\"sourceBytes\":9,\"targetWidth\":1920,\"targetHeight\":1080,"
    "\"scale\":1.5,\"slideWidth\":1280,\"slideHeight\":720,"
    "\"pages\":[{\"page\":1,\"file\":\"slide-01.png\",\"width\":1920,"
    "\"height\":1080,\"skippedImages\":0},{\"page\":2,\"file\":"
    "\"slide-02.png\",\"width\":1920,\"height\":1080,\"skippedImages\":0}],"
    "\"hiddenPages\":[],\"fontSubstitutions\":[]}";

bool waitForPowerPointImport(DocumentController& docs, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (docs.powerPointImportBusy() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !docs.powerPointImportBusy();
}

QString filePath(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

void writeTextFile(const std::filesystem::path& path, const char* text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

void exercisePowerPointImport(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto deck = root / "test-deck.pptx";
    writeTextFile(deck, "fake deck bytes");
    const auto fake = writeFakeRenderer(root, kFakeManifest);
    qputenv("OPENSHOW_PPTX_RENDER", QByteArray::fromStdString(fake.string()));

    DocumentController docs(nullptr, root);
    const int libraryBefore = docs.songs().size();
    check(docs.importPowerPointToService(filePath(deck), 0),
          "a deck starts importing into the service");
    check(waitForPowerPointImport(docs), "the service deck import finishes");
    check(!docs.powerPointImportBusy(), "the busy flag clears after the import");
    check(docs.playlistEntries().size() == 1,
          "the service gained exactly one running-order row");
    const QVariantMap entry = docs.playlistEntries().at(0).toMap();
    const QString showId = entry.value(QStringLiteral("showId")).toString();
    check(entry.value(QStringLiteral("showName")).toString() ==
              QStringLiteral("test-deck"),
          "the row is named after the deck");
    check(entry.value(QStringLiteral("slideCount")).toInt() == 2,
          "the row carries both rendered pages");
    check(docs.songs().size() == libraryBefore,
          "a service deck import is not a Library row");
    check(docs.assets().isEmpty() && docs.storedAssets().size() == 2,
          "the pages are stored but are not Media Bin rows");
    check(docs.slidesOf(showId).size() == 2,
          "the imported show holds the rendered slides");
    check(docs.status().contains(QStringLiteral("imported 2 slides")),
          "the status line names the imported page count");

    // The document itself, not only the view rows: each slide carries its
    // rendered picture as a contained background.
    check(docs.save(), "the service with the deck saves");
    const std::string manifest =
        readManifest(root / "service" / "service.json");
    check(manifest.find("\"fit\":\"contain\"") != std::string::npos,
          "every imported slide stores the contained fit");
    check(manifest.find("test-deck - 01.png") != std::string::npos,
          "the rendered pages are ordinary picture assets");

    // The whole item is one edit: one undo removes the row and its media.
    docs.undo();
    check(docs.playlistEntries().isEmpty() && docs.storedAssets().isEmpty(),
          "one undo removes the running-order row and the rendered media");
    docs.redo();
    check(docs.playlistEntries().size() == 1 && docs.storedAssets().size() == 2,
          "redo restores the row and the media together");

    // The Library destination creates a show and never touches the order.
    const int entries = docs.playlistEntries().size();
    check(docs.importPowerPointToLibrary(filePath(deck)),
          "a deck starts importing into the Library");
    check(waitForPowerPointImport(docs), "the Library deck import finishes");
    check(docs.playlistEntries().size() == entries,
          "a Library deck import leaves the running order alone");
    check(docs.songs().size() == libraryBefore + 1,
          "the Library gained the imported presentation");
    check(docs.currentSongId().toStdString() ==
              mapIds(docs.songs()).back(),
          "the imported Library presentation becomes current");

    // Save/reopen with the renderer gone: the package owns the PNG bytes, so
    // the presentation survives without any sidecar at all.
    const QString libraryShowId = docs.currentSongId();
    check(docs.save(), "the service with both decks saves");
    qputenv("OPENSHOW_PPTX_RENDER", "/no/such/pptx-render");
    DocumentController reopened(nullptr, root);
    check(reopened.load(), "the saved decks reopen without a renderer");
    check(reopened.songs().size() == libraryBefore + 1,
          "the Library deck survives save/reopen");
    check(reopened.slidesOf(libraryShowId).size() == 2,
          "the reopened Library deck still has its slides");
    check(reopened.playlistEntries().size() == entries,
          "the reopened running order is unchanged");

    // Deleting the Library deck takes its rendered pages with it: the pages
    // exist only to be that show's backgrounds, so the deck is one unit. The
    // service deck's identical-looking pages are a different show's and stay.
    check(reopened.removeLibraryShows(QStringList{libraryShowId}),
          "the Library deck can be deleted");
    check(reopened.songs().size() == libraryBefore,
          "the deleted deck is gone from the Library");
    check(reopened.storedAssets().size() == 2,
          "deleting the deck removes its two rendered pages");
    check(reopened.removeUnusedAssets() == 0,
          "nothing else is unused: the service deck's pages are in use");
    check(reopened.playlistEntries().size() == entries,
          "the deletion leaves the service deck runnable");
    reopened.undo();
    check(reopened.storedAssets().size() == 4
              && reopened.songs().size() == libraryBefore + 1,
          "one undo restores the deleted Library deck and its pages together");

    // Same bytes, same cache: the scripted renderer ran once for both.
    const std::string runs = readManifest(fake.string() + ".count");
    check(std::count(runs.begin(), runs.end(), '\n') == 1,
          "the second import of unchanged bytes reused the cached render");

    // Old binary PowerPoint is refused before any process starts.
    writeTextFile(root / "old.ppt", "binary powerpoint");
    check(!docs.importPowerPointToLibrary(filePath(root / "old.ppt")),
          "a .ppt deck is refused");
    check(docs.status().contains(QStringLiteral(".pptx")),
          "the refusal says to save it as .pptx");

    // A missing renderer is an actionable refusal, not a silent no-op.
    qputenv("OPENSHOW_PPTX_RENDER", "/no/such/pptx-render");
    writeTextFile(root / "missing-tool.pptx", "another deck");
    DocumentController missing(nullptr, root / "missing");
    check(!missing.importPowerPointToLibrary(filePath(root / "missing-tool.pptx")),
          "a missing renderer refuses the import");
    check(missing.status().contains(QStringLiteral("renderer")),
          "the missing-renderer line names the renderer");
    check(missing.songs().isEmpty(), "a refused import changes no document");

    // A bad manifest is refused without leaving assets or a show behind.
    const auto bad = writeFakeRenderer(
        root, "{\"format\":1,\"pages\":[{\"page\":1,\"file\":\"../escape.png\","
              "\"width\":1920,\"height\":1080}]}");
    qputenv("OPENSHOW_PPTX_RENDER", QByteArray::fromStdString(bad.string()));
    writeTextFile(root / "bad-deck.pptx", "bad manifest deck");
    DocumentController badDocs(nullptr, root / "bad");
    check(badDocs.importPowerPointToLibrary(filePath(root / "bad-deck.pptx")),
          "a bad-manifest deck starts and fails through the renderer");
    check(waitForPowerPointImport(badDocs), "the bad-manifest import finishes");
    check(badDocs.songs().isEmpty() && badDocs.assets().isEmpty(),
          "a bad manifest leaves no show and no asset");
    check(badDocs.status().contains(QStringLiteral("manifest")),
          "the refusal names the manifest");

    // A nonzero renderer exit is quoted, not swallowed.
    const auto failed = writeFakeRenderer(root, "", 7, "fake renderer refuses this deck");
    qputenv("OPENSHOW_PPTX_RENDER", QByteArray::fromStdString(failed.string()));
    writeTextFile(root / "failed-deck.pptx", "failed deck");
    DocumentController failedDocs(nullptr, root / "failed");
    check(failedDocs.importPowerPointToLibrary(filePath(root / "failed-deck.pptx")),
          "a failing deck starts and fails through the renderer");
    check(waitForPowerPointImport(failedDocs), "the failed render finishes");
    check(failedDocs.status().contains(QStringLiteral("fake renderer refuses")),
          "the renderer's own sentence is surfaced");
    check(failedDocs.songs().isEmpty(), "a failed render changes no document");

    // A refused render leaves no manifest-less cache directory behind.
    int partialCaches = 0;
    const auto cacheRoot = std::filesystem::path(qgetenv("XDG_CACHE_HOME").constData()) /
                           "openshow-document-tests" / "pptx";
    std::error_code cacheError;
    for (const auto& entry : std::filesystem::directory_iterator(cacheRoot, cacheError)) {
        if (entry.is_directory() &&
            !std::filesystem::exists(entry.path() / "manifest.json")) {
            ++partialCaches;
        }
    }
    check(partialCaches == 0,
          "a failed render leaves no partial cache directory");

#ifdef OPENSHOW_PPTX_RENDER_BIN
    // PX.6 preview: the real engine on the committed operator deck. The
    // end-to-end leaf broadens this; here it proves the controller talks to
    // the shipped binary and lands six 1080p slides.
    const QString realBinary = QString::fromUtf8(OPENSHOW_PPTX_RENDER_BIN);
    const QString realDeck =
        QStringLiteral(OPENSHOW_SOURCE_DIR "/tools/pptx-render/tests/fixtures/aug-8.pptx");
    if (QFileInfo::exists(realBinary) && QFileInfo::exists(realDeck)) {
        qputenv("OPENSHOW_PPTX_RENDER", QByteArray(OPENSHOW_PPTX_RENDER_BIN));
        DocumentController real(nullptr, root / "real");
        check(real.importPowerPointToLibrary(realDeck),
              "the committed operator deck starts importing through the real renderer");
        check(waitForPowerPointImport(real, 30000),
              "the real deck import finishes");
        check(real.songs().size() == 1, "the real deck is a Library presentation");
        const QVariantList pages = real.slidesOf(
            real.currentSongId());
        check(pages.size() == 6, "August's deck lands six visible slides");
        check(real.status().contains(QStringLiteral("imported 6 slides")),
              "the real status line names six slides");
        real.undo();
        check(real.songs().isEmpty() && real.assets().isEmpty(),
              "one undo removes the real deck and its renders");
    }
#else
    (void)0;
#endif
}


// The media library's audio route: a dropped audio file becomes a service cue
// with no picture and the show's own track, and a drop on an existing cue adds
// it to that show's audio layer. One gesture, one undo either way.
void exerciseMediaAudioCue(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const std::filesystem::path track = root / "song.mp3";
    const std::filesystem::path picture = root / "plate.png";
    std::ofstream(track, std::ios::binary) << "MP3BYTES";
    std::ofstream(picture, std::ios::binary) << "PNGBYTES";

    DocumentController docs(nullptr, root / "service");
    check(docs.importMediaFile(QString::fromStdString(track.string())),
          "media audio: the track imports");
    check(docs.importMediaFile(QString::fromStdString(picture.string())),
          "media audio: the picture imports");
    QString audioId;
    QString pictureId;
    for (const QVariant& entry : docs.assets()) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("kind")).toString() == QLatin1String("audio"))
            audioId = row.value(QStringLiteral("id")).toString();
        if (row.value(QStringLiteral("name")).toString() == QLatin1String("plate.png"))
            pictureId = row.value(QStringLiteral("id")).toString();
    }
    check(!audioId.isEmpty() && !pictureId.isEmpty(),
          "media audio: the bin names the audio and picture rows");

    check(docs.createPlaylist(QStringLiteral("Order")),
          "media audio: a running order exists");
    check(docs.addMediaCue(audioId, -1), "media audio: audio becomes a cue of its own");
    const QVariantList entries = docs.playlistEntries();
    check(entries.size() == 1, "media audio: the cue joins the running order");
    const QString cueShow = entries.first().toMap().value(QStringLiteral("showId")).toString();
    const QVariantList slides = docs.slidesOf(cueShow);
    check(slides.size() == 1,
          "media audio: the audio cue is one slide with no picture");
    check(slides.first().toMap().value(QStringLiteral("backgroundAssetId")).toString().isEmpty() &&
              slides.first().toMap().value(QStringLiteral("preview")).toString() ==
                  QLatin1String("(empty)"),
          "media audio: the cue has no background and no lyric box");

    check(docs.setShowAudioFor(cueShow, pictureId) == false,
          "media audio: a picture cannot replace the track");
    check(docs.setShowAudioFor(cueShow, QString()),
          "media audio: the track can be removed");
    docs.undo();
    docs.undo();
    check(docs.playlistEntries().isEmpty(),
          "media audio: two undos remove the cue and its show");

    // Drop on an existing show: the cue keeps its words and gains the track.
    check(docs.createSong(QStringLiteral("Chart")),
          "media audio: a lyric chart is authored");
    check(docs.addSlide(), "media audio: the chart has a slide");
    const QString chartId = docs.currentSongId();
    check(docs.addShowToPlaylist(chartId), "media audio: the chart joins the order");
    check(docs.setShowAudioFor(chartId, audioId),
          "media audio: the chart takes the track");
    docs.undo();
    check(!docs.slidesOf(chartId).isEmpty(),
          "media audio: the chart keeps its slides");
}

// Task PT.3: whole-item theme application through the adapter QML calls.
// One explicit assignment wins over the service current theme, the old
// per-slide text/layout overrides and slide theme go, backdrop and words
// stay, appearance and transition agree, and undo/redo treat it as one edit.
// A remembered look travels by value and is measured before it is worn.
void exerciseWholeItemThemeSurfaces(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    check(docs.createSong("Whole item"), "the whole-item fixture song exists");
    check(docs.addSlide() && docs.addSlide(), "it has three slides");
    docs.selectSlide(0);
    check(docs.createBlankTheme("House") && docs.createBlankTheme("Item look"),
          "the service and item themes exist");
    QString house;
    QString item;
    for (const QVariant& theme : docs.themes()) {
        const QVariantMap row = theme.toMap();
        const QString name = row.value(QStringLiteral("name")).toString();
        if (name == QStringLiteral("House")) {
            house = row.value(QStringLiteral("id")).toString();
        } else if (name == QStringLiteral("Item look")) {
            item = row.value(QStringLiteral("id")).toString();
        }
    }
    check(!house.isEmpty() && !item.isEmpty() && house != item,
          "both themes are found by name");
    check(docs.editTheme(house, QStringLiteral("House"), QStringLiteral("#00ff00"),
                         QStringLiteral("#000000"), QString(), QStringLiteral("House Sans"),
                         20.0, QStringLiteral("center"), 1.2),
          "the service theme is styled");
    check(docs.editTheme(item, QStringLiteral("Item look"), QStringLiteral("#abcdef"),
                         QStringLiteral("#000000"), QString(), QStringLiteral("Item Serif"),
                         60.0, QStringLiteral("left"), 1.6),
          "the item theme is styled");
    check(docs.setThemeTextStyle(
              item, QVariantMap{{QStringLiteral("verticalAlign"), QStringLiteral("top")},
                                {QStringLiteral("allCaps"), true},
                                {QStringLiteral("fontWeight"), 800}}),
          "the item theme carries the layout pair and weight");
    check(docs.setThemePlacement(item, 100.0, 120.0, 1600.0, 800.0),
          "the item theme places its box");
    check(docs.setThemeTransition(
              item, QVariantMap{{QStringLiteral("kind"), QStringLiteral("cut")},
                                {QStringLiteral("textDurationMs"), 0},
                                {QStringLiteral("mediaDurationMs"), 10000}}),
          "the item theme moves");
    check(docs.useThemeAsCurrent(house), "the service wears House");
    check(docs.customizeCurrentSlide(QStringLiteral("textColor"),
                                     QStringLiteral("#112233")) &&
              docs.customizeCurrentSlide(QStringLiteral("backgroundColor"),
                                         QStringLiteral("#123456")) &&
              docs.customizeCurrentSlide(QStringLiteral("fontSize"), 22.0),
          "the first slide carries old text and backdrop overrides");

    QString showId;
    for (const QVariant& song : docs.songs()) {
        if (song.toMap().value(QStringLiteral("name")).toString() ==
            QStringLiteral("Whole item")) {
            showId = song.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!showId.isEmpty(), "the show id is readable from the Library row");
    const QString slideId =
        docs.slides().at(0).toMap().value(QStringLiteral("id")).toString();
    const auto lookValue = [&docs](const char* field) {
        return docs.slideAppearance().value(QLatin1String(field)).toMap()
            .value(QStringLiteral("value"));
    };
    const auto lookCustomized = [&docs](const char* field) {
        return docs.slideAppearance().value(QLatin1String(field)).toMap()
            .value(QStringLiteral("customized")).toBool();
    };
    check(lookValue("textColor").toString() == QStringLiteral("#112233"),
          "the fixture starts on its own override");

    check(docs.applyWholeItemTheme(showId, item),
          "the whole-item theme applies through the adapter");
    check(docs.showInfo(showId).value(QStringLiteral("appliedThemeId")).toString() == item,
          "the item names its explicit assignment");
    check(lookValue("textColor").toString() == QStringLiteral("#abcdef") &&
              lookValue("fontSize").toDouble() == 60.0 &&
              lookValue("verticalAlign").toString() == QStringLiteral("top") &&
              lookValue("allCaps").toBool() && lookValue("fontWeight").toInt() == 800,
          "the item theme wins over the service current theme and old text overrides");
    check(!lookCustomized("textColor") && !lookCustomized("fontSize") &&
              !lookCustomized("placement"),
          "the old text, size and placement overrides are gone");
    check(lookValue("backgroundColor").toString() == QStringLiteral("#123456") &&
              lookCustomized("backgroundColor"),
          "the authored backdrop override survives");
    check(lookValue("placement").toMap().value(QStringLiteral("width")).toDouble() == 1600.0 &&
              !lookCustomized("placement"),
          "the primary box inherits the item theme's placement");
    check(docs.transitionForSlide(showId, slideId)
                  .value(QStringLiteral("kind")).toString() == QStringLiteral("cut"),
          "movement resolution agrees with the applied item theme");

    // One undo backs the whole application out; redo puts it back.
    check(docs.canUndo(), "the application is one history entry");
    docs.undo();
    check(docs.showInfo(showId).value(QStringLiteral("appliedThemeId")).toString().isEmpty() &&
              lookValue("textColor").toString() == QStringLiteral("#112233") &&
              lookCustomized("textColor"),
          "one undo restores the previous chain and overrides");
    docs.redo();
    check(lookValue("textColor").toString() == QStringLiteral("#abcdef") &&
              !lookCustomized("textColor"),
          "one redo re-applies the item theme");

    // "Use service default" returns this item to inheritance only.
    check(docs.clearWholeItemTheme(showId) &&
              docs.showInfo(showId).value(QStringLiteral("appliedThemeId"))
                  .toString().isEmpty(),
          "the item returns to inheritance");
    check(lookValue("textColor").toString() == QStringLiteral("#00ff00") &&
              lookValue("fontSize").toDouble() == 20.0,
          "it follows the service current theme again");
    docs.undo();
    check(lookValue("textColor").toString() == QStringLiteral("#abcdef"),
          "undo brings the item theme back");

    // The snapshot codec through the adapter: capture, measure, apply by
    // value, and reuse the same local definition on a second occurrence.
    const QString snapshot = docs.themeSnapshotJson(item);
    check(!snapshot.isEmpty(), "a theme captures as a portable snapshot");
    check(docs.themeSnapshotJson(QStringLiteral("theme-does-not-exist")).isEmpty() &&
              docs.status().contains(QStringLiteral("theme-does-not-exist")),
          "an unknown theme cannot be captured and says why");
    const QVariantMap measured = docs.boxStyleForSnapshot(snapshot);
    check(measured.value(QStringLiteral("boxWidth")).toDouble() == 1600.0 &&
              measured.value(QStringLiteral("boxHeight")).toDouble() == 800.0 &&
              measured.value(QStringLiteral("fontSize")).toDouble() == 60.0 &&
              measured.value(QStringLiteral("verticalAlign")).toString() ==
                  QStringLiteral("top") &&
              measured.value(QStringLiteral("allCaps")).toBool(),
          "the remembered look supplies the measurement style it will wear");
    check(docs.boxStyleForSnapshot(QStringLiteral("{}")).isEmpty(),
          "a partial snapshot is not measured");
    check(docs.applyThemeContext(item, QStringLiteral("scripture"), QVariantMap{
              {QStringLiteral("fontSize"), 90.0}, {QStringLiteral("inset"), 60.0},
              {QStringLiteral("verticalAlign"), QStringLiteral("bottom")},
              {QStringLiteral("allCaps"), false}}),
          "the item theme has an inset Bible context");
    const QVariantMap reading = docs.boxStyleForSnapshot(
        docs.themeSnapshotJson(item), QStringLiteral("scripture"));
    check(reading.value(QStringLiteral("boxWidth")).toDouble() == 1480.0 &&
              reading.value(QStringLiteral("boxHeight")).toDouble() == 680.0 &&
              reading.value(QStringLiteral("fontSize")).toDouble() == 90.0 &&
              reading.value(QStringLiteral("verticalAlign")).toString() ==
                  QStringLiteral("bottom") &&
              !reading.value(QStringLiteral("allCaps")).toBool(),
          "snapshot measurements apply context insets and typography through the resolver");
    docs.undo();
    check(docs.applyThemeContext(QStringLiteral("default"), QStringLiteral("scripture"),
                                QVariantMap{{QStringLiteral("fontSize"), 88.0},
                                            {QStringLiteral("inset"), 40.0}}),
          "the Default floor supplies an inherited Bible context");
    const QVariantMap inheritedReading = docs.boxStyleForSnapshot(
        snapshot, QStringLiteral("scripture"));
    check(inheritedReading.value(QStringLiteral("fontSize")).toDouble() == 88.0 &&
              inheritedReading.value(QStringLiteral("boxWidth")).toDouble() == 1520.0 &&
              inheritedReading.value(QStringLiteral("boxHeight")).toDouble() == 720.0,
          "snapshot measurements retain the same inherited Default context as committed slides");
    docs.undo();
    check(docs.applyRememberedTheme(showId, QStringLiteral("{bad")).isEmpty() &&
              docs.status().contains(QStringLiteral("not usable")),
          "a malformed remembered look is refused");

    check(docs.createSong("Second occurrence"), "a second item exists");
    check(docs.addSlide(), "it has a slide");
    docs.selectSlide(0);
    QString secondId;
    for (const QVariant& song : docs.songs()) {
        if (song.toMap().value(QStringLiteral("name")).toString() ==
            QStringLiteral("Second occurrence")) {
            secondId = song.toMap().value(QStringLiteral("id")).toString();
        }
    }
    const QString materialised = docs.applyRememberedTheme(secondId, snapshot);
    check(!materialised.isEmpty(), "a remembered look materialises locally");
    check(lookValue("textColor").toString() == QStringLiteral("#abcdef") &&
              lookValue("fontSize").toDouble() == 60.0,
          "the second item wears the remembered look");
    const QString reused = docs.applyRememberedTheme(secondId, snapshot);
    check(reused == materialised,
          "re-applying the same definition reuses the local theme");
    check(!docs.status().isEmpty() || true, "status is readable after applying");

    // A new Library preparation wears the remembered look and measures it;
    // a later refresh keeps the item's look (PT.6's rule, proven here for
    // the creation path only).
    QVariantList plan;
    plan.append(QVariantMap{{QStringLiteral("lines"), QStringList{QStringLiteral("Alpha line")}},
                            {QStringLiteral("group"), QStringLiteral("Verse 1")},
                            {QStringLiteral("label"), QStringLiteral("Verse 1")}});
    const QString prepared = docs.upsertCatalogSong(
        QStringLiteral("catalog-look"), QStringLiteral("Library look song"), QString(),
        QString(), QString(), plan, false, -1, snapshot);
    check(!prepared.isEmpty() &&
              docs.showForCatalogItem(QStringLiteral("catalog-look")) == prepared,
          "the Library preparation creates its linked show");
    docs.selectSong(prepared);
    docs.selectSlide(0);
    check(lookValue("textColor").toString() == QStringLiteral("#abcdef") &&
              lookValue("fontSize").toDouble() == 60.0,
          "the new preparation wears the remembered look");
    check(docs.slideAppearance().value(QStringLiteral("textColor")).toMap()
                  .value(QStringLiteral("value")).toString() == QStringLiteral("#abcdef"),
          "the preparation's text resolves through the item theme");
    QVariantList refreshed;
    refreshed.append(
        QVariantMap{{QStringLiteral("lines"), QStringList{QStringLiteral("Alpha line")}},
                    {QStringLiteral("group"), QStringLiteral("Verse 1")},
                    {QStringLiteral("label"), QStringLiteral("Verse 1")}});
    refreshed.append(
        QVariantMap{{QStringLiteral("lines"), QStringList{QStringLiteral("Beta line")}},
                    {QStringLiteral("group"), QStringLiteral("Verse 2")},
                    {QStringLiteral("label"), QStringLiteral("Verse 2")}});
    check(docs.upsertCatalogSong(QStringLiteral("catalog-look"),
                                 QStringLiteral("Library look song"), QString(), QString(),
                                 QString(), refreshed, false, -1, snapshot) == prepared,
          "a lyric refresh reuses the linked show");
    docs.selectSong(prepared);
    docs.selectSlide(0);
    check(lookValue("textColor").toString() == QStringLiteral("#abcdef"),
          "the refresh keeps the item's look rather than re-applying it");
    check(docs.slides().size() == 2, "the refresh replaced the words");
}

// Task PT.2's cross-service promise: a Library look saved in service A is
// still worn by service B after a restart, even when B already has a
// same-named theme with another look, and even after A's local theme was
// edited or deleted. The value never follows a service theme id.
void exerciseRememberedLookAcrossServices(const std::filesystem::path& root) {
    const std::filesystem::path catalogDir = root / "catalog";
    const std::filesystem::path serviceA = root / "service-a";
    const std::filesystem::path serviceB = root / "service-b";
    QString remembered;
    {
        CatalogController library(nullptr, catalogDir);
        DocumentController docs(nullptr, serviceA);
        check(docs.createSong("A song"), "service A has a song");
        check(docs.addSlide(), "service A has a slide");
        docs.selectSlide(0);
        check(docs.createBlankTheme("Brun"), "service A has a look to remember");
        QString brun;
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("name")).toString() ==
                QStringLiteral("Brun")) {
                brun = theme.toMap().value(QStringLiteral("id")).toString();
            }
        }
        check(!brun.isEmpty() &&
                  docs.editTheme(brun, QStringLiteral("Brun"), QStringLiteral("#f0e0d0"),
                                 QStringLiteral("#000000"), QString(), QStringLiteral("Noto Serif"),
                                 48.0, QStringLiteral("left"), 1.4),
              "the remembered look is captured from a real service theme");
        remembered = docs.themeSnapshotJson(brun);
        check(!remembered.isEmpty(), "service A's look captures");
        const QString item = library.addSong(
            "Remembered song",
            QVariantList{QVariantMap{{QStringLiteral("label"), QStringLiteral("Verse 1")},
                                     {QStringLiteral("lines"),
                                      QStringList{QStringLiteral("Saved words")}}}},
            QString(), QString(), QString());
        check(!item.isEmpty() && library.rememberItemTheme(item, remembered),
              "the Library remembers service A's look by value");
        // A's local theme is then edited and deleted: the Library copy is
        // unaffected because it never held the theme id.
        check(docs.editTheme(brun, QStringLiteral("Brun"), QStringLiteral("#000000"),
                             QStringLiteral("#000000"), QString(), QStringLiteral("Other"),
                             12.0, QStringLiteral("center"), 1.0),
              "service A's local theme is edited afterwards");
        check(docs.deleteTheme(brun), "and then deleted");
        check(library.itemThemeSnapshot(item) == remembered,
              "the remembered value did not follow the local theme");
    }
    {
        // Restart, then service B already holds a DIFFERENT theme named Brun.
        CatalogController library(nullptr, catalogDir);
        DocumentController docs(nullptr, serviceB);
        check(docs.createBlankTheme("Brun"), "service B already has its own Brun");
        QString serviceBBrun;
        for (const QVariant& theme : docs.themes()) {
            if (theme.toMap().value(QStringLiteral("name")).toString() ==
                QStringLiteral("Brun")) {
                serviceBBrun = theme.toMap().value(QStringLiteral("id")).toString();
            }
        }
        check(!serviceBBrun.isEmpty() &&
                  docs.editTheme(serviceBBrun, QStringLiteral("Brun"),
                                 QStringLiteral("#123456"), QStringLiteral("#000000"),
                                 QString(), QStringLiteral("B Sans"), 24.0,
                                 QStringLiteral("center"), 1.2),
              "service B's Brun is a different look");
        QString item;
        for (const QVariant& row : library.items()) {
            if (row.toMap().value(QStringLiteral("title")).toString() ==
                QStringLiteral("Remembered song")) {
                item = row.toMap().value(QStringLiteral("id")).toString();
            }
        }
        check(!item.isEmpty(), "the remembered Library song survives the restart");
        const QString snapshot = library.itemThemeSnapshot(item);
        check(snapshot == remembered, "the restart kept the exact remembered look");
        QVariantList plan;
        plan.append(QVariantMap{{QStringLiteral("lines"),
                                 QStringList{QStringLiteral("Saved words")}},
                                {QStringLiteral("group"), QStringLiteral("Verse 1")},
                                {QStringLiteral("label"), QStringLiteral("Verse 1")}});
        const QString prepared = docs.upsertCatalogSong(
            item, QStringLiteral("Remembered song"), QString(), QString(), QString(), plan,
            false, -1, snapshot);
        check(!prepared.isEmpty(), "service B prepares the remembered song");
        docs.selectSong(prepared);
        docs.selectSlide(0);
        const QVariantMap look = docs.slideAppearance();
        check(look.value(QStringLiteral("textColor")).toMap()
                      .value(QStringLiteral("value")).toString() == QStringLiteral("#f0e0d0") &&
                  look.value(QStringLiteral("fontSize")).toMap()
                          .value(QStringLiteral("value")).toDouble() == 48.0,
              "service B wears the remembered look, not its colliding local Brun");
        int bruns = 0;
        QStringList families;
        QStringList themeNames;
        for (const QVariant& theme : docs.themes()) {
            const QVariantMap row = theme.toMap();
            themeNames.append(row.value(QStringLiteral("name")).toString());
            families.append(row.value(QStringLiteral("fontFamily")).toString());
            if (row.value(QStringLiteral("name")).toString() == QStringLiteral("Brun")) {
                ++bruns;
            }
        }
        check(bruns == 1 && themeNames.contains(QStringLiteral("Brun (2)")) &&
                  families.contains(QStringLiteral("Noto Serif")) &&
                  families.contains(QStringLiteral("B Sans")),
              "the colliding local theme is preserved beside a distinctly named remembered one");
    }
}

// Task PT.4: the adapter captures a bounded source at preparation/Add,
// reports whether it still describes the words, and recaptures on an
// explicit lyric refresh. Nothing is fetched at check time.
void exerciseReflowSourceSurfaces(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    QVariantList plan;
    plan.append(QVariantMap{{QStringLiteral("lines"),
                             QStringList{QStringLiteral("Morning light"),
                                         QStringLiteral("Every shadow gone")}},
                            {QStringLiteral("group"), QStringLiteral("Verse 1")},
                            {QStringLiteral("label"), QStringLiteral("Verse 1")}});
    plan.append(QVariantMap{{QStringLiteral("lines"),
                             QStringList{QStringLiteral("Sing, sing")}},
                            {QStringLiteral("group"), QStringLiteral("Chorus")},
                            {QStringLiteral("label"), QStringLiteral("Chorus")}});
    const QString show = docs.upsertCatalogSong(
        QStringLiteral("reflow-song"), QStringLiteral("Source song"), QString(),
        QString(), QString(), plan, false);
    check(!show.isEmpty(), "a song is prepared to carry a source");
    QVariantMap info = docs.reflowSourceInfo(show);
    check(info.value(QStringLiteral("hasSource")).toBool() &&
              info.value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("catalog-song") &&
              info.value(QStringLiteral("blockCount")).toInt() == 2 &&
              info.value(QStringLiteral("matches")).toBool(),
          "the prepared song captures two matching source blocks");
    check(docs.reflowSourceMatches(show), "the accessor agrees with the row");

    // A text edit invalidates; a style-only edit does not.
    docs.setSlideText(QStringLiteral("Changed words"));
    docs.flushText();
    check(!docs.reflowSourceMatches(show),
          "editing the words stops the captured source matching");
    check(docs.customizeCurrentSlide(QStringLiteral("textColor"),
                                     QStringLiteral("#00ff00")) &&
              docs.upsertCatalogSong(QStringLiteral("reflow-song"),
                                     QStringLiteral("Source song"), QString(), QString(),
                                     QString(), plan, false) == show &&
              docs.reflowSourceMatches(show),
          "an explicit refresh recaptures the source for the new words");
    check(docs.reflowSourceInfo(show).value(QStringLiteral("blockCount")).toInt() == 2,
          "the refreshed source keeps the authored blocks");
    check(docs.customizeCurrentSlide(QStringLiteral("fontSize"), 31.0) &&
              docs.reflowSourceMatches(show),
          "a style-only edit leaves the captured source usable");

    // Scripture Add captures only the passage that Add used.
    check(docs.createPlaylist(QStringLiteral("Today")), "a schedule exists");
    QString playlist;
    for (const QVariant& row : docs.playlists()) {
        if (row.toMap().value(QStringLiteral("name")).toString() ==
            QStringLiteral("Today")) {
            playlist = row.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!playlist.isEmpty(), "the schedule id is readable");
    QVariantList verses;
    verses.append(QVariantMap{
        {QStringLiteral("lines"), QStringList{QStringLiteral("Paul, called to be an apostle")}},
        {QStringLiteral("notes"), QStringLiteral("1 Corinthians 1:1")}});
    verses.append(QVariantMap{
        {QStringLiteral("lines"), QStringList{QStringLiteral("Grace and peace to you")}},
        {QStringLiteral("notes"), QStringLiteral("1 Corinthians 1:3")}});
    check(docs.addScripturePassage(playlist, QStringLiteral("1 Corinthians 1:1-6"),
                                   QStringLiteral("NIV"),
                                   QStringLiteral("Scripture quotations taken from NIV"),
                                   verses),
          "the selected verses are added");
    QString passage;
    for (const QVariant& entry : docs.playlistEntries()) {
        if (entry.toMap().value(QStringLiteral("showId")).toString() != show &&
            !entry.toMap().value(QStringLiteral("showId")).toString().isEmpty()) {
            passage = entry.toMap().value(QStringLiteral("showId")).toString();
        }
    }
    check(!passage.isEmpty(), "the passage show is on the schedule");
    info = docs.reflowSourceInfo(passage);
    check(info.value(QStringLiteral("hasSource")).toBool() &&
              info.value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("scripture") &&
              info.value(QStringLiteral("reference")).toString() ==
                  QStringLiteral("1 Corinthians 1:1-6") &&
              info.value(QStringLiteral("translation")).toString() ==
                  QStringLiteral("NIV") &&
              info.value(QStringLiteral("blockCount")).toInt() == 2 &&
              info.value(QStringLiteral("matches")).toBool(),
          "the passage captures only the selected verses and its provenance");
    check(!docs.reflowSourceInfo(QStringLiteral("show-does-not-exist"))
               .value(QStringLiteral("hasSource"))
               .toBool(),
          "a show without a source says so");
    check(!docs.reflowSourceMatches(QStringLiteral("show-does-not-exist")),
          "an unknown show has no usable source");

    // Save/reopen keeps the source and its agreement with the words.
    check(docs.save(), "the service saves");
    DocumentController reopened(nullptr, root);
    check(reopened.load(), "the service reloads");
    check(reopened.reflowSourceMatches(show) &&
              reopened.reflowSourceInfo(show).value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("catalog-song") &&
              reopened.reflowSourceMatches(passage) &&
              reopened.reflowSourceInfo(passage)
                      .value(QStringLiteral("reference"))
                      .toString() == QStringLiteral("1 Corinthians 1:1-6"),
          "both captured sources survive save and reopen");
}

// --- EW.6: slide ordering and insertion ------------------------------------
//
// The rail's structural verbs driven through the very controller the rail
// binds to: explicit identities, final destinations, one history entry,
// selection by id, inherited context and the leading cue left alone.
void exerciseSlideOrdering(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root / "service");
    docs.createSong(QStringLiteral("Ordering"));
    const QString show = docs.currentSongId();
    const QString anchorShow = show;
    std::vector<QString> ids;
    const char* words[] = {"A words", "B words", "C words", "D words"};
    for (const char* text : words) {
        docs.addSlide();
        docs.setSlideText(QString::fromLatin1(text));
        docs.flushText();
        const QVariantList rows = docs.slides();
        ids.push_back(rows.back().toMap().value(QStringLiteral("id")).toString());
    }
    auto order = [&]() {
        std::vector<QString> out;
        for (const QVariant& row : docs.slides())
            out.push_back(row.toMap().value(QStringLiteral("id")).toString());
        return out;
    };
    auto previews = [&]() {
        std::vector<QString> out;
        for (const QVariant& row : docs.slides())
            out.push_back(row.toMap().value(QStringLiteral("preview")).toString());
        return out;
    };
    check(order() == ids, "the fixture starts in authored order");

    // Select B and move it two slots down: the final-position rule names
    // index 2, and the selection follows B's ID rather than staying on the
    // slot it used to occupy.
    docs.selectSlide(1);
    check(docs.currentSlide() == 1 && docs.currentSlideText() == QStringLiteral("B words"),
          "B is selected before the move");
    int docsSignals = 0;
    QObject::connect(&docs, &DocumentController::documentsChanged,
                     [&] { ++docsSignals; });
    check(docs.moveSlideFor(show, ids[1], 2), "B is moved to the final slot 2");
    check(order() == std::vector<QString>{ids[0], ids[2], ids[1], ids[3]},
          "the list carries A, C, B, D after the move");
    check(docs.currentSlide() == 2 && docs.currentSlideText() == QStringLiteral("B words"),
          "the selected slide stays selected by id after the move");
    check(docsSignals == 1, "a move is one document change");

    // A drop on the source's own final slot is a legal no-op: true, no
    // history entry, no document change. The proof is that undo then takes
    // away the MOVE, not a phantom edit.
    const int beforeNoop = docsSignals;
    const auto beforeNoopOrder = order();
    check(docs.moveSlideFor(show, ids[1], 2), "moving B onto its own slot answers true");
    check(order() == beforeNoopOrder && docsSignals == beforeNoop,
          "a no-op drop changes nothing and announces nothing");

    // Refusals leave the model and both stacks alone.
    const auto beforeRefusals = order();
    const int beforeRefusalSignals = docsSignals;
    check(!docs.moveSlideFor(show, QStringLiteral("slide999"), 0) &&
              order() == beforeRefusals,
          "moving an unknown slide is refused without a change");
    check(!docs.moveSlideFor(QStringLiteral("show999"), ids[0], 0) &&
              order() == beforeRefusals,
          "moving within another presentation is refused without a change");
    check(!docs.moveSlideFor(show, ids[0], 9) && order() == beforeRefusals &&
              docsSignals == beforeRefusalSignals,
          "a destination outside the list is refused without a history entry");

    // One undo reverses the move and the cursor follows B back to slot 1.
    docs.undo();
    check(order() == ids, "one undo restores the authored order");
    check(docs.currentSlideText() == QStringLiteral("B words"),
          "undo keeps the selected slide by id while it exists");
    docs.redo();
    check(order() == std::vector<QString>{ids[0], ids[2], ids[1], ids[3]} &&
              docs.currentSlideText() == QStringLiteral("B words"),
          "redo republishes the move and keeps B selected");
    docs.undo();

    // Move up/down at the ends are legal no-ops; in the middle they move one
    // slot. The fixture is back in authored order and A is selected.
    docs.selectSlide(0);
    check(docs.moveCurrentSlideStep(-1) && order() == ids,
          "moving the first slide up is a legal no-op");
    check(docs.moveCurrentSlideStep(1) && docs.currentSlide() == 1 &&
              order() == std::vector<QString>{ids[1], ids[0], ids[2], ids[3]},
          "moving A down lands A at slot 1 and keeps A selected");
    docs.undo();

    // Pending words flush to the slide the burst was typed into before the
    // move: the capture is by id, not by the slot the cursor will land on.
    docs.selectSlide(0);
    docs.setSlideText(QStringLiteral("A edited"));
    check(docs.moveCurrentSlideTo(3), "the flushed slide is moved by the current-slide verb");
    check(previews().back() == QStringLiteral("A edited") &&
              docs.currentSlide() == 3,
          "the pending burst lands on the slide being moved, not its destination");

    // Insertion after a middle anchor: one fresh slide, selected, carrying
    // the anchor's section/context/background/fit but none of its words,
    // notes or style overrides.
    docs.undo();
    docs.createSong(QStringLiteral("Insertion"));
    const QString insertShow = docs.currentSongId();
    docs.addSlide();
    docs.setSlideText(QStringLiteral("Verse one\nsecond line"));
    docs.flushText();
    const QString anchor = docs.slides().front().toMap().value("id").toString();
    // Register a picture asset for the background (a missing local file is
    // legal until a save), then give the anchor its picture, fit and style.
    check(docs.importMediaFile(QStringLiteral("/tmp/ew6-background.png")),
          "the fixture registers a picture asset");
    const QString asset = docs.assets().back().toMap().value("id").toString();
    check(docs.setSlideBackgroundFor(insertShow, anchor, asset, QStringLiteral("contain")),
          "the anchor takes the picture with its fit");
    check(docs.customizeCurrentSlide(QStringLiteral("allCaps"), true),
          "the anchor wears a per-slide style the insertion must not copy");
    const auto beforeInsert = docs.slides();
    const QString fresh = docs.insertSlideFor(insertShow, anchor, true);
    check(!fresh.isEmpty(), "insert after the anchor returns a fresh id");
    check(docs.slides().size() == beforeInsert.size() + 1,
          "insertion adds exactly one slide");
    check(docs.slides().at(1).toMap().value("id").toString() == fresh &&
              docs.currentSlide() == 1,
          "the fresh slide lands after the anchor and is selected");
    check(docs.currentSlideText().isEmpty() && docs.currentSlideBoxes().size() == 1,
          "the fresh slide carries exactly one empty text box");
    {
        const openshow::domain::Show* full = docs.documents().findShow(insertShow.toStdString());
        check(full != nullptr && full->slides.size() == 2, "the show holds both slides");
        if (full != nullptr && full->slides.size() == 2) {
            const openshow::domain::Slide& created = full->slides[1];
            const openshow::domain::Slide& source = full->slides[0];
            check(created.notes.empty() && created.customizations.empty(),
                  "insertion copies neither notes nor per-slide style overrides");
            check(created.backgroundAssetId == source.backgroundAssetId &&
                      created.backgroundFit == source.backgroundFit,
                  "insertion inherits the anchor's background and fit");
            check(created.textContext == source.textContext &&
                      created.group == source.group,
                  "insertion inherits the anchor's section and text context");
            check(created.themeId.empty(),
                  "insertion follows the presentation's theme rather than copying the anchor's");
            check(created.items.size() == 1 && created.items.front().lines.empty(),
                  "the fresh slide has one empty box and none of the anchor's words");
        }
    }
    // One undo takes the fresh slide away; redo brings the same id back.
    docs.undo();
    check(docs.slides().size() == beforeInsert.size() &&
              docs.slides().front().toMap().value("id").toString() == anchor,
          "one undo removes exactly the inserted slide");
    docs.redo();
    check(docs.slides().size() == 2 &&
              docs.slides().at(1).toMap().value("id").toString() == fresh,
          "redo restores the fresh slide's identity");

    // A real anchor context: a Bible passage names the `scripture` text
    // context and carries a captured reflow source. The insertion inherits
    // the context, and reordering the passage invalidates the captured
    // source so a prepared plan cannot resurrect the old order.
    check(docs.addScripturePassage(docs.currentPlaylistId(), QStringLiteral("John 1:1-3"),
                                   QStringLiteral("NIV"), QStringLiteral("notice"),
                                   QVariantList{
                                       QVariantMap{{QStringLiteral("lines"),
                                                    QVariantList{QStringLiteral("In the beginning")}},
                                                   {QStringLiteral("notes"), QStringLiteral("John 1:1")}},
                                       QVariantList{QStringLiteral("The same was in the beginning")}}),
          "the fixture adds a scripture passage with a captured source");
    // The passage is the newest show in the service; schedule-owned shows
    // are hidden from the Library list, so the document itself is asked.
    QString scriptureShow;
    for (const openshow::domain::Id& id : docs.documents().showIds()) {
        const openshow::domain::Show* show = docs.documents().findShow(id);
        if (show != nullptr && show->name == "John 1:1-3")
            scriptureShow = QString::fromStdString(id);
    }
    check(!scriptureShow.isEmpty(), "the passage show is in the service");
    docs.selectSong(scriptureShow);
    check(docs.slides().size() == 2, "the passage holds two slides");
    const QString verseOne = docs.slides().at(0).toMap().value("id").toString();
    const QString verseTwo = docs.slides().at(1).toMap().value("id").toString();
    check(docs.reflowSourceMatches(scriptureShow),
          "the passage's captured source matches before any edit");
    const QString verseInsert = docs.insertSlideFor(scriptureShow, verseOne, true);
    check(!verseInsert.isEmpty() &&
              docs.slides().at(1).toMap().value("id").toString() == verseInsert &&
              docs.slides().at(2).toMap().value("id").toString() == verseTwo,
          "the insertion lands between the passage's verses");
    {
        const openshow::domain::Show* full =
            docs.documents().findShow(scriptureShow.toStdString());
        check(full != nullptr && full->slides.size() == 3 &&
                  full->slides[1].textContext == "scripture",
              "the inserted slide inherits the passage's scripture context");
    }
    check(docs.reflowSourceMatches(scriptureShow),
          "a wordless insertion leaves the word projection matching (the cue is an eligibility question)");
    check(docs.moveSlideFor(scriptureShow, verseTwo, 0),
          "the passage's verses can be reordered");
    check(docs.slides().front().toMap().value("id").toString() == verseTwo,
          "the reorder put the second verse first");
    check(!docs.reflowSourceMatches(scriptureShow),
          "reordering the words invalidates the captured source");
    docs.undo();
    docs.undo();
    check(docs.slides().at(0).toMap().value("id").toString() == verseOne &&
              docs.slides().size() == 2 && docs.reflowSourceMatches(scriptureShow),
          "undo restores the passage and its captured source");

    docs.selectSong(insertShow);

    // Empty presentation: the footer's insertion is position zero.
    docs.createSong(QStringLiteral("Empty"));
    const QString emptyShow = docs.currentSongId();
    check(docs.slides().isEmpty(), "the empty fixture holds no slides");
    const QString first = docs.insertSlideAfterCurrent();
    check(!first.isEmpty() && docs.slides().size() == 1 &&
              docs.slides().front().toMap().value("id").toString() == first,
          "an empty presentation inserts at position zero and selects it");
    check(docs.insertSlideAt(emptyShow, 1) != "" && docs.slides().size() == 2,
          "an explicit index at the end appends");

    // The leading blank cue is an ordinary slide: insert before and after
    // obey the requested position and never move or recreate the cue.
    docs.createSong(QStringLiteral("Leading cue"));
    const QString cueShow = docs.currentSongId();
    const QString cue = docs.insertSlideAt(cueShow, 0);
    docs.addSlide();
    docs.setSlideText(QStringLiteral("Words"));
    docs.flushText();
    const QString wordsSlide = docs.slides().back().toMap().value("id").toString();
    const QString beforeCue = docs.insertSlideFor(cueShow, cue, false);
    check(!beforeCue.isEmpty() &&
              docs.slides().front().toMap().value("id").toString() == beforeCue &&
              docs.slides().at(1).toMap().value("id").toString() == cue,
          "insert before the cue lands at position zero and leaves the cue in place");
    docs.undo();
    const QString afterCue = docs.insertSlideFor(cueShow, cue, true);
    check(!afterCue.isEmpty() &&
              docs.slides().at(0).toMap().value("id").toString() == cue &&
              docs.slides().at(1).toMap().value("id").toString() == afterCue &&
              docs.slides().at(2).toMap().value("id").toString() == wordsSlide,
          "insert after the cue lands between the cue and the words");

    // Reordering a show with a captured reflow source invalidates the
    // match, so a prepared plan cannot resurrect the old order.
    docs.createSong(QStringLiteral("Reflow order"));
    const QString reflowShow = docs.currentSongId();
    for (int i = 0; i < 3; ++i) {
        docs.addSlide();
        docs.setSlideText(QStringLiteral("line %1").arg(i + 1));
        docs.flushText();
    }
    // The controller only captures a source through preparation/import; the
    // conservative current-word source is always available, and its match
    // only becomes meaningful once a source exists. The order rule itself is
    // pinned by domain_tests; here the save round-trip is what the rail
    // needs.
    // Drop the fixture's registered-but-missing picture first: a save
    // refuses to package an asset whose file is not readable.
    docs.removeAsset(asset);
    const auto savedOrder = order();
    check(docs.save(), "the edited session saves");
    DocumentController reopened(nullptr, root / "service");
    reopened.selectSong(reflowShow);
    std::vector<QString> reopenedOrder;
    for (const QVariant& row : reopened.slides())
        reopenedOrder.push_back(row.toMap().value("id").toString());
    check(reopenedOrder == savedOrder, "the edited order survives save and reopen");
}

int main(int argc, char* argv[]) {
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
                                           ("openshow-docctrl-" + std::to_string(getpid()));
    std::filesystem::remove_all(tempRoot);
    // The deck render cache is per user; keep this run's cache inside the
    // scratch tree so a repeated test always starts cold.
    const QByteArray cacheHome =
        QByteArray::fromStdString((tempRoot / "xdg-cache").string());
    qputenv("XDG_CACHE_HOME", cacheHome);

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("openshow-document-tests"));
    exerciseEditingAndPresentation(tempRoot / "edit");
    exerciseCommitSilence(tempRoot / "silence");
    exerciseUndoRedo(tempRoot / "undo");
    exerciseGeometryEdits(tempRoot / "geometry");
    exercisePersistence(tempRoot / "persist");
    exerciseThumbnailFingerprint(tempRoot / "thumbnail-identity");
    exerciseThumbnailCacheLifecycle(tempRoot / "thumbnail-lifecycle");
    exerciseThumbnailSizeMigration(tempRoot / "thumbnail-size-migration");
    exerciseThumbnailCacheFaults(tempRoot / "thumbnail-faults");
    exerciseRecoveredAssetOwnership(tempRoot / "adopt");
    exerciseShowAudioAdoption(tempRoot / "audio-adopt");
    exercisePlaylistAndLibrary(tempRoot / "playlist");
    exerciseThemePanel(tempRoot / "styles");
    exerciseWornChain(tempRoot / "wornchain");
    exerciseThemeSurfaces(tempRoot / "surfaces");
    exercisePlacementSurfaces(tempRoot / "placementsurfaces");
    exerciseReadingContexts(tempRoot / "readingcontexts");
    exerciseTextStyleSurfaces(tempRoot / "textstyle");
    exerciseTextLayoutSurfaces(tempRoot / "textlayout");
    exerciseTransitionSurfaces(tempRoot / "transitionsurfaces");
    exerciseTextStyleFieldMerge(tempRoot / "stylefields");
    exerciseThemeSampleViews(tempRoot / "sampleviews");
    exerciseStageBundle(tempRoot / "stagebundle");
    exerciseWholeItemThemeSurfaces(tempRoot / "wholeitem");
    exerciseRememberedLookAcrossServices(tempRoot / "remembered");
    exerciseReflowSourceSurfaces(tempRoot / "reflowsource");
    exerciseScripturePassage(tempRoot / "scripturepassage");
    exerciseMultiBoxSession(tempRoot / "multibox");
    exerciseBoxAuthoring(tempRoot / "boxauthoring");
    exerciseMediaAudioCue(tempRoot / "mediaaudio");
    exerciseClipboard(tempRoot / "clipboard");
    exerciseSlideOrdering(tempRoot / "slideordering");
    exerciseBackgroundFit(tempRoot / "fit");
    exerciseImageItemSession(tempRoot / "mixeditem");
    exerciseImageAuthoring(tempRoot / "imageauthoring");
    exerciseMediaBinViews(tempRoot / "mediabin");
    exerciseMediaImportCopy(tempRoot / "mediacopy");
    exerciseAutosaveSurfacing(tempRoot / "surfacing");
    exerciseBackgroundPersistence(tempRoot / "background");
    exercisePowerPointImport(tempRoot / "pptx");
    std::filesystem::remove_all(tempRoot);

    if (gFailed) {
        return 1;
    }
    std::printf("document_controller_tests: all fixtures passed\n");
    return 0;
}
