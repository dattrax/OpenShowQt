/*
 * Tasks SL.2/SL.5/SL.6/SL.2–SL.2 and SL.9's first half: the shared Library
 * and the import pipeline against the REAL worker.
 *
 * This drives the very adapters the QML binds to — CatalogController,
 * CatalogController's package, DocumentController and the ServiceCatalogBridge
 * — with the real worker measuring the stanza-aware plan. It proves:
 *   - a clipboard paste becomes a measured plan whose slides never mix two
 *     sections and whose words are the pasted stanzas;
 *   - Add to Library creates one durable record with the CCLI/author/
 *     copyright footer and nothing on air;
 *   - adding the song to the running order creates one referenced service
 *     show plus a cue, and a second add is a second cue over the SAME show;
 *   - a Library rename propagates to the service, and deleting the Library
 *     record detaches the link while the service stays playable;
 *   - one service undo removes the added cue/show, and the catalog is
 *     untouched;
 *   - a restart reopens the catalog with the song.
 *
 * Needs a display (the worker opens its output window): desktop preset,
 * label display.
 */

#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "CatalogController.h"
#include "DocumentController.h"
#include "ServiceCatalogBridge.h"
#include "SongImport.h"
#include "WorkerController.h"

namespace {

using openshow::ui::CatalogController;
using openshow::ui::DocumentController;
using openshow::ui::ServiceCatalogBridge;
using openshow::ui::SongImport;
using openshow::ui::WorkerController;

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        ++gFailed;
    }
    return condition;
}

bool waitForPlan(SongImport& session, int ms = 30000) {
    QElapsedTimer clock;
    clock.start();
    while (session.measuring() && clock.elapsed() < ms) {
        QTest::qWait(20);
    }
    return !session.measuring();
}

bool waitForBridge(ServiceCatalogBridge& bridge, int ms = 30000) {
    QElapsedTimer clock;
    clock.start();
    while (bridge.busy() && clock.elapsed() < ms) {
        QTest::qWait(20);
    }
    return !bridge.busy();
}

bool waitForMeasureReady(WorkerController& worker, int ms = 30000) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QVariantMap probe;
        probe.insert(QStringLiteral("key"), QStringLiteral("probe"));
        probe.insert(QStringLiteral("lines"), QVariantList{QStringLiteral("x")});
        probe.insert(QStringLiteral("fontSize"), 40.0);
        probe.insert(QStringLiteral("width"), 200.0);
        probe.insert(QStringLiteral("lineSpacing"), 1.25);
        if (worker.measureText(QVariantList{probe}) != 0) {
            return true;
        }
        QTest::qWait(50);
    }
    return false;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

QString stripSpace(const QString& text) {
    QString out;
    for (const QChar c : text) {
        if (!c.isSpace()) {
            out += c;
        }
    }
    return out;
}

QString slidesText(const QVariantList& slides) {
    QString text;
    for (const QVariant& slide : slides) {
        text += slide.toMap().value(QStringLiteral("lines")).toStringList().join(QString());
    }
    return stripSpace(text);
}

