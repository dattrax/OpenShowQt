/*
 * Task CS.4 fixtures: InspectorAdapter — the one read model and the one
 * verb pair every shared style control speaks. The tests drive the very
 * class the QML will instantiate (Qt Core only), in a private scratch
 * directory, over the REAL controller: the per-field shape for all four
 * targets, and every commit/reset route landing on its host verb — or
 * refusing with the sentence the control will show. The domain vetting
 * itself is document_controller_tests' territory; what lives here is the
 * routing and the shape.
 */

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <QCoreApplication>
#include <QVariantList>
#include <QVariantMap>

#include "DocumentController.h"
#include "InspectorAdapter.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::ui::DocumentController;
using openshow::ui::InspectorAdapter;

QVariantMap themeRowNamed(DocumentController& docs, const QString& name) {
    for (const QVariant& entry : docs.themes()) {
        if (entry.toMap().value(QStringLiteral("name")).toString() == name) {
            return entry.toMap();
        }
    }
    return {};
}

QVariantMap fieldTarget(const QString& kind, const QString& id = QString(),
                        const QString& view = QString()) {
    QVariantMap target;
    target.insert(QStringLiteral("kind"), kind);
    target.insert(QStringLiteral("id"), id);
    target.insert(QStringLiteral("view"), view);
    return target;
}

QVariantMap rect(double x, double y, double w, double h) {
    QVariantMap out;
    out.insert(QStringLiteral("x"), x);
    out.insert(QStringLiteral("y"), y);
    out.insert(QStringLiteral("width"), w);
    out.insert(QStringLiteral("height"), h);
    return out;
}

// A refused commit is a SENTENCE, not silence: the fixtures count them.
struct Refusals {
    int count = 0;
    QString lastField;
    QString lastReason;
};

void watchRefusals(InspectorAdapter& adapter, Refusals& refusals) {
    QObject::connect(&adapter, &InspectorAdapter::refused,
                     [&refusals](const QString& field, const QString& reason) {
                         ++refusals.count;
                         refusals.lastField = field;
                         refusals.lastReason = reason;
                     });
}

// --- the service floor -----------------------------------------------------

void exerciseServiceTarget(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Floor"));
    InspectorAdapter adapter;
    adapter.setDocument(&docs);
    adapter.setTarget(fieldTarget(QStringLiteral("service")));
    Refusals refusals;
    watchRefusals(adapter, refusals);
    int changes = 0;
    QObject::connect(&adapter, &InspectorAdapter::changed, [&changes] { ++changes; });

    const QVariantMap state = adapter.fieldState(QStringLiteral("transition"));
    check(state.value(QStringLiteral("ok")).toBool() &&
              state.value(QStringLiteral("isSet")).toBool() &&
              state.value(QStringLiteral("source")).toString() == QStringLiteral("the service") &&
              state.value(QStringLiteral("editable")).toBool() &&
              !state.value(QStringLiteral("canReset")).toBool(),
          "the service answers its concrete transition and never offers Inherit");
    const QVariantMap value = state.value(QStringLiteral("value")).toMap();
    check(value == docs.serviceDefaultTransition(),
          "the displayed value IS the service default row");

    QVariantMap cut;
    cut.insert(QStringLiteral("kind"), QStringLiteral("cut"));
    cut.insert(QStringLiteral("textDurationMs"), 0);
    cut.insert(QStringLiteral("mediaDurationMs"), 0);
    check(adapter.commit(QStringLiteral("transition"), cut) &&
              docs.serviceDefaultTransition().value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("cut"),
          "the transition commit lands on the service verb");
    check(changes > 0, "a document change re-opens the adapter reads");
    check(!adapter.reset(QStringLiteral("transition")) && refusals.count == 1 &&
              refusals.lastReason.contains(QStringLiteral("concrete")),
          "reset names the concrete floor instead of pretending");
    check(!adapter.commit(QStringLiteral("fontFamily"), QStringLiteral("Serif")) &&
              refusals.count == 2,
          "the service refuses everything but movement");
    check(adapter.fields() == QStringList{QStringLiteral("transition")},
          "the service advertises only what it can store");
}

