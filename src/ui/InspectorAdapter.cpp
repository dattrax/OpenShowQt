#include "InspectorAdapter.h"

#include <cstddef>

#include "DocumentController.h"

#include "openshow/domain/documents.h"

namespace openshow::ui {

namespace {

// The canonical field spellings (the catalogue's, which are the domain's):
// base appearance, style-block leaves, whole effect records, box, movement.
const char* const kBaseFields[] = {"fontFamily", "fontSize", "align", "lineSpacing",
                                   "textColor", "backgroundColor", "backgroundGradient"};
const char* const kStyleLeaves[] = {"fontWeight", "fontStyle", "fitMode", "minFontSize"};
const char* const kStrokeMembers[] = {"strokeEnabled", "strokeColor", "strokeWidth"};
const char* const kShadowMembers[] = {"shadowEnabled", "shadowColor", "shadowOpacity",
                                      "shadowOffsetX", "shadowOffsetY", "shadowBlurSigma"};

bool isIn(const char* const* list, std::size_t count, const QString& field) {
    for (std::size_t i = 0; i < count; ++i) {
        if (field == QLatin1String(list[i])) {
            return true;
        }
    }
    return false;
}

bool isBaseField(const QString& field) {
    return isIn(kBaseFields, std::size(kBaseFields), field);
}
bool isStyleLeaf(const QString& field) {
    return isIn(kStyleLeaves, std::size(kStyleLeaves), field);
}
bool isStrokeMember(const QString& field) {
    return isIn(kStrokeMembers, std::size(kStrokeMembers), field);
}
bool isShadowMember(const QString& field) {
    return isIn(kShadowMembers, std::size(kShadowMembers), field);
}

// "box" is the inspector's spelling; the hosts call the same rectangle
// "placement" (theme records, slide customizations). One canonical name
// inside this file so every route agrees on which unit is meant.
QString canonicalField(const QString& field) {
    return field == QLatin1String("box") ? QLatin1String("placement") : field;
}

// The one shape every answer wears.
QVariantMap shape(const QVariant& value, const QVariant& ownValue, bool isSet,
                  const QString& source, bool editable, bool canReset) {
    QVariantMap out;
    out.insert(QStringLiteral("value"), value);
    out.insert(QStringLiteral("ownValue"), ownValue);
    out.insert(QStringLiteral("isSet"), isSet);
    out.insert(QStringLiteral("source"), source);
    out.insert(QStringLiteral("editable"), editable);
    out.insert(QStringLiteral("canReset"), canReset);
    out.insert(QStringLiteral("ok"), true);
    return out;
}

// An unusable target still answers in the same shape: nothing set, nothing
// editable, and the reason names the missing identity.
QVariantMap nothing(const QString& reason) {
    QVariantMap out = shape(QVariant(), QVariant(), false, QString(), false, false);
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("reason"), reason);
    return out;
}

// The contract's own defaults (the same constants the resolver's concrete
// answers wear, so "unset" and "draws today" can never drift apart).
QVariantMap defaultStrokeMap() {
    const openshow::domain::TextStroke defaults;
    QVariantMap out;
    out.insert(QStringLiteral("enabled"), defaults.enabled);
    out.insert(QStringLiteral("color"), QString::fromStdString(defaults.color));
    out.insert(QStringLiteral("width"), defaults.width);
    return out;
}

QVariantMap defaultShadowMap() {
    const openshow::domain::TextShadow defaults;
    QVariantMap out;
    out.insert(QStringLiteral("enabled"), defaults.enabled);
    out.insert(QStringLiteral("color"), QString::fromStdString(defaults.color));
    out.insert(QStringLiteral("opacity"), defaults.opacity);
    out.insert(QStringLiteral("offsetX"), defaults.offsetX);
    out.insert(QStringLiteral("offsetY"), defaults.offsetY);
    out.insert(QStringLiteral("blurSigma"), defaults.blurSigma);
    return out;
}

QVariant effectDefault(const QString& field) {
    const QString member = field.mid(QLatin1String("stroke").size());
    const QVariantMap defaults = field.startsWith(QLatin1String("stroke"))
                                     ? defaultStrokeMap()
                                     : defaultShadowMap();
    if (member == QLatin1String("Enabled")) {
        return defaults.value(QStringLiteral("enabled"));
    }
    if (member == QLatin1String("Color")) {
        return defaults.value(QStringLiteral("color"));
    }
    return defaults.value(member.at(0).toLower() + member.mid(1));
}

// A style leaf's answer when its target is silent — the resolver's own
// floor, read from the domain's constants.
QVariant styleDefault(const QString& field) {
    if (field == QLatin1String("fontWeight")) {
        return openshow::domain::kDefaultFontWeight;
    }
    if (field == QLatin1String("fontStyle")) {
        return QStringLiteral("normal");
    }
    if (field == QLatin1String("fitMode")) {
        return QStringLiteral("shrink");
    }
    return openshow::domain::kDefaultMinFontSize;  // minFontSize
}

QVariantMap boxFromSample(const QVariantMap& sample) {
    QVariantMap box;
    if (sample.value(QStringLiteral("ok")).toBool()) {
        box.insert(QStringLiteral("x"), sample.value(QStringLiteral("boxX")));
        box.insert(QStringLiteral("y"), sample.value(QStringLiteral("boxY")));
        box.insert(QStringLiteral("width"), sample.value(QStringLiteral("boxWidth")));
        box.insert(QStringLiteral("height"), sample.value(QStringLiteral("boxHeight")));
    }
    return box;
}

QVariantMap rectValue(const QVariantMap& rect) {
    QVariantMap out;
    out.insert(QStringLiteral("x"), rect.value(QStringLiteral("x")));
    out.insert(QStringLiteral("y"), rect.value(QStringLiteral("y")));
    out.insert(QStringLiteral("width"), rect.value(QStringLiteral("width")));
    out.insert(QStringLiteral("height"), rect.value(QStringLiteral("height")));
    return out;
}

// The rectangle a commit takes: the four numbers, one unit. A shape this
// cannot even read is refused before any verb runs; the DOMAIN's vetting
// still decides what lands.
bool rectFrom(const QVariant& value, double* x, double* y, double* width, double* height) {
    if (value.typeId() != QMetaType::QVariantMap) {
        return false;
    }
    const QVariantMap map = value.toMap();
    bool okX = false, okY = false, okW = false, okH = false;
    *x = map.value(QStringLiteral("x")).toDouble(&okX);
    *y = map.value(QStringLiteral("y")).toDouble(&okY);
    *width = map.value(QStringLiteral("width")).toDouble(&okW);
    *height = map.value(QStringLiteral("height")).toDouble(&okH);
    return okX && okY && okW && okH;
}

}  // namespace

