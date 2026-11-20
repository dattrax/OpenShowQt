/*
 * The Library adapter's regressions (tasks SL.2 and SL.5): durable edits,
 * restart survival, title-only filtering, category moves, independent
 * history and the honest failures. Qt Core only; no display and no worker.
 */

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
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
            "Morning Light", stanzas({"Morning light is breaking", "Every shadow gone"}),
            "9000001", "Example Writer", "\u00a9 2024 Example");
        check(!id.isEmpty(), "a valid song is added");
        check(library.songCount() == 1, "the Songs branch counts it");
        check(library.items().size() == 1, "the row list has it");
        check(library.canUndo(), "the add is undoable");
        const QVariantMap row = library.items().first().toMap();
        check(row.value(QStringLiteral("ccli")).toString() == "9000001",
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
        check(titlesOf(reopened.items()) == QStringList{"Morning Light"},
              "the title survives a restart");
        check(!reopened.canUndo(),
              "the undo stack itself does not need to survive a restart");
        check(reopened.items().first().toMap()
                  .value(QStringLiteral("ccli"))
                  .toString() == "9000001",
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

// Task PT.2: the Library remembers a future-service look by value. It is
// durable, advances the revisions, never touches canonical words, is one
// Library history step, and announces itself through the separate
// itemThemeChanged signal so the service bridge cannot re-prepare a linked
// show and defeat an Apply-only choice.
void testRememberedLookAdapter() {
    const std::filesystem::path dir = scratchDirectory();
    const QString look = QStringLiteral(
        R"({"name":"Remembered","textColor":"#f0e0d0","fontFamily":"Noto Serif",)"
        R"("fontSize":48.0,"align":"left","lineSpacing":1.4,"fontWeight":700,)"
        R"("fontStyle":"italic","fontUnderline":true,"fitMode":"fixed",)"
        R"("minFontSize":18.0,"verticalAlign":"bottom","allCaps":true,)"
        R"("stroke":{"enabled":true,"color":"#ff0000","width":3.5},)"
        R"("shadow":{"enabled":true,"color":"#00ff00","opacity":0.4,)"
        R"("offsetX":-4.0,"offsetY":6.0,"blurSigma":2.5},)"
        R"("placement":{"x":120.0,"y":80.0,"width":1680.0,"height":900.0},)"
        R"("transition":{"kind":"cut","textDurationMs":0,"mediaDurationMs":10000},)"
        R"("contexts":[]})");

    QString id;
    {
        CatalogController library(nullptr, dir);
        id = library.addSong("Remembered", stanzas({"Original words", "Second line"}),
                             "9000002", "A Writer", "\u00a9 2024");
        check(!id.isEmpty(), "a song exists to remember a look for");
        const QVariantMap before = library.items().first().toMap();
        check(!before.value(QStringLiteral("hasThemeSnapshot")).toBool(),
              "a new song remembers nothing");

        int themeSignals = 0;
        int itemSignals = 0;
        QObject::connect(&library, &CatalogController::itemThemeChanged,
                         [&themeSignals](const QString&) { ++themeSignals; });
        QObject::connect(&library, &CatalogController::itemChanged,
                         [&itemSignals](const QString&) { ++itemSignals; });
        check(library.rememberItemTheme(id, look),
              "the Library remembers the chosen look");
        const QVariantMap after = library.items().first().toMap();
        check(after.value(QStringLiteral("hasThemeSnapshot")).toBool(),
              "the row reports that it remembers a look");
        check(after.value(QStringLiteral("contentRevision")).toULongLong() ==
                      before.value(QStringLiteral("contentRevision")).toULongLong() + 1 &&
                  after.value(QStringLiteral("recordRevision")).toULongLong() ==
                      before.value(QStringLiteral("recordRevision")).toULongLong() + 1,
              "a real preference change advances both revisions");
        check(after.value(QStringLiteral("stanzaCount")).toInt() ==
                      before.value(QStringLiteral("stanzaCount")).toInt() &&
                  after.value(QStringLiteral("lineCount")).toInt() ==
                      before.value(QStringLiteral("lineCount")).toInt() &&
                  after.value(QStringLiteral("title")).toString() == QLatin1String("Remembered"),
              "the canonical words and title are untouched");
        check(!library.itemThemeSnapshot(id).isEmpty(),
              "the remembered look reads back as codec JSON");
        check(themeSignals == 1 && itemSignals == 0,
              "the preference announces itself without the content signal");

        check(library.rememberItemTheme(id, look) &&
                  library.items().first().toMap()
                          .value(QStringLiteral("contentRevision"))
                          .toULongLong() ==
                      after.value(QStringLiteral("contentRevision")).toULongLong(),
              "remembering the same look again costs no revision");

        const QString invalid = QStringLiteral("{not json");
        check(!library.rememberItemTheme(id, invalid),
              "a malformed snapshot is refused");
        check(library.items().first().toMap()
                  .value(QStringLiteral("hasThemeSnapshot")).toBool(),
              "the refusal kept the previous preference");
        check(!library.rememberItemTheme(QStringLiteral("item-does-not-exist"), look),
              "an unknown item cannot remember a look");

        library.undo();
        check(library.items().first().toMap()
                      .value(QStringLiteral("hasThemeSnapshot"))
                      .toBool() == false,
              "one Library undo forgets the look");
        library.redo();
        check(library.items().first().toMap()
                  .value(QStringLiteral("hasThemeSnapshot"))
                  .toBool(),
              "Library redo remembers it again");
    }
    {
        // Restart: the preference is durable and the words are canonical.
        CatalogController reopened(nullptr, dir);
        check(reopened.items().size() == 1 &&
                  reopened.items().first().toMap()
                      .value(QStringLiteral("hasThemeSnapshot"))
                      .toBool(),
              "the remembered look survives a restart");
        check(!reopened.itemThemeSnapshot(id).isEmpty(),
              "the codec JSON is readable after a restart");
        check(reopened.items().first().toMap().value(QStringLiteral("lineCount")).toInt() == 2,
              "the words survive the restart unchanged");
        check(reopened.forgetItemTheme(id) &&
                  reopened.items().first().toMap()
                          .value(QStringLiteral("hasThemeSnapshot"))
                          .toBool() == false &&
                  reopened.itemThemeSnapshot(id).isEmpty(),
              "the adapter can forget the look durably");
        reopened.undo();
        check(reopened.items().first().toMap()
                  .value(QStringLiteral("hasThemeSnapshot"))
                  .toBool(),
              "the forget is one Library history step");
    }
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

void testCoalescedThumbnailVariants() {
    const auto dir = scratchDirectory();
    QString id;
    auto variant = [](int index) {
        openshow::domain::ThumbnailCacheEntry entry;
        entry.fingerprint = openshow::domain::sha256Hex("look-" + std::to_string(index));
        entry.renderer = openshow::domain::rendererIdentity();
        entry.width = 160; entry.height = 90;
        const std::string bytes(24, static_cast<char>('a' + index));
        entry.pngBytes = bytes.size();
        entry.checksum = openshow::domain::pngChecksum(bytes);
        entry.pngBase64 = openshow::domain::base64Encode(bytes);
        return entry;
    };
    const auto manifest = dir / "catalog" / "catalog.json";
    auto read = [&] {
        std::ifstream file(manifest, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    {
        CatalogController library(nullptr, dir);
        id = library.addSong("Cache writes", stanzas({"words"}), {}, {}, {});
        const auto before = library.itemOf(id);
        const auto plain = read();
        for (int i = 0; i < 8; ++i) {
            check(library.rememberThumbnailVariant(id, variant(i)), "a new Library variant is accepted");
        }
        check(read() == plain, "a burst of pictures performs no synchronous catalog writes");
        check(library.itemOf(id) == before, "cache arrivals change no authored fields or revisions");
        QElapsedTimer timer; timer.start();
        while (read() == plain && timer.elapsed() < 6000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        check(read() != plain, "the coalesced background write eventually becomes durable");
        CatalogController reopened(nullptr, dir);
        const auto variants = reopened.thumbnailVariantsFor(id);
        check(variants.size() == 6 && variants.front().fingerprint == variant(2).fingerprint &&
                  variants.back().fingerprint == variant(7).fingerprint,
              "the six newest prepared pictures survive a Library restart");
        library.rememberThumbnailVariant(id, variant(8));
        check(library.renameItem(id, "Authored edit"), "an authored edit serializes with the cache writer");
        library.rememberThumbnailVariant(id, variant(9));
    } // destruction joins the worker and flushes the final pending snapshot
    {
        CatalogController reopened(nullptr, dir);
        check(reopened.itemOf(id).value("title").toString() == "Authored edit",
              "a cache write cannot overwrite an authored edit");
        const auto variants = reopened.thumbnailVariantsFor(id);
        check(variants.size() == 6 && variants.back().fingerprint == variant(9).fingerprint,
              "shutdown persists the last coalesced picture");
        const auto written = std::filesystem::last_write_time(manifest);
        check(reopened.flushThumbnailVariants(nullptr) &&
                  std::filesystem::last_write_time(manifest) == written,
              "an unchanged cache flush does not rewrite or sync the catalog");
    }
    std::filesystem::remove_all(dir);
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
    testRememberedLookAdapter();
    testWriteFailureChangesNothing();
    testCoalescedThumbnailVariants();
    testUnreadableCatalogIsNotOverwritten();
    if (gFailed) {
        std::fprintf(stderr, "catalog_controller_tests: FAILED\n");
        return 1;
    }
    std::printf("catalog_controller_tests: all checks passed\n");
    return 0;
}