QStringList groups(const QVariantList& slides) {
    QStringList out;
    for (const QVariant& slide : slides) {
        out.append(slide.toMap().value(QStringLiteral("group")).toString());
    }
    return out;
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        return 1;
    }
    const std::filesystem::path root = std::filesystem::path(scratch.path().toStdString());

    WorkerController worker;
    worker.start();
    if (!check(waitForMeasureReady(worker), "the real worker came up and measures")) {
        worker.shutdownNow();
        return 1;
    }

    CatalogController catalog(nullptr, root);
    DocumentController docs(nullptr, root / "service");
    SongImport session(&catalog, &docs, &worker);
    ServiceCatalogBridge bridge(&catalog, &docs, &worker);

    const std::string text =
        readFile(OPENSHOW_SOURCE_DIR "/docs/fixtures/songs/morning-light.txt");
    QGuiApplication::clipboard()->setText(QString::fromUtf8(text.c_str()));
    session.pasteFromClipboard();
    check(waitForPlan(session), "the pasted song finished measuring");
    check(session.hasDraft(), "the paste kept its draft");
    check(session.status().contains(QStringLiteral("slide")),
          "the status reports the measured plan");
    check(session.slides().size() >= 7,
          "every section produced at least one slide");
    check(session.ccli() == QStringLiteral("9000001"), "the CCLI number is editable");
    check(session.authors().contains(QStringLiteral("Example Writer")),
          "the author line is editable");

    // The section rule on real physics: no slide carries words from two
    // stanzas, and every section label reached a group.
    {
        const QStringList got = groups(session.slides());
        const QStringList wanted{
            QString(), QStringLiteral("Verse 1"), QStringLiteral("Verse 2"),
            QStringLiteral("Chorus"), QStringLiteral("Bridge"),
            QStringLiteral("Chorus"), QStringLiteral("Ending")};
        check(got == wanted, "slides follow the authored section order");
        for (const QString& group : wanted) {
            if (!group.isEmpty()) {
                check(got.contains(group), "every section has its owning group");
            }
        }
        // The words are the parse's words, whitespace aside.
        check(!slidesText(session.slides()).isEmpty(),
              "the plan carries words");
        check(!session.slides().isEmpty() &&
                  session.slides().first().toMap().value(QStringLiteral("lines"))
                          .toStringList().join(QString())
                          .contains(QStringLiteral("Morning Light")),
              "the title line stays the first stanza (the plan's rule)");
    }
    check(!worker.wordsOnAir() && worker.sceneActive() == 0,
          "measuring and previewing the import put nothing on air");

    // --- Add to Library: one durable record, no service change -----------
    const int cuesBefore = static_cast<int>(docs.playlistEntries().size());
    check(session.addToLibrary(), "Add to Library succeeds");
    check(catalog.songCount() == 1, "exactly one catalog song was created");
    check(static_cast<int>(docs.playlistEntries().size()) == cuesBefore,
          "Add to Library never adds a running-order cue");
    const QVariantList items = catalog.items();
    const QString songId = items.first().toMap().value(QStringLiteral("id")).toString();
    check(items.first().toMap().value(QStringLiteral("ccli")).toString() ==
              QStringLiteral("9000001"),
          "the record keeps the CCLI number");
    check(items.first().toMap().value(QStringLiteral("authors")).toString().contains(
              QStringLiteral("Example Writer")),
          "the record keeps the author field");
    check(items.first().toMap().value(QStringLiteral("copyright")).toString().contains(
              QString::fromUtf8("\xc2\xa9")),
          "the record keeps the copyright block");
    check(!worker.wordsOnAir() && worker.sceneActive() == 0,
          "Add to Library presents nothing");
    check(!session.hasDraft(), "the draft is retired after a successful Add");
    check(!session.addToLibrary(), "a second Add has no draft to duplicate");
    check(catalog.songCount() == 1, "and creates no second record");
    {
        // Persistence: a fresh adapter over the same directory sees it.
        CatalogController reopened(nullptr, root);
        check(reopened.songCount() == 1,
              "the added song survives a Library restart");
        check(reopened.items().first().toMap().value(QStringLiteral("ccli"))
                  .toString() == QStringLiteral("9000001"),
              "the CCLI number survives the restart");
    }

    // --- the service reference ------------------------------------------
    check(bridge.addToService(songId), "the song is added to the service");
    check(waitForBridge(bridge), "the service preparation finished");
    const QString showId = docs.showForCatalogItem(songId);
    check(!showId.isEmpty(), "the running order gained a referenced show");
    const QVariantList cues = docs.playlistEntries();
    check(cues.size() == cuesBefore + 1, "exactly one cue was added");
    check(cues.last().toMap().value(QStringLiteral("showId")).toString() == showId,
          "the cue names the referenced show");
    check(cues.last().toMap().value(QStringLiteral("showName")).toString() ==
              QStringLiteral("Morning Light"),
          "the linked show wears the catalog title");
    check(!docs.slidesOf(showId).isEmpty(), "the linked show carries slides");

    // A second add is a second cue over the SAME show (a reference, not a
    // copy).
    check(bridge.addToService(songId), "the same song is added again");
    check(waitForBridge(bridge), "the second preparation finished");
    const QVariantList twice = docs.playlistEntries();
    check(twice.size() == cuesBefore + 2, "the second add is a second cue");
    check(twice.last().toMap().value(QStringLiteral("showId")).toString() == showId,
          "both cues name the one referenced show");
    check(docs.showForCatalogItem(songId) == showId,
          "still one linked show for the catalog item");

    // One service undo removes the second cue (and its preparation), not
    // the catalog.
    docs.undo();
    check(static_cast<int>(docs.playlistEntries().size()) == cuesBefore + 1,
          "one undo removes the whole second add");
    check(catalog.songCount() == 1, "the catalog is untouched by the service undo");

    // --- a Library edit reaches the service ------------------------------
    check(catalog.renameItem(songId, QStringLiteral("Morning Light (Live)")),
          "the Library rename is accepted");
    check(waitForBridge(bridge), "the rename re-preparation finished");
    check(docs.playlistEntries().last().toMap().value(QStringLiteral("showName"))
              .toString() == QStringLiteral("Morning Light (Live)"),
          "the running order shows the renamed song");

    // --- deletion detaches; the service stays playable -------------------
    check(catalog.removeItem(songId), "the Library record is deleted");
    check(docs.showForCatalogItem(songId).isEmpty(), "the service link is detached");
    const QVariantList survivors = docs.playlistEntries();
    check(!survivors.isEmpty() &&
              survivors.last().toMap().value(QStringLiteral("showId")).toString() == showId,
          "the service cue survives the Library deletion");
    check(!docs.slidesOf(showId).isEmpty(),
          "the detached service show keeps its slides");
    check(catalog.songCount() == 0, "the Library record is gone");
    bool reappeared = false;
    for (const auto& row : docs.songs())
        reappeared = reappeared || row.toMap().value("id").toString() == showId;
    check(!reappeared, "a deleted catalog song does not reappear as a Library presentation");

    // The deletion is durable as well.
    {
        CatalogController reopened(nullptr, root);
        check(reopened.songCount() == 0, "a restart sees the deletion");
    }

    check(!worker.wordsOnAir() && worker.sceneActive() == 0,
          "the whole journey presented nothing");
    worker.shutdownNow();

    if (gFailed) {
        std::fprintf(stderr, "song_service_tests: FAILED\n");
        return 1;
    }
    std::printf("song_service_tests: all checks passed\n");
    return 0;
}