// --- the theme target --------------------------------------------------

void exerciseThemeTarget(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("House Style"));
    docs.addSlide();
    check(docs.createBlankTheme(QStringLiteral("House")), "a blank theme exists to edit");
    const QString id = themeRowNamed(docs, QStringLiteral("House"))
                           .value(QStringLiteral("id"))
                           .toString();
    check(!id.isEmpty(), "the row carries its id");
    InspectorAdapter adapter;
    adapter.setDocument(&docs);
    adapter.setTarget(fieldTarget(QStringLiteral("theme"), id));
    Refusals refusals;
    watchRefusals(adapter, refusals);

    const QVariantMap family = adapter.fieldState(QStringLiteral("fontFamily"));
    check(family.value(QStringLiteral("isSet")).toBool() &&
              !family.value(QStringLiteral("canReset")).toBool() &&
              family.value(QStringLiteral("source")).toString() == QStringLiteral("the renderer default") &&
              family.value(QStringLiteral("ownValue")) ==
                  themeRowNamed(docs, QStringLiteral("House"))
                      .value(QStringLiteral("fontFamily")),
          "a stored empty family keeps its ownValue while naming the actual renderer fallback");

    check(adapter.commit(QStringLiteral("fontSize"), 64.0) &&
              themeRowNamed(docs, QStringLiteral("House")).value(QStringLiteral("fontSize"))
                      .toDouble() == 64.0 &&
              themeRowNamed(docs, QStringLiteral("House")).value(QStringLiteral("name"))
                      .toString() == QStringLiteral("House"),
          "one scalar restates the base record through the theme's own verb, name intact");
    check(adapter.commit(QStringLiteral("align"), QStringLiteral("left")) &&
              themeRowNamed(docs, QStringLiteral("House")).value(QStringLiteral("align"))
                      .toString() == QStringLiteral("left"),
          "align lands on the same record");

    check(adapter.commit(QStringLiteral("box"), rect(100.0, 200.0, 800.0, 300.0)) &&
              themeRowNamed(docs, QStringLiteral("House"))
                  .value(QStringLiteral("placementSet"))
                  .toBool(),
          "the box commits as one unit through the placement verb");
    const QVariantMap placed = adapter.fieldState(QStringLiteral("box"));
    check(placed.value(QStringLiteral("isSet")).toBool() &&
              placed.value(QStringLiteral("canReset")).toBool() &&
              placed.value(QStringLiteral("ownValue")).toMap().value(QStringLiteral("width"))
                      .toDouble() == 800.0,
          "a placed box reports its own rectangle");
    check(adapter.reset(QStringLiteral("box")) &&
              !themeRowNamed(docs, QStringLiteral("House"))
                   .value(QStringLiteral("placementSet"))
                   .toBool(),
          "reset removes the placement");
    check(!adapter.reset(QStringLiteral("box")) && refusals.count == 1,
          "a second reset names the nothing");
    check(!adapter.fieldState(QStringLiteral("box"))
               .value(QStringLiteral("isSet"))
               .toBool(),
          "the unplaced box resolves through the sample instead of pretending");

    check(adapter.commit(QStringLiteral("fontWeight"), 700) &&
              themeRowNamed(docs, QStringLiteral("House")).value(QStringLiteral("fontWeightSet"))
                      .toBool(),
          "a style leaf rides the one-field verb");
    const QVariantMap weight = adapter.fieldState(QStringLiteral("fontWeight"));
    check(weight.value(QStringLiteral("value")).toInt() == 700 &&
              weight.value(QStringLiteral("canReset")).toBool(),
          "the leaf reports itself and offers inherit");
    check(adapter.reset(QStringLiteral("fontWeight")) &&
              !themeRowNamed(docs, QStringLiteral("House"))
                   .value(QStringLiteral("fontWeightSet"))
                   .toBool() &&
              adapter.fieldState(QStringLiteral("fontWeight")).value(QStringLiteral("value"))
                      .toInt() == 400 &&
              adapter.fieldState(QStringLiteral("fontWeight")).value(QStringLiteral("source"))
                      .toString() == QStringLiteral("the renderer default"),
          "inherit names the contract default as today's answer");

    check(adapter.commit(QStringLiteral("strokeEnabled"), true) &&
              themeRowNamed(docs, QStringLiteral("House")).value(QStringLiteral("strokeSet"))
                      .toBool(),
          "enabling a stroke materialises the whole record");
    check(adapter.commit(QStringLiteral("strokeWidth"), 5.0) &&
              themeRowNamed(docs, QStringLiteral("House"))
                  .value(QStringLiteral("stroke"))
                  .toMap()
                  .value(QStringLiteral("width"))
                  .toDouble() == 5.0,
          "a member merges into the record");
    const QVariantMap member = adapter.fieldState(QStringLiteral("strokeWidth"));
    check(member.value(QStringLiteral("isSet")).toBool() &&
              !member.value(QStringLiteral("canReset")).toBool(),
          "a member inherits only with its record");
    check(!adapter.reset(QStringLiteral("strokeWidth")) && refusals.count == 2 &&
              refusals.lastReason.contains(QStringLiteral("whole")),
          "a member reset is refused naming the whole-record rule");
    check(adapter.reset(QStringLiteral("stroke")) &&
              !themeRowNamed(docs, QStringLiteral("House")).value(QStringLiteral("strokeSet"))
                   .toBool() &&
              !adapter.fieldState(QStringLiteral("strokeEnabled")).value(QStringLiteral("value"))
                   .toBool(),
          "the whole-record inherit lands, and the default reads as off");

    QVariantMap fade;
    fade.insert(QStringLiteral("kind"), QStringLiteral("fade"));
    fade.insert(QStringLiteral("textDurationMs"), 300);
    fade.insert(QStringLiteral("mediaDurationMs"), 400);
    check(adapter.commit(QStringLiteral("transition"), fade) &&
              themeRowNamed(docs, QStringLiteral("House"))
                  .value(QStringLiteral("transitionSet"))
                  .toBool(),
          "the theme records its own movement");
    check(adapter.fieldState(QStringLiteral("transition")).value(QStringLiteral("source"))
              .toString() == QStringLiteral("this theme"),
          "the theme answers for its own transition");
    check(adapter.reset(QStringLiteral("transition")) &&
              !themeRowNamed(docs, QStringLiteral("House"))
                   .value(QStringLiteral("transitionSet"))
                   .toBool() &&
              adapter.fieldState(QStringLiteral("transition")).value(QStringLiteral("source"))
                      .toString() == QStringLiteral("the service") &&
              adapter.fieldState(QStringLiteral("transition")).value(QStringLiteral("value"))
                      .toMap() == docs.serviceDefaultTransition(),
          "silence falls to the service floor, visibly");
    check(!adapter.reset(QStringLiteral("fontFamily")) && refusals.count == 3,
          "the theme is the root of its own typography");
    check(!adapter.commit(QStringLiteral("nope"), 1.0) && refusals.count == 4,
          "an unknown field is refused naming itself");

    InspectorAdapter ghost;
    ghost.setDocument(&docs);
    ghost.setTarget(fieldTarget(QStringLiteral("theme"), QStringLiteral("show999")));
    check(!ghost.fieldState(QStringLiteral("fontFamily")).value(QStringLiteral("ok")).toBool(),
          "an unknown theme id is an unusable target, not an empty one");
}

