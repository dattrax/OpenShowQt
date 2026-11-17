/*
 * Task PT.5: optional reflow preparation against the chosen future
 * appearance, driven through the production adapters and the REAL worker.
 *
 * Proves that an eligible item produces a measured plan whose words are the
 * item's own, that a smaller box yields more slides, that the portable
 * look's numbers (not the browsing cursor's) are measured, that a plan for
 * changed words is refused, and that ineligible/no-worker/cancelled paths
 * leave the document untouched.
 *
 * Needs a display for the worker's output window: desktop preset, label
 * display.
 */

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QTest>
#include <QTemporaryDir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "CatalogController.h"
#include "DocumentController.h"
#include "ReflowPlanner.h"
#include "ThemeApplySession.h"
#include "WorkerController.h"

namespace {

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        ++gFailed;
    }
    return condition;
}

bool waitForMeasureReady(openshow::ui::WorkerController& worker, int ms = 30000) {
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

struct Reply {
    bool fired = false;
    bool ok = false;
    QString error;
};

// Spins the event loop until the reply lands or the clock runs out. The
// caller installs the connecting lambda, because each leg wants its own
// captured Reply.
void waitForReply(openshow::ui::ReflowPlanner&, Reply* reply, int ms = 30000) {
    QElapsedTimer clock;
    clock.start();
    while (!reply->fired && clock.elapsed() < ms) {
        QTest::qWait(20);
    }
}

QStringList slideListIds(const QVariantList& rows) {
    QStringList ids;
    for (const QVariant& row : rows) {
        ids.append(row.toMap().value(QStringLiteral("id")).toString());
    }
    return ids;
}

QVariantList refreshedSlides(const QVariantMap& plan) {
    return plan.value(QStringLiteral("slides")).toList();
}

QStringList wordsOf(const QVariantList& slides) {
    QStringList words;
    for (const QVariant& row : slides) {
        for (const QVariant& line : row.toMap().value(QStringLiteral("lines")).toList()) {
            for (const QString& word :
                 line.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                words.append(word);
            }
        }
    }
    return words;
}

QString themeIdNamed(openshow::ui::DocumentController& docs, const QString& name) {
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() == name) {
            return theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    return QString();
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        return 1;
    }
    const std::filesystem::path root =
        std::filesystem::path(scratch.path().toStdString());

    openshow::ui::WorkerController worker;
    worker.start();
    if (!check(waitForMeasureReady(worker), "the real worker came up and measures")) {
        worker.shutdownNow();
        return 1;
    }

    openshow::ui::DocumentController docs(nullptr, root / "service");
    // A leading blank cue (the Add's presentation policy), then two stanzas.
    QVariantList plan;
    plan.append(QVariantMap{{QStringLiteral("lines"), QStringList{}},
                            {QStringLiteral("blank"), true}});
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
        QStringLiteral("reflow-prep"), QStringLiteral("Prepared song"), QString(),
        QString(), QString(), plan, false);
    check(!show.isEmpty(), "a song with a leading cue is prepared");

    // Two portable looks: a roomy box and a tight one.
    check(docs.createBlankTheme(QStringLiteral("Roomy")) &&
              docs.createBlankTheme(QStringLiteral("Tight")),
          "two service themes exist to capture");
    const QString roomy = themeIdNamed(docs, QStringLiteral("Roomy"));
    const QString tight = themeIdNamed(docs, QStringLiteral("Tight"));
    check(docs.setThemePlacement(roomy, 100.0, 80.0, 1720.0, 920.0) &&
              docs.editTheme(roomy, QStringLiteral("Roomy"), QStringLiteral("#ffffff"),
                             QStringLiteral("#000000"), QString(), QStringLiteral("Sans"),
                             48.0, QStringLiteral("center"), 1.3),
          "the roomy look is authored");
    check(docs.setThemePlacement(tight, 700.0, 400.0, 360.0, 160.0) &&
              docs.editTheme(tight, QStringLiteral("Tight"), QStringLiteral("#ffffff"),
                             QStringLiteral("#000000"), QString(), QStringLiteral("Sans"),
                             72.0, QStringLiteral("center"), 1.3),
          "the tight look is authored");
    const QString roomyJson = docs.themeSnapshotJson(roomy);
    const QString tightJson = docs.themeSnapshotJson(tight);
    check(!roomyJson.isEmpty() && !tightJson.isEmpty(),
          "both looks capture as portable snapshots");

    openshow::ui::ReflowPlanner planner(&docs, &worker);
    const QVariantMap eligibility = planner.eligibility(show);
    check(eligibility.value(QStringLiteral("eligible")).toBool() &&
              eligibility.value(QStringLiteral("sourceKind")).toString() ==
                  QStringLiteral("catalog-song") &&
              eligibility.value(QStringLiteral("blockCount")).toInt() == 2 &&
              eligibility.value(QStringLiteral("leadingBlank")).toBool() &&
              eligibility.value(QStringLiteral("usesStoredSource")).toBool(),
          "the prepared song is eligible, keeps its cue and uses its source");

    Reply roomyReply;
    QObject::connect(&planner, &openshow::ui::ReflowPlanner::prepared,
                     [&roomyReply](bool ok, const QString& error) {
                         roomyReply.fired = true;
                         roomyReply.ok = ok;
                         roomyReply.error = error;
                     });
    check(planner.prepare(show, roomyJson), "the roomy preparation starts");
    waitForReply(planner, &roomyReply);
    check(roomyReply.ok, "the roomy look measures a plan");
    const QVariantMap roomyPlan = planner.plan();
    const int roomySlides =
        roomyPlan.value(QStringLiteral("slideCount")).toInt();
    check(roomyPlan.value(QStringLiteral("ok")).toBool() && roomySlides == 2,
          "each stanza fits one slide in the roomy box");
    check(roomyPlan.value(QStringLiteral("leadingBlank")).toBool(),
          "the plan reports the leading cue to preserve");
    check(wordsOf(roomyPlan.value(QStringLiteral("slides")).toList()) ==
              QStringList{QStringLiteral("Morning"), QStringLiteral("light"),
                          QStringLiteral("Every"), QStringLiteral("shadow"),
                          QStringLiteral("gone"), QStringLiteral("Sing,"),
                          QStringLiteral("sing")},
          "the plan's words are the item's own, in order");
    check(roomyPlan.value(QStringLiteral("sourceKind")).toString() ==
              QStringLiteral("catalog-song") &&
              !roomyPlan.value(QStringLiteral("usedFallback")).toBool(),
          "the plan names its source honestly");

    Reply tightReply;
    QObject::connect(&planner, &openshow::ui::ReflowPlanner::prepared,
                     [&tightReply](bool ok, const QString& error) {
                         tightReply.fired = true;
                         tightReply.ok = ok;
                         tightReply.error = error;
                     });
    check(planner.prepare(show, tightJson), "the tight preparation starts");
    waitForReply(planner, &tightReply);
    check(tightReply.ok, "the tight look measures a plan");
    const QVariantMap tightPlan = planner.plan();
    check(tightPlan.value(QStringLiteral("slideCount")).toInt() > roomySlides,
          "a smaller box produces more slides");
    check(tightPlan.value(QStringLiteral("reservedHeight")).toDouble() == 0.0,
          "a song reserves no caption room");
    check(wordsOf(tightPlan.value(QStringLiteral("slides")).toList()) ==
              wordsOf(roomyPlan.value(QStringLiteral("slides")).toList()),
          "the tighter packing keeps exactly the same words");

    int committedSlideCount = 0;
    // The prepared plan publishes as ONE service edit, keeping the leading
    // cue and restoring everything with one undo (PT.6's contract, proven
    // here against the real worker's numbers).
    {
        const QVariantList before = docs.slidesOf(show);
        const QString cueId =
            before.at(0).toMap().value(QStringLiteral("id")).toString();
        const QString firstPlannedLine =
            tightPlan.value(QStringLiteral("slides"))
                .toList()
                .at(0)
                .toMap()
                .value(QStringLiteral("lines"))
                .toList()
                .at(0)
                .toString();
        openshow::ui::ThemeApplySession session(&docs, nullptr);
        check(session.applyReflow(show, tightJson, tightPlan),
              "the prepared plan publishes as one service edit");
        const QVariantList after = docs.slidesOf(show);
        check(after.size() ==
                      1 + tightPlan.value(QStringLiteral("slideCount")).toInt() &&
                  after.at(0).toMap().value(QStringLiteral("id")).toString() == cueId,
              "the plan replaces the text run and keeps the leading cue's identity");
        check(after.at(1).toMap().value(QStringLiteral("preview")).toString() ==
                  firstPlannedLine,
              "the committed words are the measured plan's words");
        docs.undo();
        check(slideListIds(docs.slidesOf(show)) == slideListIds(before),
              "one service undo restores the old slide identities and words");
        docs.redo();
        check(docs.slidesOf(show)
                  .at(1)
                  .toMap()
                  .value(QStringLiteral("preview"))
                  .toString() == firstPlannedLine,
              "redo republishes the plan without measuring again");
        committedSlideCount = docs.slidesOf(show).size();
    }

    // Cancel clears the plan and retires the ask.
    planner.cancel();
    check(!planner.hasPlan() && planner.plan().isEmpty(),
          "cancelling leaves no plan");

    // A text edit while the measurement is in flight refuses the stale plan.
    {
        Reply stale;
        QObject::connect(&planner, &openshow::ui::ReflowPlanner::prepared,
                         [&stale](bool ok, const QString& error) {
                             stale.fired = true;
                             stale.ok = ok;
                             stale.error = error;
                         });
        check(planner.prepare(show, roomyJson), "a preparation starts before the edit");
        docs.selectSong(show);
        docs.selectSlide(1);
        docs.setSlideText(QStringLiteral("Changed while measuring"));
        docs.flushText();
        waitForReply(planner, &stale);
        check(stale.fired && !stale.ok &&
                  stale.error.contains(QStringLiteral("words changed")),
              "a plan for words that moved is refused, not published");
        check(!planner.hasPlan(), "the stale failure left no plan");
    }

    // Two text boxes on one slide: no Reflow, with a specific reason.
    {
        openshow::ui::DocumentController shaped(nullptr, root / "shaped");
        check(shaped.createSong(QStringLiteral("Two boxes")), "the shaped song exists");
        check(shaped.addSlide(), "the shaped song has a slide");
        shaped.selectSlide(0);
        check(shaped.addTextBox(), "the shaped song has two text boxes");
        openshow::ui::ReflowPlanner shapedPlanner(&shaped, &worker);
        const QString shapedId =
            shaped.songs().first().toMap().value(QStringLiteral("id")).toString();
        const QVariantMap shapedEligibility = shapedPlanner.eligibility(shapedId);
        check(!shapedEligibility.value(QStringLiteral("eligible")).toBool() &&
                  shapedEligibility.value(QStringLiteral("reason"))
                          .toString()
                          .contains(QStringLiteral("more than one text box")),
              "the eligibility row explains the second box");
        Reply refused;
        QObject::connect(&shapedPlanner, &openshow::ui::ReflowPlanner::prepared,
                         [&refused](bool ok, const QString& error) {
                             refused.fired = true;
                             refused.ok = ok;
                             refused.error = error;
                         });
        check(!shapedPlanner.prepare(shapedId, roomyJson) && refused.fired &&
                  !refused.ok &&
                  refused.error.contains(QStringLiteral("more than one text box")),
              "an ineligible item refuses before any measurement");
    }

    // No measuring worker: Apply only is still available, but reflow is not.
    {
        openshow::ui::ReflowPlanner offline(&docs, nullptr);
        Reply refused;
        QObject::connect(&offline, &openshow::ui::ReflowPlanner::prepared,
                         [&refused](bool ok, const QString& error) {
                             refused.fired = true;
                             refused.ok = ok;
                             refused.error = error;
                         });
        check(!offline.prepare(show, roomyJson) && refused.fired && !refused.ok &&
                  refused.error.contains(QStringLiteral("measuring output")),
              "reflow refuses without a measuring worker");
    }

    // The document itself was never mutated by any of the above: the only
    // edit was the test's own stale-words change, and the item still has its
    // three slides and its captured source.
    check(docs.slidesOf(show).size() == committedSlideCount &&
              docs.reflowSourceInfo(show).value(QStringLiteral("hasSource")).toBool(),
          "preparing, cancelling and a refused stale plan never re-paginate the item");

    // --- PT.8: the Library -> service -> reflow -> Go -> undo/save journey --
    {
        const std::filesystem::path journeyRoot = root / "journey";
        openshow::ui::CatalogController library(nullptr, journeyRoot / "catalog");
        QVariantList journeyStanzas;
        journeyStanzas.append(QVariantMap{
            {QStringLiteral("label"), QStringLiteral("Verse 1")},
            {QStringLiteral("lines"),
             QStringList{QStringLiteral("Journey verse one"),
                         QStringLiteral("second line")}}});
        journeyStanzas.append(QVariantMap{
            {QStringLiteral("label"), QStringLiteral("Chorus")},
            {QStringLiteral("lines"), QStringList{QStringLiteral("Journey chorus")}}});
        const QString item = library.addSong(QStringLiteral("Journey song"),
                                             journeyStanzas, QString(), QString(),
                                             QString());
        check(!item.isEmpty(), "PT.8: the Library song exists");

        // The look is captured in one service and remembered by value.
        QString snapshot;
        {
            openshow::ui::DocumentController serviceA(nullptr, journeyRoot / "a");
            check(serviceA.createBlankTheme(QStringLiteral("Easter")),
                  "PT.8: service A has a look");
            const QString easter = themeIdNamed(serviceA, QStringLiteral("Easter"));
            check(!easter.isEmpty() &&
                      serviceA.editTheme(easter, QStringLiteral("Easter"),
                                         QStringLiteral("#ffeeaa"),
                                         QStringLiteral("#000000"), QString(),
                                         QStringLiteral("Journey Serif"), 52.0,
                                         QStringLiteral("center"), 1.35) &&
                      serviceA.setThemePlacement(easter, 140.0, 110.0, 1640.0, 860.0),
                  "PT.8: the look is authored");
            snapshot = serviceA.themeSnapshotJson(easter);
            check(!snapshot.isEmpty() &&
                      library.rememberItemTheme(item, snapshot),
                  "PT.8: the Library remembers the look (service A is not touched)");
        }

        // A later service B prepares the song and wears the saved look.
        openshow::ui::DocumentController serviceB(nullptr, journeyRoot / "b");
        const std::filesystem::path backgroundPath = journeyRoot / "journey-bg.png";
        {
            std::ofstream bytes(backgroundPath, std::ios::binary);
            bytes << "journey-background";
        }
        check(serviceB.importMediaFile(QString::fromStdString(backgroundPath.string())),
              "PT.8: service B has a background asset");
        const QString background =
            serviceB.assets().last().toMap().value(QStringLiteral("id")).toString();
        QVariantList planRows;
        planRows.append(QVariantMap{
            {QStringLiteral("lines"),
             QStringList{QStringLiteral("Journey verse one"),
                         QStringLiteral("second line")}},
            {QStringLiteral("group"), QStringLiteral("Verse 1")},
            {QStringLiteral("label"), QStringLiteral("Verse 1")}});
        planRows.append(QVariantMap{
            {QStringLiteral("lines"), QStringList{QStringLiteral("Journey chorus")}},
            {QStringLiteral("group"), QStringLiteral("Chorus")},
            {QStringLiteral("label"), QStringLiteral("Chorus")}});
        const QString showB = serviceB.upsertCatalogSong(
            item, QStringLiteral("Journey song"), QString(), QString(), QString(),
            planRows, true, -1, snapshot);
        check(!showB.isEmpty(), "PT.8: service B prepares the song");
        serviceB.selectSong(showB);
        serviceB.selectSlide(0);
        check(serviceB.slideAppearance()
                      .value(QStringLiteral("textColor"))
                      .toMap()
                      .value(QStringLiteral("value"))
                      .toString() == QStringLiteral("#ffeeaa") &&
                  serviceB.slideAppearance()
                          .value(QStringLiteral("fontSize"))
                          .toMap()
                          .value(QStringLiteral("value"))
                          .toDouble() == 52.0,
              "PT.8: the future-service saved look is worn, with no Library open elsewhere");

        // Per-slide overrides and media: Apply only removes the text override
        // and leaves the backdrop and common background alone.
        const QVariantList beforeApply = serviceB.slidesOf(showB);
        for (int i = 0; i < beforeApply.size(); ++i) {
            check(serviceB.setSlideBackgroundFor(
                      showB,
                      beforeApply.at(i).toMap().value(QStringLiteral("id")).toString(),
                      background),
                  "PT.8: the text run shares a background");
        }
        check(serviceB.customizeCurrentSlide(QStringLiteral("textColor"),
                                             QStringLiteral("#123456")) &&
                  serviceB.customizeCurrentSlide(QStringLiteral("backgroundColor"),
                                                 QStringLiteral("#654321")),
              "PT.8: the slide carries old text and backdrop overrides");
        openshow::ui::ThemeApplySession session(&serviceB, &library);
        check(session.applyOnly(showB, snapshot, item, true),
              "PT.8: Apply only republishes the same look");
        check(serviceB.slideAppearance()
                      .value(QStringLiteral("textColor"))
                      .toMap()
                      .value(QStringLiteral("customized"))
                      .toBool() == false &&
                  serviceB.slideAppearance()
                          .value(QStringLiteral("backgroundColor"))
                          .toMap()
                          .value(QStringLiteral("customized"))
                          .toBool(),
              "PT.8: the text override went and the backdrop override stayed");
        check(slideListIds(serviceB.slidesOf(showB)) == slideListIds(beforeApply),
              "PT.8: Apply only preserves every slide ID");
        bool mediaKept = true;
        for (const QVariant& row : serviceB.slidesOf(showB)) {
            mediaKept = mediaKept &&
                        row.toMap().value(QStringLiteral("backgroundAssetId"))
                                .toString() == background;
        }
        check(mediaKept, "PT.8: the common background survives Apply only");

        // Reflow, then Go: the compiled scene carries the same words.
        openshow::ui::ReflowPlanner journeyPlanner(&serviceB, &worker);
        Reply prepared;
        QObject::connect(&journeyPlanner, &openshow::ui::ReflowPlanner::prepared,
                         [&prepared](bool ok, const QString& error) {
                             prepared.fired = true;
                             prepared.ok = ok;
                             prepared.error = error;
                         });
        check(journeyPlanner.prepare(showB, snapshot),
              "PT.8: the reflow preparation starts");
        waitForReply(journeyPlanner, &prepared);
        check(prepared.ok, "PT.8: the reflow plan measures");
        const QVariantMap journeyPlan = journeyPlanner.plan();
        check(session.applyReflow(showB, snapshot, journeyPlan, item, true),
              "PT.8: Apply and reflow publishes the plan");
        const QVariantList reflowedRows = serviceB.slidesOf(showB);
        check(wordsOf(refreshedSlides(journeyPlan)) ==
                  (QStringList{QStringLiteral("Journey"), QStringLiteral("verse"),
                               QStringLiteral("one"), QStringLiteral("second"),
                               QStringLiteral("line"), QStringLiteral("Journey"),
                               QStringLiteral("chorus")}),
              "PT.8: the plan keeps every current word in order");
        const QString firstSlideId =
            reflowedRows.at(0).toMap().value(QStringLiteral("id")).toString();
        const QString scene = serviceB.sceneJsonForSlide(firstSlideId);
        check(!scene.isEmpty() && scene.contains(QStringLiteral("Journey verse one")),
              "PT.8: the reflowed slide compiles with its words");
        serviceB.selectSong(showB);
        serviceB.selectSlide(0);
        const bool wasLive = worker.wordsOnAir();
        worker.presentSceneJson(scene);
        QElapsedTimer goClock;
        goClock.start();
        while (!worker.wordsOnAir() && goClock.elapsed() < 10000)
            QTest::qWait(20);
        check(worker.wordsOnAir(), "PT.8: Go puts the reflowed words on air");
        if (!wasLive) {
            // Leave the audience where we found it.
            worker.clearSlide();
        }

        // Save/reopen: the source, the look and the words survive.
        check(serviceB.save(), "PT.8: service B saves");
        openshow::ui::DocumentController reopened(nullptr, journeyRoot / "b");
        check(reopened.load(), "PT.8: service B reopens");
        check(reopened.reflowSourceMatches(showB) &&
                  reopened.showInfo(showB)
                          .value(QStringLiteral("appliedThemeId"))
                          .toString() != QString(),
              "PT.8: the applied look and the source survive save/reopen");
        reopened.selectSong(showB);
        reopened.selectSlide(0);
        check(reopened.slideAppearance()
                      .value(QStringLiteral("textColor"))
                      .toMap()
                      .value(QStringLiteral("value"))
                      .toString() == QStringLiteral("#ffeeaa"),
              "PT.8: the reopened item still wears the chosen look");

        // Legacy scripture: an Add captures its own source, so the legacy
        // path is a captured source dropped again — the eligibility then
        // uses the conservative current-word source and honest labels.
        {
            check(serviceB.createPlaylist(QStringLiteral("Legacy schedule")),
                  "PT.8: a schedule exists for the legacy passage");
            QString playlist;
            for (const QVariant& row : serviceB.playlists()) {
                if (row.toMap().value(QStringLiteral("name")).toString() ==
                    QStringLiteral("Legacy schedule"))
                    playlist = row.toMap().value(QStringLiteral("id")).toString();
            }
            QVariantList verses;
            verses.append(QVariantMap{
                {QStringLiteral("lines"),
                 QStringList{QStringLiteral("In the beginning was the Word")}},
                {QStringLiteral("notes"), QStringLiteral("John 1:1")}});
            verses.append(QVariantMap{
                {QStringLiteral("lines"),
                 QStringList{QStringLiteral("He was with God in the beginning")}},
                {QStringLiteral("notes"), QStringLiteral("John 1:2")}});
            check(!playlist.isEmpty() &&
                      serviceB.addScripturePassage(
                          playlist, QStringLiteral("John 1:1-3"),
                          QStringLiteral("NIV"), QStringLiteral("NIV notice"),
                          verses),
                  "PT.8: a legacy passage is added");
            QString passage;
            for (const QVariant& entry : serviceB.playlistEntries()) {
                if (entry.toMap().value(QStringLiteral("showName")).toString() ==
                    QStringLiteral("John 1:1-3"))
                    passage = entry.toMap().value(QStringLiteral("showId")).toString();
            }
            check(!passage.isEmpty() &&
                      serviceB.forgetReflowSource(passage),
                  "PT.8: the passage's captured source is dropped");
            const QVariantMap legacyEligibility =
                journeyPlanner.eligibility(passage);
            check(legacyEligibility.value(QStringLiteral("eligible")).toBool() &&
                      !legacyEligibility.value(QStringLiteral("usesStoredSource"))
                           .toBool() &&
                      !legacyEligibility.value(QStringLiteral("sectionStartsNewSlide"))
                           .toBool(),
                  "PT.8: legacy scripture falls back to its current words");
            Reply legacyPrepared;
            QObject::connect(&journeyPlanner, &openshow::ui::ReflowPlanner::prepared,
                             [&legacyPrepared](bool ok, const QString& error) {
                                 legacyPrepared.fired = true;
                                 legacyPrepared.ok = ok;
                                 legacyPrepared.error = error;
                             });
            check(journeyPlanner.prepare(passage, snapshot),
                  "PT.8: the legacy passage prepares");
            waitForReply(journeyPlanner, &legacyPrepared);
            check(legacyPrepared.ok &&
                      session.applyReflow(passage, snapshot,
                                          journeyPlanner.plan(), QString(), false),
                  "PT.8: the legacy passage reflows without a Library link");
            QStringList passageWords;
            for (const QVariant& row : serviceB.slidesOf(passage)) {
                for (const QString& word :
                     row.toMap().value(QStringLiteral("preview")).toString()
                         .split(QLatin1Char(' '), Qt::SkipEmptyParts))
                    passageWords.append(word);
            }
            check(passageWords.join(QLatin1Char(' ')) ==
                      QStringLiteral("In the beginning was the Word"),
                  "PT.8: the legacy passage keeps its current words");
        }

        // Separate undo scopes.
        openshow::ui::DocumentController undoService(nullptr, journeyRoot / "b");
        check(undoService.load(), "PT.8: the journey service reloads for undo");
        openshow::ui::ThemeApplySession undoSession(&undoService, &library);
        const QVariantList beforeUndo = undoService.slidesOf(showB);
        check(undoSession.applyOnly(showB, snapshot, item, true),
              "PT.8: an apply is published for the undo leg");
        undoService.undo();
        check(slideListIds(undoService.slidesOf(showB)) == slideListIds(beforeUndo),
              "PT.8: one service undo restores the slide identities");
        check(!library.itemThemeSnapshot(item).isEmpty(),
              "PT.8: the service undo leaves the Library preference");
        library.undo();
        check(library.itemThemeSnapshot(item).isEmpty(),
              "PT.8: the Library undo is its own scope");
    }

    worker.shutdownNow();
    if (gFailed > 0) {
        std::fprintf(stderr, "reflow_planner_tests: %d failure(s)\n", gFailed);
        return 1;
    }
    std::printf("reflow_planner_tests: all checks passed\n");
    return 0;
}