InspectorAdapter::InspectorAdapter(QObject* parent) : QObject(parent) {}

void InspectorAdapter::connectEdges() {
    if (fDocument != nullptr) {
        disconnect(fDocument, &DocumentController::documentsChanged, this,
                   &InspectorAdapter::changed);
        disconnect(fDocument, &DocumentController::navigationChanged, this,
                   &InspectorAdapter::changed);
        connect(fDocument, &DocumentController::documentsChanged, this,
                &InspectorAdapter::changed);
        connect(fDocument, &DocumentController::navigationChanged, this,
                &InspectorAdapter::changed);
    }
}

void InspectorAdapter::setDocument(DocumentController* document) {
    if (fDocument == document) {
        return;
    }
    fDocument = document;
    // The slide target follows the cursor; the theme/context targets live in
    // the documents. Both edges re-open every read this adapter answers, in
    // whichever order a page sets the two properties.
    connectEdges();
    emit documentChanged();
}

void InspectorAdapter::setTarget(const QVariantMap& target) {
    if (fTarget == target) {
        return;
    }
    fTarget = target;
    connectEdges();
    emit targetChanged();
    // The parts re-sync their displayed rows off changed(); a rail that
    // switched WHICH record it edits has moved every answer they show,
    // even though no document did.
    emit changed();
}

