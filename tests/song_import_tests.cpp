/*
 * The clipboard import session's regressions (tasks SI.1/SI.3/SI.4), with no
 * worker: the draft, editable metadata, the honest no-renderer failure, and
 * the rule that nothing reaches the catalog without a measured plan. The
 * real-worker happy path belongs to the page acceptance; this pins the parts
 * that must be true even when measurement cannot run.
 */

#include <QGuiApplication>
#include <QClipboard>
#include <QString>
#include <QVariantMap>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "CatalogController.h"
#include "DocumentController.h"
#include "SongImport.h"
#include "WorkerController.h"

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
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void setClipboard(const std::string& utf8) {
    QGuiApplication::clipboard()->setText(QString::fromUtf8(utf8.c_str()));
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("openshow-song-import-" + std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    openshow::ui::CatalogController catalog(nullptr, root);
    openshow::ui::DocumentController documents(nullptr, root / "service");
    openshow::ui::WorkerController worker;  // deliberately not started
    openshow::ui::SongImport session(&catalog, &documents, &worker);

    check(!session.hasDraft() && session.slides().isEmpty(),
          "a fresh session has no draft");
    check(session.title().isEmpty() && session.ccli().isEmpty() &&
              session.copyright().isEmpty(),
          "and no metadata");

    const std::string morning = readFile(OPENSHOW_SOURCE_DIR
                                         "/docs/fixtures/songs/morning-light.txt");
    setClipboard(morning);
    session.pasteFromClipboard();
    check(session.hasDraft(), "a paste produces a draft");
    check(session.title() == QStringLiteral("Morning Light"),
          "the title suggestion reaches the editable field");
    check(session.ccli() == QStringLiteral("9000001"),
          "the CCLI number reaches its field");
    check(session.authors() ==
              QStringLiteral("Synthetic Example Writer, Second Example Writer"),
          "the author line reaches its field");
    check(session.copyright().contains(QString::fromUtf8("\xc2\xa9 2024 Example Music")),
          "the copyright block reaches its field");
    check(session.slides().isEmpty(),
          "no slides exist without a renderer");
    check(!session.canAdd(), "Add stays asleep without a measured plan");
    check(session.status().contains(QStringLiteral("renderer")),
          "the failure names the missing renderer");
    check(catalog.songCount() == 0,
          "a paste writes nothing to the Library");

    check(!session.addToLibrary(), "Add refuses with no measured plan");
    check(catalog.songCount() == 0, "the refused Add wrote nothing");

    // A second paste replaces the draft instead of stacking it.
    const std::string simple = readFile(OPENSHOW_SOURCE_DIR
                                        "/docs/fixtures/songs/simple-praise.txt");
    setClipboard(simple);
    session.pasteFromClipboard();
    check(session.hasDraft(), "a replacement paste keeps a draft");
    check(session.title() == QStringLiteral("Simple Praise"),
          "the replacement's title is the suggestion");
    check(session.ccli().isEmpty() && session.authors().isEmpty(),
          "a footer-less song clears the previous attribution");
    check(catalog.songCount() == 0,
          "the replacement paste wrote nothing to the Library");

    // An empty clipboard is refused and the existing draft survives: there
    // was nothing to replace it with, and wiping the operator's work because
    // they copied a blank line is not a service.
    setClipboard("   \n\t");
    session.pasteFromClipboard();
    check(session.hasDraft(), "an empty clipboard keeps the existing draft");
    check(session.status().contains(QStringLiteral("clipboard has no text")),
          "the empty-clipboard refusal says why");
    check(session.title() == QStringLiteral("Simple Praise"),
          "the surviving draft keeps its title");

    // Cancel discards everything and writes nothing.
    setClipboard(morning);
    session.pasteFromClipboard();
    session.cancel();
    check(!session.hasDraft() && session.slides().isEmpty() &&
              session.title().isEmpty(),
          "Cancel discards the whole draft");
    check(catalog.songCount() == 0, "Cancel wrote nothing");

    // A title-less paste can be measured later, but Add needs a title. With
    // no renderer it never gets that far; the point here is that the title
    // field write itself is harmless and writes nothing.
    setClipboard("[Verse]\nJust words\n");
    session.pasteFromClipboard();
    check(session.title().isEmpty(), "a label-first paste suggests no title");
    session.setTitle("Typed Title");
    check(session.title() == QStringLiteral("Typed Title"),
          "the operator can type a title");
    check(catalog.songCount() == 0, "typing a title writes nothing");

    std::filesystem::remove_all(root, ignored);
    if (gFailed) {
        std::fprintf(stderr, "song_import_tests: FAILED\n");
        return 1;
    }
    std::printf("song_import_tests: all checks passed\n");
    return 0;
}