// --- the context target -----------------------------------------------------

void exerciseContextTarget(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Reading"));
    docs.addSlide();
    check(docs.createBlankTheme(QStringLiteral("House")), "a blank theme exists");
    const QString id = themeRowNamed(docs, QStringLiteral("House"))
                           .value(QStringLiteral("id"))
                           .toString();
    InspectorAdapter adapter;
    adapter.setDocument(&docs);
    adapter.setTarget(fieldTarget(QStringLiteral("context"), id, QStringLiteral("scripture")));
    Refusals refusals;
    watchRefusals(adapter, refusals);
    const QString themeColour =
        themeRowNamed(docs, QStringLiteral("House")).value(QStringLiteral("textColor")).toString();

    check(adapter.fieldState(QStringLiteral("fontFamily")).value(QStringLiteral("editable")).toBool()
              && !docs.themeContext(id, "scripture").value("set").toBool(),
          "an absent view exposes editable inherited values without creating a record");
    const auto inheritedLook = docs.themeSampleAppearance(id, "scripture");
    check(adapter.commit("fontSize", 36.0) && docs.themeContext(id, "scripture").value("set").toBool(),
          "the first field edit creates an absent context");
    const auto createdLook = docs.themeSampleAppearance(id, "scripture");
    check(createdLook.value("fontSize").toDouble() == 36.0
              && createdLook.value("boxWidth") == inheritedLook.value("boxWidth")
              && createdLook.value("fontFamily") == inheritedLook.value("fontFamily"),
          "first edit preserves the other inherited appearance values");
    docs.undo();
    check(!docs.themeContext(id, "scripture").value("set").toBool(),
          "one undo removes the first context creation");
    check(adapter.commit("transition", QVariantMap{{"kind", "cut"}, {"textDurationMs", 0}, {"mediaDurationMs", 0}})
              && docs.themeContext(id, "scripture").value("transitionSet").toBool(),
          "a first transition edit also creates the context atomically");
    docs.undo();
    check(!docs.themeContext(id, "scripture").value("set").toBool(),
          "one undo removes a first transition edit and its context");

    const auto floor = themeRowNamed(docs, "Default").value("id").toString();
    check(docs.applyThemeContext(floor, "scripture", QVariantMap{
              {"fontFamily", "Lora"}, {"fontSize", 60.0}, {"lineSpacing", 1.7},
              {"align", "center"}, {"box", rect(33, 44, 1234, 567)}})
              && docs.setTextContextTransition(floor, "scripture", QVariantMap{
                  {"kind", "fade"}, {"textDurationMs", 70}, {"mediaDurationMs", 80}}),
          "Default supplies a distinct inherited Bible appearance and movement");
    check(adapter.commit("fontSize", 52.0), "edit an absent view inheriting Default's context");
    const auto kept = docs.themeSampleAppearance(id, "scripture");
    check(kept.value("fontSize").toDouble() == 52 && kept.value("fontFamily") == "Lora"
              && kept.value("boxX").toDouble() == 33 && kept.value("boxHeight").toDouble() == 567
              && docs.themeContext(id, "scripture").value("transition").toMap().value("textDurationMs").toInt() == 70,
          "first edit keeps inherited typography, placement and movement while changing only the chosen value");
    docs.undo();
    check(!docs.themeContext(id, "scripture").value("set").toBool(),
          "one undo returns to Default's inherited context");
    docs.removeThemeContext(floor, "scripture");

    QVariantMap seed;
    seed.insert(QStringLiteral("fontFamily"), QStringLiteral("DejaVu Serif"));
    seed.insert(QStringLiteral("fontSize"), 48.0);
    seed.insert(QStringLiteral("lineSpacing"), 1.4);
    seed.insert(QStringLiteral("align"), QStringLiteral("left"));
    seed.insert(QStringLiteral("box"), rect(200.0, 300.0, 1520.0, 480.0));
    check(docs.applyThemeContext(id, QStringLiteral("scripture"), seed),
          "the view is authored");

    const QVariantMap family = adapter.fieldState(QStringLiteral("fontFamily"));
    check(family.value(QStringLiteral("isSet")).toBool() &&
              family.value(QStringLiteral("ownValue")).toString() ==
                  QStringLiteral("DejaVu Serif") &&
              family.value(QStringLiteral("source")).toString() == QStringLiteral("this view") &&
              family.value(QStringLiteral("value")).toString() == QStringLiteral("DejaVu Serif"),
          "an own field reports its own value through the sample");

    check(adapter.commit(QStringLiteral("align"), QStringLiteral("center")),
          "one typography field commits through the adapter");
    QVariantMap ctx = docs.themeContext(id, QStringLiteral("scripture"));
    check(ctx.value(QStringLiteral("alignSet")).toBool() &&
              ctx.value(QStringLiteral("align")).toString() == QStringLiteral("center"),
          "the entry wears the new align");
    check(ctx.value(QStringLiteral("fontFamilySet")).toBool() &&
              ctx.value(QStringLiteral("fontFamily")).toString() ==
                  QStringLiteral("DejaVu Serif") &&
              ctx.value(QStringLiteral("boxSet")).toBool() &&
              ctx.value(QStringLiteral("width")).toDouble() == 1520.0,
          "THE CENTRALISED MERGE: the untouched members and the box survive "
          "the one-field write");

    check(adapter.reset(QStringLiteral("fontSize")), "a field can go silent again");
    ctx = docs.themeContext(id, QStringLiteral("scripture"));
    check(!ctx.value(QStringLiteral("fontSizeSet")).toBool() &&
              ctx.value(QStringLiteral("fontFamilySet")).toBool() &&
              ctx.value(QStringLiteral("alignSet")).toBool(),
          "only the named member went silent");
    check(adapter.fieldState(QStringLiteral("fontSize")).value(QStringLiteral("source"))
              .toString() == QStringLiteral("this theme"),
          "a silent member names the theme under it");

    check(adapter.commit(QStringLiteral("box"), rect(100.0, 150.0, 1720.0, 780.0)) &&
              docs.themeContext(id, QStringLiteral("scripture"))
                      .value(QStringLiteral("boxSet"))
                      .toBool() &&
              docs.themeContext(id, QStringLiteral("scripture"))
                      .value(QStringLiteral("width"))
                      .toDouble() == 1720.0,
          "the view's box writes as one unit");

    check(!adapter.commit(QStringLiteral("textColor"), QStringLiteral("#ff0000")) &&
              refusals.count == 1 && refusals.lastReason.contains(QStringLiteral("TV.3")),
          "a colour is REFUSED naming TV.3 — the Songs base is never written "
          "from a Bible rail");
    check(themeRowNamed(docs, QStringLiteral("House")).value(QStringLiteral("textColor"))
              .toString() == themeColour,
          "the refused write left the theme's colour exactly as it was");
    const QVariantMap colour = adapter.fieldState(QStringLiteral("textColor"));
    check(!colour.value(QStringLiteral("editable")).toBool() &&
              !colour.value(QStringLiteral("isSet")).toBool() &&
              colour.value(QStringLiteral("source")).toString() == QStringLiteral("this theme") &&
              colour.value(QStringLiteral("reason")).toString().contains(
                  QStringLiteral("TV.3")),
          "and the state states the inherited source with its reason");
    check(!docs.themeContext(id, QStringLiteral("scripture")).contains(QStringLiteral("textColor")),
          "nothing colour-shaped reached the entry");

    QVariantMap cut;
    cut.insert(QStringLiteral("kind"), QStringLiteral("cut"));
    cut.insert(QStringLiteral("textDurationMs"), 0);
    cut.insert(QStringLiteral("mediaDurationMs"), 0);
    check(adapter.commit(QStringLiteral("transition"), cut) &&
              docs.themeContext(id, QStringLiteral("scripture"))
                      .value(QStringLiteral("transitionSet"))
                      .toBool(),
          "the view's movement goes through its own verb");
    check(adapter.reset(QStringLiteral("transition")) &&
              !docs.themeContext(id, QStringLiteral("scripture"))
                   .value(QStringLiteral("transitionSet"))
                   .toBool() &&
              adapter.fieldState(QStringLiteral("transition")).value(QStringLiteral("source"))
                      .toString() == QStringLiteral("the service"),
          "movement silence falls through to the service");
}