QVariantMap InspectorAdapter::fieldState(const QString& field) const {
    if (fDocument == nullptr) {
        return nothing(QStringLiteral("no document session"));
    }
    const QString kind = fTarget.value(QStringLiteral("kind")).toString();
    const QString id = fTarget.value(QStringLiteral("id")).toString();
    const QString view = fTarget.value(QStringLiteral("view")).toString();
    const QString name = canonicalField(field);

    if (kind == QLatin1String("theme")) {
        QVariantMap row;
        QVariantMap floor;
        for (const QVariant& entry : fDocument->themes()) {
            if (entry.toMap().value(QStringLiteral("builtin")).toBool()) floor = entry.toMap();
            if (entry.toMap().value(QStringLiteral("id")).toString() == id) {
                row = entry.toMap();
            }
        }
        if (row.isEmpty()) {
            return nothing(QStringLiteral("no theme '%1'").arg(id));
        }
        // The compiled fixture is the value authority. In particular a silent
        // style field inherits the editable Default theme before the contract
        // floor; substituting 400/disabled here can contradict the picture.
        const QVariantMap sample = fDocument->themeSampleAppearance(id);
        auto styleSource = [&](const QString& record, bool own) {
            return own ? QStringLiteral("this theme")
                       : (floor.value(record + QLatin1String("Set")).toBool()
                              ? QStringLiteral("the Default theme")
                              : QStringLiteral("the renderer default"));
        };
        if (isBaseField(name)) {
            const QVariant own = row.value(name);
            const bool ownAnswers = name == QLatin1String("lineSpacing") ||
                (name == QLatin1String("fontSize") ? own.toDouble() != 0
                                                  : !own.toString().isEmpty());
            const QVariant lower = floor.value(name);
            const bool floorAnswers = name == QLatin1String("fontSize")
                                          ? lower.toDouble() != 0 : !lower.toString().isEmpty();
            return shape(sample.value(name, own), own, true,
                         ownAnswers ? QStringLiteral("this theme")
                                    : (floorAnswers ? QStringLiteral("the Default theme")
                                                    : QStringLiteral("the renderer default")),
                         true, false);
        }
        if (name == QLatin1String("placement")) {
            const bool isSet = row.value(QStringLiteral("placementSet")).toBool();
            const QVariant own = isSet ? row.value(QStringLiteral("placement")) : QVariant();
            QVariant resolved = own;
            if (!isSet) {
                resolved = boxFromSample(sample);
            }
            return shape(resolved, own, isSet,
                         isSet ? QStringLiteral("this theme")
                               : (floor.value(QStringLiteral("placementSet")).toBool()
                                      ? QStringLiteral("the Default theme")
                                      : QStringLiteral("the renderer default")),
                         true, isSet);
        }
        if (isStyleLeaf(name)) {
            const bool isSet = row.value(name + QLatin1String("Set")).toBool();
            return shape(sample.value(name, styleDefault(name)),
                         isSet ? row.value(name) : QVariant(), isSet,
                         styleSource(name, isSet),
                         true, isSet);
        }
        if (name == QLatin1String("stroke") || name == QLatin1String("shadow")) {
            const bool isSet = row.value(name + QLatin1String("Set")).toBool();
            return shape(sample.value(name),
                         isSet ? row.value(name) : QVariant(), isSet,
                         styleSource(name, isSet),
                         true, isSet);
        }
        if (isStrokeMember(name) || isShadowMember(name)) {
            // Members belong to their WHOLE record: sparse sets speak for
            // the record, and an inherit removes the record, not a member.
            const QString record = name.startsWith(QLatin1String("stroke"))
                                       ? QStringLiteral("stroke")
                                       : QStringLiteral("shadow");
            const bool isSet = row.value(record + QLatin1String("Set")).toBool();
            QVariant own;
            if (isSet) {
                own = row.value(record).toMap().value(
                    name.at(record.size()).toLower() + name.mid(record.size() + 1));
            }
            const QString member = name.at(record.size()).toLower() + name.mid(record.size() + 1);
            const QVariant value = sample.value(record).toMap().value(member, effectDefault(name));
            return shape(value, own, isSet,
                         styleSource(record, isSet),
                         true, false);
        }
        if (name == QLatin1String("transition")) {
            const bool isSet = row.value(QStringLiteral("transitionSet")).toBool();
            const QVariant own = isSet ? row.value(QStringLiteral("transition")) : QVariant();
            // Silence under a theme falls to the service floor.
            return shape(isSet ? own : QVariant(fDocument->serviceDefaultTransition()), own,
                         isSet,
                         isSet ? QStringLiteral("this theme") : QStringLiteral("the service"),
                         true, isSet);
        }
        return nothing(QStringLiteral("'%1' is not an inspector field").arg(field));
    }

    if (kind == QLatin1String("context")) {
        const QVariantMap row = fDocument->themeContext(id, view);
        if (!row.value(QStringLiteral("set")).toBool()) {
            return nothing(QStringLiteral("theme '%1' carries no view '%2'").arg(id, view));
        }
        const QVariantMap sample = fDocument->themeSampleAppearance(id, view);
        QVariantMap themeRow;
        for (const QVariant& entry : fDocument->themes()) {
            if (entry.toMap().value(QStringLiteral("id")).toString() == id) {
                themeRow = entry.toMap();
                break;
            }
        }
        if (name == QLatin1String("fontFamily") || name == QLatin1String("fontSize") ||
            name == QLatin1String("align") || name == QLatin1String("lineSpacing")) {
            const bool isSet = row.value(name + QLatin1String("Set")).toBool();
            return shape(sample.value(name), isSet ? row.value(name) : QVariant(), isSet,
                         isSet ? QStringLiteral("this view") : QStringLiteral("this theme"),
                         true, isSet);
        }
        if (name == QLatin1String("placement")) {
            // One unit (task TV.1): the view answers with a box OR the
            // legacy inset; either is an own value, and writing a box
            // retires a stored inset in the same edit.
            const bool boxSet = row.value(QStringLiteral("boxSet")).toBool();
            const bool insetSet = row.value(QStringLiteral("insetSet")).toBool();
            QVariant own;
            if (boxSet) {
                own = rectValue(row);
            } else if (insetSet) {
                own = row.value(QStringLiteral("inset"));
            }
            const QVariant resolved = boxFromSample(sample);
            return shape(resolved, own, boxSet || insetSet,
                         (boxSet || insetSet) ? QStringLiteral("this view")
                                              : QStringLiteral("this theme"),
                         true, boxSet || insetSet);
        }
        if (name == QLatin1String("transition")) {
            const bool ownSet = row.value(QStringLiteral("transitionSet")).toBool();
            const QVariant own = ownSet ? row.value(QStringLiteral("transition")) : QVariant();
            const bool themeSet = themeRow.value(QStringLiteral("transitionSet")).toBool();
            QVariant value = own;
            QString source = QStringLiteral("this view");
            if (!ownSet) {
                value = themeSet ? themeRow.value(QStringLiteral("transition"))
                                 : QVariant(fDocument->serviceDefaultTransition());
                source = themeSet ? QStringLiteral("this theme") : QStringLiteral("the service");
            }
            return shape(value, own, ownSet, source, true, ownSet);
        }
        // Until TV.3 a text context stores typography and a box ONLY. The
        // controls must STATE where the answer comes from and disable —
        // never write the Songs base from a Bible rail.
        if (isBaseField(name)) {
            const QVariant inherited =
                name == QLatin1String("textColor") || name == QLatin1String("backgroundColor") ||
                        name == QLatin1String("backgroundGradient")
                    ? themeRow.value(name)
                    : QVariant(sample.value(name));
            QVariantMap out = shape(inherited, QVariant(), false, QStringLiteral("this theme"),
                                    false, false);
            out.insert(QStringLiteral("reason"),
                       QStringLiteral("a text context cannot store %1 yet (TV.3)").arg(name));
            return out;
        }
        if (isStyleLeaf(name) || name == QLatin1String("stroke") ||
            name == QLatin1String("shadow")) {
            const bool themeSet = themeRow.value(name + QLatin1String("Set")).toBool();
            QVariant inherited = themeRow.value(name);
            if (!themeSet) {
                inherited = (name == QLatin1String("stroke"))    ? defaultStrokeMap()
                            : (name == QLatin1String("shadow"))  ? defaultShadowMap()
                                                                : styleDefault(name);
            }
            QVariantMap out = shape(inherited, QVariant(), false, QStringLiteral("this theme"),
                                    false, false);
            out.insert(QStringLiteral("reason"),
                       QStringLiteral("a text context cannot store %1 yet (TV.3)").arg(name));
            return out;
        }
        if (isStrokeMember(name) || isShadowMember(name)) {
            QVariantMap out = shape(QVariant(), QVariant(), false, QStringLiteral("this theme"),
                                    false, false);
            out.insert(QStringLiteral("reason"),
                       QStringLiteral("a text context cannot store %1 yet (TV.3)").arg(name));
            return out;
        }
        return nothing(QStringLiteral("'%1' is not an inspector field").arg(field));
    }

    if (kind == QLatin1String("slide")) {
        const QVariantMap appearance = fDocument->slideAppearance();
        if (appearance.isEmpty()) {
            return nothing(QStringLiteral("no current slide"));
        }
        const auto row = [&appearance](const QString& key) {
            return appearance.value(key).toMap();
        };
        if (name == QLatin1String("transition")) {
            QVariantMap out = shape(QVariant(), QVariant(), false, QString(), false, false);
            out.insert(QStringLiteral("reason"),
                       QStringLiteral("a slide never carries its own movement"));
            return out;
        }
        const bool isMember = isStrokeMember(name) || isShadowMember(name);
        const QString record =
            isMember ? (name.startsWith(QLatin1String("stroke")) ? QStringLiteral("stroke")
                                                                 : QStringLiteral("shadow"))
                     : name;
        if (!appearance.contains(record)) {
            return nothing(QStringLiteral("'%1' is not an inspector field").arg(field));
        }
        const QVariantMap entry = row(record);
        const bool isSet = entry.value(QStringLiteral("customized")).toBool();
        const QVariant resolved = entry.value(QStringLiteral("value"));
        if (isMember) {
            const QString member = name.at(record.size()).toLower() +
                                   name.mid(record.size() + 1);
            const QVariant value =
                resolved.toMap().value(member, effectDefault(name));
            return shape(value, isSet ? value : QVariant(), isSet,
                         isSet ? QStringLiteral("this slide") : QStringLiteral("the theme"),
                         true, false);  // an effect inherits only whole
        }
        const QVariant own = isSet ? resolved : QVariant();
        return shape(resolved, own, isSet,
                     isSet ? QStringLiteral("this slide") : QStringLiteral("the theme"),
                     true, isSet);
    }

    if (kind == QLatin1String("service")) {
        if (name == QLatin1String("transition")) {
            const QVariant value = fDocument->serviceDefaultTransition();
            return shape(value, value, true, QStringLiteral("the service"), true,
                         false);  // concrete floor: nothing under it to inherit
        }
        // A real target that simply answers one question; the controls ask
        // no other of it.
        QVariantMap out = shape(QVariant(), QVariant(), false, QString(), false, false);
        out.insert(QStringLiteral("reason"),
                   QStringLiteral("the service only answers how scenes move"));
        return out;
    }

    return nothing(QStringLiteral("unknown inspector target '%1'").arg(kind));
}

