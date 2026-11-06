#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include "CatalogController.h"
#include <cstdio>
#include "DocumentController.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir scratch;
    openshow::ui::DocumentController docs(nullptr, scratch.path().toStdString());
    bool ok = true;
    auto check = [&](bool value, const char* message) {
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ok = false; }
    };
    docs.createSong("Repeated presentation");
    const auto a = docs.currentSongId();
    docs.addSlide();
    docs.setSlideText("Keep this service content");
    docs.flushText();
    docs.addShowToPlaylist(a);
    docs.addShowToPlaylist(a);
    docs.createSong("Other presentation");
    docs.addShowToPlaylist(docs.currentSongId());
    const auto before = docs.playlistEntries();
    const auto first = before[0].toMap().value("entryId").toString();
    const auto middle = before[1].toMap().value("entryId").toString();
    const auto last = before[2].toMap().value("entryId").toString();
    check(docs.removePlaylistEntries({first, last, first}), "batch removes unique selected entry identities");
    check(docs.playlistEntries().size() == 1
          && docs.playlistEntries()[0].toMap().value("entryId") == middle,
          "another occurrence of the same presentation stays in place");
    check(docs.songs().size() == 2, "service removal preserves Library sources");
    docs.undo();
    check(docs.playlistEntries() == before, "one undo restores the entire batch in its original order");
    check(!docs.removePlaylistEntries({first, "missing"}) && docs.playlistEntries() == before,
          "a stale identity refuses the whole batch without removing a valid entry");
    docs.redo();
    check(docs.playlistEntries().size() == 1, "refusal preserves redo history");
    const auto serviceRows = docs.playlistEntries();
    const auto sourceRows = docs.songs();
    const auto b = sourceRows[1].toMap().value("id").toString();
    const auto savedSlides = docs.slidesOf(a);
    check(docs.removeLibraryShows({a, b}), "remove referenced and unreferenced Library presentations together");
    check(docs.songs().isEmpty() && docs.playlistEntries() == serviceRows && docs.slidesOf(a) == savedSlides,
          "Library presentation removal retains prepared service entries and their content");
    check(docs.save(), "the retained service copy saves");
    {
        openshow::ui::DocumentController reopened(nullptr, scratch.path().toStdString());
        check(reopened.songs().isEmpty() && reopened.playlistEntries() == serviceRows
              && reopened.slidesOf(a) == savedSlides, "service-only presentation remains playable after reopening");
    }
    docs.undo();
    check(docs.songs() == sourceRows, "one undo restores both Library presentations");
    check(!docs.removeLibraryShows({a, "missing"}) && docs.songs() == sourceRows,
          "a stale Library presentation refuses without a partial removal");

    openshow::ui::CatalogController library(nullptr, scratch.path().toStdString());
    const QVariantList stanzas{QVariantMap{{"label", "Verse"}, {"lines", QVariantList{"Words"}}}};
    const auto firstSong = library.addSong("First", stanzas, "", "", "");
    const auto secondSong = library.addSong("Second", stanzas, "", "", "");
    library.moveItemToCategory(secondSong, "presentations");
    const auto catalogRows = library.items();
    auto manifestBytes = [&] {
        QFile file(library.directory() + "/catalog.json");
        file.open(QIODevice::ReadOnly);
        return file.readAll();
    };
    const auto savedCatalog = manifestBytes();
    check(!library.removeItems({firstSong, "missing"}) && library.items() == catalogRows
          && manifestBytes() == savedCatalog, "a stale catalog identity leaves all rows and durable bytes intact");
    qputenv("OPENSHOW_CATALOG_FAULT", "write");
    const bool refused = !library.removeItems({firstSong, secondSong});
    qunsetenv("OPENSHOW_CATALOG_FAULT");
    check(refused && library.items() == catalogRows && manifestBytes() == savedCatalog,
          "a failed batch write changes neither catalog model nor file");
    QStringList removed;
    QObject::connect(&library, &openshow::ui::CatalogController::itemRemoved,
                     [&](const auto& id) { removed.append(id); });
    check(library.removeItems({firstSong, secondSong, firstSong}) && library.items().isEmpty()
          && removed.size() == 2, "catalog batch deletes each identity once after its durable write");
    library.undo();
    check(library.items() == catalogRows, "one Library undo restores the complete batch and categories");
    {
        openshow::ui::CatalogController reopened(nullptr, scratch.path().toStdString());
        check(reopened.items() == catalogRows, "the undone catalog batch survives restart");
    }
    // Confirmed Library deletions cannot reappear through any older whole-
    // document snapshot. Service copies and future service undo still work.
    openshow::ui::DocumentController permanent(nullptr, scratch.path().toStdString() + "/permanent-service");
    permanent.createSong("Retained service presentation");
    const auto retained = permanent.currentSongId();
    permanent.addSlide();
    permanent.setSlideText("Retained words");
    permanent.flushText();
    permanent.addShowToPlaylist(retained);
    permanent.addShowToPlaylist(retained);
    permanent.createSong("Unreferenced database presentation");
    const auto discarded = permanent.currentSongId();
    const auto retainedEntries = permanent.playlistEntries();
    check(!permanent.removeLibraryShows({retained, "missing"}, true)
              && permanent.songs().size() == 2, "stale permanent deletion is atomic");
    check(permanent.removeLibraryShows({retained, discarded}, true), "permanently delete both presentations");
    while (permanent.canUndo()) {
        permanent.undo();
        check(permanent.songs().isEmpty(), "older service undo cannot resurrect deleted Library presentations");
    }
    while (permanent.canRedo()) {
        permanent.redo();
        check(permanent.songs().isEmpty(), "older service redo cannot resurrect deleted Library presentations");
    }
    check(permanent.playlistEntries() == retainedEntries && !permanent.slidesOf(retained).isEmpty()
              && permanent.showInfo(discarded).isEmpty(),
          "history retains prepared service copies and discards unreferenced deleted presentations");
    permanent.removePlaylistEntries({retainedEntries.first().toMap().value("entryId").toString()});
    permanent.undo();
    check(permanent.playlistEntries() == retainedEntries,
          "service deletion remains undoable after permanent Library deletion");
    check(permanent.save(), "permanent presentation deletion saves");
    {
        openshow::ui::DocumentController reopened(nullptr, scratch.path().toStdString() + "/permanent-service");
        check(reopened.songs().isEmpty() && reopened.playlistEntries() == retainedEntries
                  && !reopened.slidesOf(retained).isEmpty() && reopened.showInfo(discarded).isEmpty(),
              "restart preserves permanent deletion and playable service copies");
    }
    check(library.removeItems({firstSong, secondSong}, true), "permanently delete catalog items");
    while (library.canUndo()) {
        library.undo();
        check(library.items().isEmpty(), "older catalog undo cannot resurrect permanently deleted records");
    }
    while (library.canRedo()) {
        library.redo();
        check(library.items().isEmpty(), "older catalog redo cannot resurrect permanently deleted records");
    }
    {
        openshow::ui::CatalogController reopened(nullptr, scratch.path().toStdString());
        check(reopened.items().isEmpty(), "permanent catalog deletion remains durable after restart");
    }
    return ok ? 0 : 1;
}