// --- the slide target ---------------------------------------------------

void exerciseSlideTarget(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong(QStringLiteral("Custom"));
    docs.addSlide();
    docs.selectSlide(0);
    InspectorAdapter adapter;
    adapter.setDocument(&docs);
    adapter.setTarget(fieldTarget(QStringLiteral("slide")));
    Refusals refusals;
    watchRefusals(adapter, refusals);

    const QVariantMap colour = adapter.fieldState(QStringLiteral("textColor"));
    check(colour.value(QStringLiteral("ok")).toBool() &&
              !colour.value(QStringLiteral("isSet")).toBool() &&
              colour.value(QStringLiteral("source")).toString() == QStringLiteral("the theme"),
          "before customizing, the slide names the theme for its colour");

    check(adapter.commit(QStringLiteral("textColor"), QStringLiteral("#123456")) &&
              docs.slideAppearance()
                  .value(QStringLiteral("textColor"))
                  .toMap()
                  .value(QStringLiteral("customized"))
                  .toBool(),
          "a colour customizes through the slide verb");
    const QVariantMap customized = adapter.fieldState(QStringLiteral("textColor"));
    check(customized.value(QStringLiteral("isSet")).toBool() &&
              customized.value(QStringLiteral("ownValue")).toString() ==
                  QStringLiteral("#123456") &&
              customized.value(QStringLiteral("canReset")).toBool() &&
              customized.value(QStringLiteral("source")).toString() == QStringLiteral("this slide"),
          "the customized field reports itself");
    check(adapter.reset(QStringLiteral("textColor")) &&
              !docs.slideAppearance()
                   .value(QStringLiteral("textColor"))
                   .toMap()
                   .value(QStringLiteral("customized"))
                   .toBool() &&
              adapter.fieldState(QStringLiteral("textColor")).value(QStringLiteral("source"))
                      .toString() == QStringLiteral("the theme"),
          "revert hands the answer back to the theme");

    check(adapter.commit(QStringLiteral("strokeEnabled"), true),
          "an effect member materialises the record on the slide too");
    check(adapter.commit(QStringLiteral("strokeWidth"), 7.0) &&
              docs.slideAppearance()
                  .value(QStringLiteral("stroke"))
                  .toMap()
                  .value(QStringLiteral("value"))
                  .toMap()
                  .value(QStringLiteral("width"))
                  .toDouble() == 7.0,
          "the member MERGES into the row's record — enabling survives "
          "the width edit");
    const QVariantMap member = adapter.fieldState(QStringLiteral("strokeEnabled"));
    check(member.value(QStringLiteral("isSet")).toBool() &&
              member.value(QStringLiteral("value")).toBool() &&
              !member.value(QStringLiteral("canReset")).toBool(),
          "the member reports through the record's sparse set");
    check(!adapter.reset(QStringLiteral("strokeWidth")) && refusals.count == 1,
          "a member revert is refused — whole only");
    check(adapter.reset(QStringLiteral("stroke")) &&
              !docs.slideAppearance()
                   .value(QStringLiteral("stroke"))
                   .toMap()
                   .value(QStringLiteral("customized"))
                   .toBool(),
          "the whole record reverts as one");

    check(adapter.commit(QStringLiteral("box"), rect(150.0, 250.0, 1600.0, 500.0)) &&
              docs.slideAppearance()
                  .value(QStringLiteral("placement"))
                  .toMap()
                  .value(QStringLiteral("customized"))
                  .toBool(),
          "the box customizes placement through the box verb");
    check(adapter.reset(QStringLiteral("box")) &&
              !docs.slideAppearance()
                   .value(QStringLiteral("placement"))
                   .toMap()
                   .value(QStringLiteral("customized"))
                   .toBool(),
          "the placement revert takes the whole rectangle back");

    QVariantMap fade;
    fade.insert(QStringLiteral("kind"), QStringLiteral("fade"));
    fade.insert(QStringLiteral("textDurationMs"), 500);
    fade.insert(QStringLiteral("mediaDurationMs"), 800);
    check(!adapter.commit(QStringLiteral("transition"), fade) && refusals.count == 2 &&
              !adapter.fieldState(QStringLiteral("transition"))
                   .value(QStringLiteral("editable"))
                   .toBool(),
          "a slide never carries its own movement — stated, not silently "
          "accepted");

    // No slide, honest target failure.
    DocumentController empty(nullptr, root.parent_path() / "slide-empty");
    InspectorAdapter bare;
    bare.setDocument(&empty);
    bare.setTarget(fieldTarget(QStringLiteral("slide")));
    check(!bare.fieldState(QStringLiteral("textColor")).value(QStringLiteral("ok")).toBool(),
          "with no current slide the target says so");
}