bool InspectorAdapter::commit(const QString& field, const QVariant& value) {
    if (fDocument == nullptr) {
        emit refused(field, QStringLiteral("no document session"));
        return false;
    }
    const QString kind = fTarget.value(QStringLiteral("kind")).toString();
    const QString id = fTarget.value(QStringLiteral("id")).toString();
    const QString view = fTarget.value(QStringLiteral("view")).toString();
    const QString name = canonicalField(field);

    // NOTE: there is deliberately NO "value unchanged" guard here. A commit
    // that equals the RESOLVED value can still be a materialise — the view
    // seeding its own Crossfade off what the show was already drawing is
    // exactly that write (TR's pinned rule). Where an unchanged focus-out
    // must not write (editingFinished fires on a bare focus loss), the
    // TEXT rows say so themselves before they ever reach this door.
    if (kind == QLatin1String("theme")) {
        QVariantMap row;
        for (const QVariant& entry : fDocument->themes()) {
            if (entry.toMap().value(QStringLiteral("id")).toString() == id) {
                row = entry.toMap();
                break;
            }
        }
        if (row.isEmpty()) {
            emit refused(field, QStringLiteral("no theme '%1'").arg(id));
            return false;
        }
        bool done = false;
        bool ok = false;
        if (isBaseField(name)) {
            // The base record is concrete and WHOLE (editTheme's contract:
            // leaving backgroundGradient out would silently flatten it), so
            // one field's commit restates the row with that field replaced.
            QString textColor = row.value(QStringLiteral("textColor")).toString();
            QString backgroundColor = row.value(QStringLiteral("backgroundColor")).toString();
            QString backgroundGradient =
                row.value(QStringLiteral("backgroundGradient")).toString();
            QString fontFamily = row.value(QStringLiteral("fontFamily")).toString();
            double fontSize = row.value(QStringLiteral("fontSize")).toDouble();
            QString align = row.value(QStringLiteral("align")).toString();
            double lineSpacing = row.value(QStringLiteral("lineSpacing")).toDouble();
            if (name == QLatin1String("textColor")) {
                textColor = value.toString();
            } else if (name == QLatin1String("backgroundColor")) {
                backgroundColor = value.toString();
            } else if (name == QLatin1String("backgroundGradient")) {
                backgroundGradient = value.toString();
            } else if (name == QLatin1String("fontFamily")) {
                fontFamily = value.toString();
            } else if (name == QLatin1String("fontSize")) {
                fontSize = value.toDouble();
            } else if (name == QLatin1String("align")) {
                align = value.toString();
            } else {
                lineSpacing = value.toDouble();
            }
            ok = fDocument->editTheme(id, row.value(QStringLiteral("name")).toString(), textColor,
                                      backgroundColor, backgroundGradient, fontFamily, fontSize,
                                      align, lineSpacing);
            done = true;
        } else if (name == QLatin1String("placement")) {
            double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
            if (!rectFrom(value, &x, &y, &width, &height)) {
                emit refused(field, QStringLiteral("a box is four numbers, one unit"));
                return false;
            }
            ok = fDocument->setThemePlacement(id, x, y, width, height);
            done = true;
        } else if (name == QLatin1String("transition")) {
            if (value.typeId() != QMetaType::QVariantMap) {
                emit refused(field, QStringLiteral("a transition is a record"));
                return false;
            }
            ok = fDocument->setThemeTransition(id, value.toMap());
            done = true;
        } else if (isStyleLeaf(name) || isStrokeMember(name) || isShadowMember(name) ||
                   name == QLatin1String("stroke") || name == QLatin1String("shadow")) {
            // One field through the one-field verb: the domain vetting and
            // the contract defaults (an enabling starts from what the
            // audience draws today) are the theme's, not this file's.
            ok = fDocument->setThemeTextStyleField(id, name, value);
            done = true;
        }
        if (!done) {
            emit refused(field, QStringLiteral("'%1' is not an inspector field").arg(field));
            return false;
        }
        if (!ok) {
            emit refused(field, QStringLiteral("the theme refused %1; the record still "
                                               "answers as it did").arg(field));
        }
        return ok;
    }

    if (kind == QLatin1String("context")) {
        const QVariantMap row = fDocument->themeContext(id, view);
        if (!row.value(QStringLiteral("set")).toBool()) {
            emit refused(field, QStringLiteral("theme '%1' carries no view '%2'").arg(id, view));
            return false;
        }
        if (name == QLatin1String("transition")) {
            if (value.typeId() != QMetaType::QVariantMap) {
                emit refused(field, QStringLiteral("a transition is a record"));
                return false;
            }
            const bool ok = fDocument->setTextContextTransition(id, view, value.toMap());
            if (!ok) {
                emit refused(field, QStringLiteral("the view refused its transition"));
            }
            return ok;
        }
        const bool isTypography = name == QLatin1String("fontFamily") ||
                                  name == QLatin1String("fontSize") ||
                                  name == QLatin1String("align") ||
                                  name == QLatin1String("lineSpacing");
        if (!isTypography && name != QLatin1String("placement")) {
            emit refused(field, QStringLiteral("a text context cannot store %1 yet (TV.3); "
                                               "the themes answer stays untouched").arg(name));
            return false;
        }
        // The whole-entry merge, in ONE place (the Theme Editor used to do
        // this arithmetic in QML): absent keys are silence, so the entry's
        // own set is read back and the committed field replaces its member.
        // A box write retires a stored inset, and an inset write retires
        // the box (task TV.1 — the view answers with one box).
        QVariantMap fields;
        if (row.value(QStringLiteral("fontFamilySet")).toBool()) {
            fields.insert(QStringLiteral("fontFamily"), row.value(QStringLiteral("fontFamily")));
        }
        if (row.value(QStringLiteral("fontSizeSet")).toBool()) {
            fields.insert(QStringLiteral("fontSize"), row.value(QStringLiteral("fontSize")));
        }
        if (row.value(QStringLiteral("lineSpacingSet")).toBool()) {
            fields.insert(QStringLiteral("lineSpacing"), row.value(QStringLiteral("lineSpacing")));
        }
        if (row.value(QStringLiteral("alignSet")).toBool()) {
            fields.insert(QStringLiteral("align"), row.value(QStringLiteral("align")));
        }
        if (name == QLatin1String("placement")) {
            const QVariantMap given = value.toMap();
            if (given.contains(QStringLiteral("inset"))) {
                fields.insert(QStringLiteral("inset"), given.value(QStringLiteral("inset")));
            } else {
                double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
                if (!rectFrom(value, &x, &y, &width, &height)) {
                    emit refused(field, QStringLiteral("a box is four numbers, one unit"));
                    return false;
                }
                QVariantMap box;
                box.insert(QStringLiteral("x"), x);
                box.insert(QStringLiteral("y"), y);
                box.insert(QStringLiteral("width"), width);
                box.insert(QStringLiteral("height"), height);
                fields.insert(QStringLiteral("box"), box);
            }
        } else {
            // The committed field replaces its member; the rest of the row
            // carries over exactly as stored.
            fields.insert(name, value);
            // The typography path keeps whichever box spelling is stored.
            if (row.value(QStringLiteral("boxSet")).toBool()) {
                fields.insert(QStringLiteral("box"), rectValue(row));
            } else if (row.value(QStringLiteral("insetSet")).toBool()) {
                fields.insert(QStringLiteral("inset"), row.value(QStringLiteral("inset")));
            }
        }
        if (!fDocument->applyThemeContext(id, view, fields)) {
            emit refused(field, QStringLiteral("the view refused %1; the fields go back "
                                               "to the record").arg(field));
            return false;
        }
        return true;
    }

    if (kind == QLatin1String("slide")) {
        if (name == QLatin1String("transition")) {
            emit refused(field, QStringLiteral("a slide never carries its own movement"));
            return false;
        }
        if (isStrokeMember(name) || isShadowMember(name)) {
            // Members merge once through the record verb: the row already
            // carries the concrete record, so this adds one member and the
            // controller's vetting decides.
            const QString record = name.startsWith(QLatin1String("stroke"))
                                       ? QStringLiteral("stroke")
                                       : QStringLiteral("shadow");
            const QVariantMap appearance = fDocument->slideAppearance();
            const QVariantMap entry = appearance.value(record).toMap();
            if (entry.isEmpty()) {
                emit refused(field, QStringLiteral("no current slide"));
                return false;
            }
            QVariantMap merged = entry.value(QStringLiteral("value")).toMap();
            merged.insert(name.at(record.size()).toLower() + name.mid(record.size() + 1),
                          value);
            const bool ok = fDocument->customizeCurrentSlide(record, merged);
            if (!ok) {
                emit refused(field, QStringLiteral("the slide refused %1").arg(field));
            }
            return ok;
        }
        if (name == QLatin1String("placement")) {
            // The slide's box writes through the box verb — a geometry move
            // and a typed commit record the SAME one placement customization
            // the drag already makes.
            double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
            if (!rectFrom(value, &x, &y, &width, &height)) {
                emit refused(field, QStringLiteral("a box is four numbers, one unit"));
                return false;
            }
            const bool ok = fDocument->setTextBoxGeometry(x, y, width, height);
            if (!ok) {
                emit refused(field, QStringLiteral("the slide refused its box"));
            }
            return ok;
        }
        const bool ok = fDocument->customizeCurrentSlide(name, value);
        if (!ok) {
            emit refused(field, QStringLiteral("the slide refused %1").arg(field));
        }
        return ok;
    }

    if (kind == QLatin1String("service")) {
        if (name != QLatin1String("transition")) {
            emit refused(field, QStringLiteral("the service only answers how scenes move"));
            return false;
        }
        const QVariantMap record = value.toMap();
        const bool ok = fDocument->setServiceDefaultTransition(
            record.value(QStringLiteral("kind")).toString(),
            record.value(QStringLiteral("textDurationMs")).toInt(),
            record.value(QStringLiteral("mediaDurationMs")).toInt());
        if (!ok) {
            emit refused(field, QStringLiteral("the service refused that transition"));
        }
        return ok;
    }

    emit refused(field, QStringLiteral("unknown inspector target '%1'").arg(kind));
    return false;
}

