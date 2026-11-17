/*
 * Task PT.6: the apply transaction. One service history entry for the look
 * (and, for reflow, the replacement slides); one Library history entry for a
 * remembered look; prevalidation before the durable Library write; and no
 * partial publication when either side refuses. Qt Core only.
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
#include "DocumentController.h"
#include "ThemeApplySession.h"

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

using openshow::ui::CatalogController;
using openshow::ui::DocumentController;
using openshow::ui::ThemeApplySession;

std::filesystem::path scratchDirectory() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openshow-theme-apply-" + std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    return path;
}

QVariantList stanzas() {
    QVariantMap verse;
    verse.insert(QStringLiteral("label"), QStringLiteral("Verse 1"));
    verse.insert(QStringLiteral("lines"),
                 QStringList{QStringLiteral("Original verse line"),
                             QStringLiteral("Second original line")});
    QVariantMap chorus;
    chorus.insert(QStringLiteral("label"), QStringLiteral("Chorus"));
    chorus.insert(QStringLiteral("lines"), QStringList{QStringLiteral("Original chorus")});
    return QVariantList{verse, chorus};
}

QStringList slideIds(const QVariantList& rows) {
    QStringList ids;
    for (const QVariant& row : rows) {
        ids.append(row.toMap().value(QStringLiteral("id")).toString());
    }
    return ids;
}

QString previewOf(const QVariantList& rows, int index) {
    return rows.at(index).toMap().value(QStringLiteral("preview")).toString();
}

QString sectionOf(const QVariantList& rows, int index) {
    return rows.at(index).toMap().value(QStringLiteral("sectionLabel")).toString();
}

QVariantMap planFor(DocumentController& docs, const QString& show,
                    bool includeBackground = false) {
    QVariantMap plan;
    plan.insert(QStringLiteral("showId"), show);
    plan.insert(QStringLiteral("sourceFingerprint"), docs.showWordsFingerprint(show));
    plan.insert(QStringLiteral("contentSlideCount"), 2);
    plan.insert(QStringLiteral("leadingBlank"), true);
    plan.insert(QStringLiteral("backgroundAssetId"), QString());
    plan.insert(QStringLiteral("backgroundFit"), 0);
    plan.insert(QStringLiteral("textContext"), QString());
    plan.insert(QStringLiteral("sectionStartsNewSlide"), true);
    QVariantList slides;
    slides.append(QVariantMap{
        {QStringLiteral("lines"),
         QStringList{QStringLiteral("Reflowed verse one"), QStringLiteral("line two")}},
        {QStringLiteral("label"), QStringLiteral("Verse 1")},
        {QStringLiteral("group"), QStringLiteral("Verse 1")}});
    slides.append(QVariantMap{
        {QStringLiteral("lines"), QStringList{QStringLiteral("Reflowed chorus")}},
        {QStringLiteral("label"), QStringLiteral("Chorus")},
        {QStringLiteral("group"), QStringLiteral("Chorus")}});
    plan.insert(QStringLiteral("slides"), slides);
    (void)includeBackground;
    return plan;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const std::filesystem::path root = scratchDirectory();

    CatalogController catalog(nullptr, root / "catalog");
    const QString item = catalog.addSong(
        QStringLiteral("Apply song"), stanzas(), QString(), QString(), QString());
    check(!item.isEmpty(), "the Library item exists to link");

    DocumentController docs(nullptr, root / "service");
    QVariantList addPlan;
    addPlan.append(QVariantMap{{QStringLiteral("lines"), QStringList{}},
                               {QStringLiteral("blank"), true}});
    addPlan.append(QVariantMap{{QStringLiteral("lines"),
                                QStringList{QStringLiteral("Original verse line"),
                                            QStringLiteral("Second original line")}},
                               {QStringLiteral("group"), QStringLiteral("Verse 1")},
                               {QStringLiteral("label"), QStringLiteral("Verse 1")}});
    addPlan.append(QVariantMap{{QStringLiteral("lines"),
                                QStringList{QStringLiteral("Original chorus")}},
                               {QStringLiteral("group"), QStringLiteral("Chorus")},
                               {QStringLiteral("label"), QStringLiteral("Chorus")}});
    const QString show = docs.upsertCatalogSong(
        item, QStringLiteral("Apply song"), QString(), QString(), QString(), addPlan,
        false);
    check(!show.isEmpty(), "the service show is prepared and linked");

    check(docs.createBlankTheme(QStringLiteral("Chosen")), "the chosen look exists");
    QString chosen;
    for (const QVariant& theme : docs.themes()) {
        if (theme.toMap().value(QStringLiteral("name")).toString() ==
            QStringLiteral("Chosen")) {
            chosen = theme.toMap().value(QStringLiteral("id")).toString();
        }
    }
    check(!chosen.isEmpty() &&
              docs.editTheme(chosen, QStringLiteral("Chosen"),
                             QStringLiteral("#abcdef"), QStringLiteral("#000000"),
                             QString(), QStringLiteral("Chosen Serif"), 55.0,
                             QStringLiteral("left"), 1.4) &&
              docs.setThemePlacement(chosen, 120.0, 90.0, 1680.0, 900.0),
          "the chosen look is authored");
    const QString snapshot = docs.themeSnapshotJson(chosen);
    check(!snapshot.isEmpty(), "the chosen look captures");

    ThemeApplySession session(&docs, &catalog);

    // --- Apply only -------------------------------------------------------
    const QVariantList beforeSlides = docs.slidesOf(show);
    check(beforeSlides.size() == 3, "the show starts with a cue and two slides");
    check(session.applyOnly(show, snapshot, item, true),
          "Apply only publishes the look and remembers it");
    check(!docs.showInfo(show).value(QStringLiteral("appliedThemeId"))
               .toString()
               .isEmpty(),
          "the item carries the explicit assignment");
    docs.selectSong(show);
    docs.selectSlide(1);
    check(docs.slideAppearance()
                  .value(QStringLiteral("textColor"))
                  .toMap()
                  .value(QStringLiteral("value"))
                  .toString() == QStringLiteral("#abcdef") &&
              docs.slideAppearance()
                      .value(QStringLiteral("fontSize"))
                      .toMap()
                      .value(QStringLiteral("value"))
                      .toDouble() == 55.0,
          "the item wears the chosen look");
    check(slideIds(docs.slidesOf(show)) == slideIds(beforeSlides),
          "Apply only preserves every slide identity");
    check(catalog.itemThemeSnapshot(item) == snapshot,
          "the Library remembers the same look");
    {
        const QVariantList before = docs.slidesOf(show);
        check(catalog.rememberItemTheme(item, snapshot),
              "remembering the look again is accepted");
        check(slideIds(docs.slidesOf(show)) == slideIds(before),
              "a Library preference change never repaginates the service");
    }

    // One service undo restores the previous chain and keeps the Library
    // preference; the Library's own undo is a separate scope.
    docs.undo();
    check(docs.showInfo(show).value(QStringLiteral("appliedThemeId"))
              .toString()
              .isEmpty(),
          "one service undo removes the assignment");
    check(catalog.itemThemeSnapshot(item) == snapshot,
          "the service undo does not touch the Library preference");
    docs.redo();
    check(!docs.showInfo(show).value(QStringLiteral("appliedThemeId"))
               .toString()
               .isEmpty(),
          "service redo re-applies the look");
    catalog.undo();
    check(catalog.itemThemeSnapshot(item).isEmpty(),
          "one Library undo forgets the preference");
    check(!docs.showInfo(show).value(QStringLiteral("appliedThemeId"))
               .toString()
               .isEmpty(),
          "the Library undo does not rebuild the service");
    catalog.redo();
    check(catalog.itemThemeSnapshot(item) == snapshot, "Library redo restores it");

    // --- Apply and reflow -------------------------------------------------
    // A common background across the text run: reflow keeps it on every new
    // slide and leaves it alone on the leading cue.
    const std::filesystem::path backgroundPath = root / "shared-background.png";
    {
        std::ofstream bytes(backgroundPath, std::ios::binary);
        bytes << "not-a-real-png";
    }
    check(docs.importMediaFile(QString::fromStdString(backgroundPath.string())),
          "a background asset is imported");
    const QString background =
        docs.assets().last().toMap().value(QStringLiteral("id")).toString();
    check(!background.isEmpty(), "the background asset has an id");
    const QVariantList preSlideRows = docs.slidesOf(show);
    for (int i = 1; i < preSlideRows.size(); ++i) {
        check(docs.setSlideBackgroundFor(
                  show,
                  preSlideRows.at(i).toMap().value(QStringLiteral("id")).toString(),
                  background),
              "the text run shares the background");
    }
    const QVariantList oldSlides = docs.slidesOf(show);
    const QString cueId = oldSlides.at(0).toMap().value(QStringLiteral("id")).toString();
    const QString lookBefore = docs.showInfo(show)
                                   .value(QStringLiteral("appliedThemeId"))
                                   .toString();
    QVariantMap plan = planFor(docs, show);
    plan.insert(QStringLiteral("backgroundAssetId"), background);
    check(session.applyReflow(show, snapshot, plan, item, true),
          "Apply and reflow publishes the plan");
    const QVariantList reflowed = docs.slidesOf(show);
    check(reflowed.size() == 3, "the plan replaces the text run and keeps the cue");
    check(reflowed.at(0).toMap().value(QStringLiteral("id")).toString() == cueId,
          "the leading cue keeps its identity");
    check(slideIds(reflowed) != slideIds(oldSlides),
          "the text slides are new identities");
    check(previewOf(reflowed, 1) == QStringLiteral("Reflowed verse one") &&
              previewOf(reflowed, 2) == QStringLiteral("Reflowed chorus"),
          "the plan's words are published in order");
    check(sectionOf(reflowed, 1) == QStringLiteral("Verse 1") &&
              sectionOf(reflowed, 2) == QStringLiteral("Chorus"),
          "the item's sections survive the reflow");
    bool commonBackground = true;
    for (int i = 1; i < reflowed.size(); ++i) {
        commonBackground =
            commonBackground &&
            reflowed.at(i).toMap().value(QStringLiteral("backgroundAssetId")).toString() ==
                background;
    }
    check(commonBackground,
          "every reflowed slide wears the common background");
    check(reflowed.at(0).toMap().value(QStringLiteral("backgroundAssetId"))
              .toString() ==
              oldSlides.at(0).toMap().value(QStringLiteral("backgroundAssetId"))
                  .toString(),
          "the leading cue keeps its own background");
    docs.selectSong(show);
    docs.selectSlide(1);
    check(docs.slideAppearance()
                  .value(QStringLiteral("textColor"))
                  .toMap()
                  .value(QStringLiteral("value"))
                  .toString() == QStringLiteral("#abcdef") &&
              docs.slideAppearance()
                      .value(QStringLiteral("fontSize"))
                      .toMap()
                      .value(QStringLiteral("value"))
                      .toDouble() == 55.0,
          "the reflowed item wears the look it was measured against");

    // One service undo restores theme, slide IDs and words together.
    docs.undo();
    const QVariantList restored = docs.slidesOf(show);
    check(slideIds(restored) == slideIds(oldSlides) &&
              previewOf(restored, 1) == QStringLiteral("Original verse line"),
          "one service undo restores the old slides, IDs and words");
    check(docs.showInfo(show).value(QStringLiteral("appliedThemeId")).toString() ==
              lookBefore,
          "the same undo restores the previous look");
    docs.redo();
    const QVariantList republished = docs.slidesOf(show);
    check(previewOf(republished, 1) == QStringLiteral("Reflowed verse one") &&
              republished.at(0).toMap().value(QStringLiteral("id")).toString() == cueId,
          "redo republishes the plan without re-measuring");

    // --- Stale plan -------------------------------------------------------
    {
        const QVariantList before = docs.slidesOf(show);
        const QString preferenceBefore = catalog.itemThemeSnapshot(item);
        docs.selectSong(show);
        docs.selectSlide(1);
        docs.setSlideText(QStringLiteral("Edited after the plan"));
        docs.flushText();
        const QString problem = docs.reflowPlanProblem(show, plan);
        check(problem.contains(QStringLiteral("words changed")),
              "a stale plan is refused with the operator's sentence");
        check(!session.applyReflow(show, snapshot, plan, item, true) &&
                  session.lastError().contains(QStringLiteral("words changed")),
              "the stale plan is not published");
        check(slideIds(docs.slidesOf(show)) == slideIds(before),
              "a refused stale plan changes no slide");
        check(catalog.itemThemeSnapshot(item) == preferenceBefore,
              "a refused stale plan changes no Library preference");
    }

    // --- Refused Library write -------------------------------------------
    {
        const QVariantList before = docs.slidesOf(show);
        const QVariantMap fresh = planFor(docs, show);
        check(!session.applyReflow(show, snapshot, fresh,
                                   QStringLiteral("item-does-not-exist"), true),
              "an unknown Library item refuses the whole apply");
        check(session.lastError().contains(QStringLiteral("no longer exists")),
              "the refusal names the Library item");
        check(slideIds(docs.slidesOf(show)) == slideIds(before),
              "a refused Library write publishes no service edit");
    }

    // --- Apply only still works offline and for an unlinked item ---------
    {
        check(docs.createSong(QStringLiteral("Unlinked")), "an unlinked song exists");
        check(docs.addSlide(), "the unlinked song has a slide");
        const QString unlinked =
            docs.songs().last().toMap().value(QStringLiteral("id")).toString();
        check(!unlinked.isEmpty() &&
                  session.applyOnly(unlinked, snapshot, QString(), false),
              "Apply only needs no Library link and no worker");
        check(!docs.showInfo(unlinked).value(QStringLiteral("appliedThemeId"))
                   .toString()
                   .isEmpty(),
              "the unlinked item wears the look");
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (gFailed > 0) {
        std::fprintf(stderr, "theme_apply_tests: %d failure(s)\n", gFailed);
        return 1;
    }
    std::printf("theme_apply_tests: all checks passed\n");
    return 0;
}