void exerciseInheritedTheme(const std::filesystem::path& root) {
    DocumentController docs(nullptr, root);
    docs.createSong("Inherited fixture");
    docs.addSlide();
    const QString floor = QString::fromLatin1(openshow::domain::Documents::kDefaultThemeId);
    check(docs.editTheme(floor, "Default", "#ffffff", "#000000", "", "Lora", 48,
                         "center", 1.25), "the editable Default supplies family and size");
    const QVariantMap stroke{{"enabled", true}, {"color", "#123456"}, {"width", 6.0}};
    const QVariantMap shadow{{"enabled", true}, {"color", "#654321"}, {"opacity", 0.5},
                             {"offsetX", 8.0}, {"offsetY", 9.0}, {"blurSigma", 3.0}};
    check(docs.setThemeTextStyle(floor, {{"fontWeight", 800}, {"fontStyle", "italic"},
                                        {"fitMode", "fixed"}, {"minFontSize", 20.0},
                                        {"stroke", stroke}, {"shadow", shadow}}),
          "Default carries nontrivial typography and effects");
    docs.createBlankTheme("Inheriting");
    const QString id = themeRowNamed(docs, "Inheriting").value("id").toString();
    InspectorAdapter adapter;
    adapter.setDocument(&docs);
    adapter.setTarget(fieldTarget("theme", id));
    const auto sample = docs.themeSampleAppearance(id);
    for (const QString field : {"fontWeight", "fontStyle", "fitMode", "minFontSize",
                                "stroke", "shadow"}) {
        const auto state = adapter.fieldState(field);
        check(state.value("value") == sample.value(field) && !state.value("isSet").toBool() &&
                  state.value("source").toString() == "the Default theme",
              "an inherited style matches the compiled sample and names Default, not the contract");
    }
    check(adapter.fieldState("fontFamily").value("value") == sample.value("fontFamily") &&
              adapter.fieldState("fontSize").value("value") == sample.value("fontSize"),
          "base fields also display the compiled family and size rather than silent raw fields");
    check(adapter.fieldState("strokeColor").value("value").toString() == "#123456" &&
              adapter.fieldState("shadowOffsetX").value("value").toDouble() == 8,
          "effect member controls show the inherited whole record");
    check(adapter.commit("fontWeight", 500) && adapter.reset("fontWeight") &&
              adapter.fieldState("fontWeight").value("value").toInt() == 800,
          "Inherit restores Default's weight on the inspector as well as the canvas");
    check(adapter.commit("strokeEnabled", false) &&
              !adapter.fieldState("strokeEnabled").value("value").toBool() &&
              adapter.reset("stroke") && adapter.fieldState("strokeEnabled").value("value").toBool(),
          "explicit effect-off and whole-record Inherit remain distinct");
    check(themeRowNamed(docs, "Default").value("fontWeight").toInt() == 800,
          "reading and overriding an inheriting theme leaves Default unchanged");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("openshow-inspector-tests"));

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
                                           ("openshow-inspector-" + std::to_string(getpid()));
    std::filesystem::remove_all(tempRoot);
    exerciseServiceTarget(tempRoot / "service");
    exerciseThemeTarget(tempRoot / "theme");
    exerciseContextTarget(tempRoot / "context");
    exerciseSlideTarget(tempRoot / "slide");
    exerciseInheritedTheme(tempRoot / "inheritance");
    std::filesystem::remove_all(tempRoot);

    if (gFailed) {
        return 1;
    }
    std::printf("inspector_adapter_tests: all fixtures passed\n");
    return 0;
}
