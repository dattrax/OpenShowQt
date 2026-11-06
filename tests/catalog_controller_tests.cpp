/*
 * The Library adapter's regressions (tasks SL.2 and SL.5): durable edits,
 * restart survival, title-only filtering, category moves, independent
 * history and the honest failures. Qt Core only; no display and no worker.
 */

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "CatalogController.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::ui::CatalogController;

std::filesystem::path scratchDirectory() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openshow-catalog-controller-" + std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    return path;
}

QVariantList stanzas(const QStringList& lines, const QString& label = QStringLiteral("Verse 1")) {
    QVariantMap stanza;
    stanza.insert(QStringLiteral("label"), label);
    stanza.insert(QStringLiteral("lines"), lines);
    return QVariantList{stanza};
}

QStringList titlesOf(const QVariantList& rows) {
    QStringList titles;
    for (const QVariant& row : rows) {
        titles.append(row.toMap().value(QStringLiteral("title")).toString());
    }
    return titles;
}

QStringList idsOf(const QVariantList& rows) {
    QStringList ids;
    for (const QVariant& row : rows) {
        ids.append(row.toMap().value(QStringLiteral("id")).toString());
    }
    return ids;
}

void testDurableAddAndRestart() {
    const std::filesystem::path dir = scratchDirectory();
    {
        CatalogController library(nullptr, dir);
        check(library.items().isEmpty(), "a fresh Library is empty");
        check(library.songCount() == 0 && library.presentationCount() == 0,
              "both categories start empty");
        check(!library.canUndo() && !library.canRedo(),
              "no history before an edit");

        const QString id = library.addSong(
            "Goodbye Yesterday", stanzas({"Goodbye yesterday", "Living in the light"}),
            "7231931", "Example Writer", "\u00a9 2024 Example");
        check(!id.isEmpty(), "a valid song is added");
        check(library.songCount() == 1, "the Songs branch counts it");
        check(library.items().size() == 1, "the row list has it");
        check(library.canUndo(), "the add is undoable");
        const QVariantMap row = library.items().first().toMap();
        check(row.value(QStringLiteral("ccli")).toString() == "7231931",
              "the CCLI number is exposed");
        check(row.value(QStringLiteral("authors")).toString() == "Example Writer",
              "the author field is exposed");
        check(row.value(QStringLiteral("copyright")).toString().contains(
                  QString(QChar(0x00a9))),
              "the copyright field is exposed");
        check(row.value(QStringLiteral("stanzaCount")).toInt() == 1,
              "stanza count is exposed");
        check(row.value(QStringLiteral("lineCount")).toInt() == 2,
              "line count is exposed");

        check(std::filesystem::is_regular_file(dir / "catalog" / "catalog.json"),
              "the add is durable immediately");
    }
    {
        // Restart: a new adapter over the same directory sees the song.
        CatalogController reopened(nullptr, dir);
        check(reopened.songCount() == 1, "the song survives a restart");
        check(titlesOf(reopened.items()) == QStringList{"Goodbye Yesterday"},
              "the title survives a restart");
        check(!reopened.canUndo(),
              "the undo stack itself does not need to survive a restart");
        check(reopened.items().first().toMap()
                  .value(QStringLiteral("ccli"))
                  .toString() == "7231931",
              "the CCLI number survives a restart");
    }
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

void testTitleOnlyFiltering() {
    const std::filesystem::path dir = scratchDirectory();
    CatalogController library(nullptr, dir);
    const QString jesusLovesMe =
        library.addSong("Jesus Loves Me", stanzas({"Jesus loves me"}), {}, "", "");
    const QString allHail =
        library.addSong("All Hail King Jesus", stanzas({"All hail the king"}), {}, "", "");
    // This title does NOT contain 'Jesus'; its lyrics do. The Library
    // filter is a title filter, so it must not appear.
    const QString lyricOnly = library.addSong(
        "Morning Song", stanzas({"Jesus is the reason we sing"}), {}, "", "");
    library.addSong("Amazing Grace", stanzas({"Amazing grace"}), {}, "", "");
    library.addSong("Jes\u00fas, Lover of My Soul",
                    stanzas({"Jesus, lover of my soul"}), {}, "", "");

    check(titlesOf(library.searchTitles("Jesus")) ==
              (QStringList{"Jesus Loves Me", "All Hail King Jesus",
                           "Jes\u00fas, Lover of My Soul"}),
          "Jesus matches titles only, in creation order, accents normalized");
    check(!idsOf(library.searchTitles("Jesus")).contains(lyricOnly),
          "a lyrics-only hit is excluded");
    check(titlesOf(library.searchTitles("jesus hail")) ==
              QStringList{"All Hail King Jesus"},
          "every query word must match");
    check(titlesOf(library.searchTitles("JESUS")).size() == 3,
          "the filter is case-insensitive");
    check(titlesOf(library.searchTitles("  ")).size() == 5,
          "a blank query is the whole Library");
    check(library.searchTitles("nothing here").isEmpty(),
          "no match is an empty list");
    check(idsOf(library.searchTitles("Jesus")).contains(jesusLovesMe),
          "the first match is still listed");
    check(idsOf(library.searchTitles("Jesus")).contains(allHail),
          "the second match is still listed");
    check(library.songCount() == 5,
          "filtering never changed the catalog");

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

void testMovesUndoAndDuplicates() {
    const std::filesystem::path dir = scratchDirectory();
    CatalogController library(nullptr, dir);
    const QString first =
        library.addSong("Notices", stanzas({"Welcome"}), {}, "", "");
    const QString second =
        library.addSong("Notices", stanzas({"Welcome"}), {}, "", "");
    check(first != second, "duplicate titles are distinct items");
    check(library.songCount() == 2, "both duplicates are in Songs");

    check(library.moveItemToCategory(first, "presentations"),
          "an item moves to Presentations");
    check(library.songCount() == 1 && library.presentationCount() == 1,
          "both branches count correctly");
    check(library.itemOf(first).value(QStringLiteral("contentRevision")).toULongLong() == 1,
          "a category move preserves the content revision");
    check(library.itemOf(first).value(QStringLiteral("recordRevision")).toULongLong() == 2,
          "a category move advances the record revision");
    check(library.stanzasOf(first).size() == 1,
          "a category move preserves the words");
    check(library.canUndo(), "history still has entries");
    check(library.moveItemToCategory(first, "presentations"),
          "repeating a move succeeds");
    library.undo();
    check(library.songCount() == 2 && library.presentationCount() == 0,
          "undo returns the item to Songs (the repeated move added no step)");
    library.redo();
    check(library.songCount() == 1 && library.presentationCount() == 1,
          "redo moves it again");
    check(!library.moveItemToCategory(first, "folders"),
          "an unknown category is refused");
    check(library.status().contains("Unknown Library category"),
          "the refusal names the category");
    check(library.songCount() == 1 && library.presentationCount() == 1,
          "the refusal changed nothing");

    library.removeItem(first);
    check(library.presentationCount() == 0, "remove works");
    check(library.songCount() == 1, "the duplicate is untouched");
    library.undo();
    check(library.presentationCount() == 1, "remove is undoable");
    check(!library.removeItem("item999"), "removing an unknown item is refused");

    // A restart shows the durable undo result (redo's move), not the
    // original in-memory arrangement.
    CatalogController reopened(nullptr, dir);
    check(reopened.songCount() == 1 && reopened.presentationCount() == 1,
          "the undone/redone arrangement is what the file holds");

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

void testWriteFailureChangesNothing() {
    const std::filesystem::path dir = scratchDirectory();
    CatalogController library(nullptr, dir);
    library.addSong("Original", stanzas({"one"}), {}, "", "");
    const std::filesystem::path manifest = dir / "catalog" / "catalog.json";

    std::ifstream before(manifest, std::ios::binary);
    std::string beforeBytes((std::istreambuf_iterator<char>(before)),
                            std::istreambuf_iterator<char>());

    qputenv("OPENSHOW_CATALOG_FAULT", "write");
    const QString refused = library.addSong("Should not land", stanzas({"two"}),
                                            {}, "", "");
    qunsetenv("OPENSHOW_CATALOG_FAULT");
    check(refused.isEmpty(), "a faulted write refuses the add");
    check(library.songCount() == 1, "the refused add is not published");
    check(library.items().size() == 1, "no row was published either");
    check(library.status().contains("simulated fault"),
          "the failure names its step");
    check(!library.canRedo(), "the failure pushed no redo entry");

    std::ifstream after(manifest, std::ios::binary);
    std::string afterBytes((std::istreambuf_iterator<char>(after)),
                           std::istreambuf_iterator<char>());
    check(beforeBytes == afterBytes, "the committed file is untouched");

    library.undo();  // the successful add is still undoable exactly once
    check(library.songCount() == 0, "the failed write did not consume history");

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

void testUnreadableCatalogIsNotOverwritten() {
    const std::filesystem::path dir = scratchDirectory();
    std::filesystem::create_directories(dir / "catalog");
    {
        std::ofstream out(dir / "catalog" / "catalog.json", std::ios::trunc);
        out << "{ this is not a catalog";
    }
    CatalogController library(nullptr, dir);
    check(library.items().isEmpty(), "an unreadable Library reads empty");
    check(library.status().contains("could not be read"),
          "the status says the Library could not be read");
    const QString id =
        library.addSong("Would overwrite", stanzas({"words"}), {}, "", "");
    check(id.isEmpty(), "a mutation is refused while the file is unreadable");
    check(library.status().contains("Fix or move"),
          "the refusal tells the operator what to do");
    {
        std::ifstream in(dir / "catalog" / "catalog.json", std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        check(bytes == "{ this is not a catalog",
              "the unreadable file was never overwritten");
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testDurableAddAndRestart();
    testTitleOnlyFiltering();
    testMovesUndoAndDuplicates();
    testWriteFailureChangesNothing();
    testUnreadableCatalogIsNotOverwritten();
    if (gFailed) {
        std::fprintf(stderr, "catalog_controller_tests: FAILED\n");
        return 1;
    }
    std::printf("catalog_controller_tests: all checks passed\n");
    return 0;
}