bool InspectorAdapter::reset(const QString& field) {
    if (fDocument == nullptr) {
        emit refused(field, QStringLiteral("no document session"));
        return false;
    }
    const QString kind = fTarget.value(QStringLiteral("kind")).toString();
    const QString id = fTarget.value(QStringLiteral("id")).toString();
    const QString view = fTarget.value(QStringLiteral("view")).toString();
    const QString name = canonicalField(field);

    if (kind == QLatin1String("theme")) {
        if (isBaseField(name)) {
            emit refused(field, QStringLiteral("a theme is the root of its own colours "
                                               "and typography"));
            return false;
        }
        bool ok = false;
        if (name == QLatin1String("placement")) {
            ok = fDocument->clearThemePlacement(id);
        } else if (name == QLatin1String("transition")) {
            ok = fDocument->setThemeTransition(id, QVariantMap());  // silence is a removal
        } else if (isStrokeMember(name) || isShadowMember(name)) {
            emit refused(field, QStringLiteral("an effect inherits only whole — clear "
                                               "'stroke' or 'shadow'"));
            return false;
        } else if (isStyleLeaf(name) || name == QLatin1String("stroke") ||
                   name == QLatin1String("shadow")) {
            ok = fDocument->setThemeTextStyleField(id, name, QVariant());
        } else {
            emit refused(field, QStringLiteral("'%1' is not an inspector field").arg(field));
            return false;
        }
        if (!ok) {
            emit refused(field, QStringLiteral("there was nothing to inherit from for %1")
                                    .arg(field));
        }
        return ok;
    }

    if (kind == QLatin1String("context")) {
        const QVariantMap row = fDocument->themeContext(id, view);
        if (!row.value(QStringLiteral("set")).toBool()) {
            emit refused(field, QStringLiteral("theme '%1' carries no view '%2'").arg(id, view));
            return false;
        }
        if (name == QLatin1String("transition")) {
            const bool ok = fDocument->setTextContextTransition(id, view, QVariantMap());
            if (!ok) {
                emit refused(field, QStringLiteral("the view carried no transition of its own"));
            }
            return ok;
        }
        const bool isTypography = name == QLatin1String("fontFamily") ||
                                  name == QLatin1String("fontSize") ||
                                  name == QLatin1String("align") ||
                                  name == QLatin1String("lineSpacing");
        if (!isTypography && name != QLatin1String("placement")) {
            emit refused(field, QStringLiteral("a text context never stored %1").arg(name));
            return false;
        }
        // The same centralised merge with the field left OUT — the entry
        // keeps what it still carries and goes silent on this one.
        QVariantMap fields;
        if (name != QLatin1String("fontFamily") && row.value(QStringLiteral("fontFamilySet")).toBool()) {
            fields.insert(QStringLiteral("fontFamily"), row.value(QStringLiteral("fontFamily")));
        }
        if (name != QLatin1String("fontSize") && row.value(QStringLiteral("fontSizeSet")).toBool()) {
            fields.insert(QStringLiteral("fontSize"), row.value(QStringLiteral("fontSize")));
        }
        if (name != QLatin1String("lineSpacing") &&
            row.value(QStringLiteral("lineSpacingSet")).toBool()) {
            fields.insert(QStringLiteral("lineSpacing"), row.value(QStringLiteral("lineSpacing")));
        }
        if (name != QLatin1String("align") && row.value(QStringLiteral("alignSet")).toBool()) {
            fields.insert(QStringLiteral("align"), row.value(QStringLiteral("align")));
        }
        if (name != QLatin1String("placement")) {
            if (row.value(QStringLiteral("boxSet")).toBool()) {
                fields.insert(QStringLiteral("box"), rectValue(row));
            } else if (row.value(QStringLiteral("insetSet")).toBool()) {
                fields.insert(QStringLiteral("inset"), row.value(QStringLiteral("inset")));
            }
        }
        if (!fDocument->applyThemeContext(id, view, fields)) {
            emit refused(field, QStringLiteral("the view refused to let %1 go").arg(field));
            return false;
        }
        return true;
    }

    if (kind == QLatin1String("slide")) {
        if (name == QLatin1String("transition")) {
            emit refused(field, QStringLiteral("a slide never carries its own movement"));
            return false;
        }
        if (isStrokeMember(name) || isShadowMember(name)) {
            emit refused(field, QStringLiteral("an effect inherits only whole — clear "
                                               "'stroke' or 'shadow'"));
            return false;
        }
        const bool ok = fDocument->revertCurrentSlideField(name);
        if (!ok) {
            emit refused(field, QStringLiteral("this slide has no %1 to revert").arg(field));
        }
        return ok;
    }

    if (kind == QLatin1String("service")) {
        emit refused(field, QStringLiteral("the service default is concrete — there is "
                                           "nothing under it to inherit"));
        return false;
    }

    emit refused(field, QStringLiteral("unknown inspector target '%1'").arg(kind));
    return false;
}

