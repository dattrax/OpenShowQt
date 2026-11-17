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
#include <string>

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

    worker.shutdownNow();
    if (gFailed > 0) {
        std::fprintf(stderr, "reflow_planner_tests: %d failure(s)\n", gFailed);
        return 1;
    }
    std::printf("reflow_planner_tests: all checks passed\n");
    return 0;
}