QStringList InspectorAdapter::fields() const {
    const QString kind = fTarget.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("theme")) {
        QStringList out;
        for (const char* field : kBaseFields) {
            out << QLatin1String(field);
        }
        out << QStringLiteral("placement") << QStringLiteral("transition");
        for (const char* field : kStyleLeaves) {
            out << QLatin1String(field);
        }
        out << QStringLiteral("stroke") << QStringLiteral("shadow");
        for (const char* field : kStrokeMembers) {
            out << QLatin1String(field);
        }
        for (const char* field : kShadowMembers) {
            out << QLatin1String(field);
        }
        return out;
    }
    if (kind == QLatin1String("context")) {
        return {QStringLiteral("fontFamily"), QStringLiteral("fontSize"),
                QStringLiteral("align"), QStringLiteral("lineSpacing"),
                QStringLiteral("placement"), QStringLiteral("transition")};
    }
    if (kind == QLatin1String("slide")) {
        return {QStringLiteral("textColor"), QStringLiteral("backgroundColor"),
                QStringLiteral("backgroundGradient"), QStringLiteral("fontFamily"),
                QStringLiteral("fontSize"), QStringLiteral("align"),
                QStringLiteral("lineSpacing"), QStringLiteral("placement"),
                QStringLiteral("fontWeight"), QStringLiteral("fontStyle"),
                QStringLiteral("fitMode"), QStringLiteral("minFontSize"),
                QStringLiteral("stroke"), QStringLiteral("shadow")};
    }
    if (kind == QLatin1String("service")) {
        return {QStringLiteral("transition")};
    }
    return {};
}

}  // namespace openshow::ui
