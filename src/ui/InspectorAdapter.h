#pragma once

#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// The QML_ELEMENT moc needs the property type complete, not forward-declared.
#include "DocumentController.h"

namespace openshow::ui {

// The inspector read model and write router (task CS.4, control-sets plan).
// Shared style controls never walk theme chains — they read ONE shape and
// emit ONE verb pair, and this class is the single place that shape is built
// and that pair is translated. The reads are the controller's existing ones
// (`themeSampleAppearance`, the `themes()` rows, `themeContext()`,
// `slideAppearance`, `serviceDefaultTransition`); the writes stay the host's
// existing verbs, separate, atomic and undoable, so centralising the routing
// changes no contract the domain already keeps.
//
// `target` is explicit identity, never "whatever is selected":
//
//   {kind: "theme",   id: <themeId>}                      the Songs look
//   {kind: "context", id: <themeId>, view: "scripture"}   one Bible view
//   {kind: "slide"}                                       the current slide
//   {kind: "service"}                                     the movement floor
//
// `fieldState(field)` answers, for exactly that target:
//
//   {value,       // what the audience draws today (resolved)
//    ownValue,    // what THIS target stores itself, when isSet
//    isSet,       // does this target carry the field
//    source,      // who answers when silence falls: "this theme",
//                 // "this view", "this slide", "the service",
//                 // "the renderer default", "" (nobody does)
//    editable,    // can this target store this field at all (capability)
//    canReset}    // is there an own value to remove (the Inherit button)
//
// plus `reason` where something is not storable and `ok` false when the
// target itself does not exist (unknown theme id, no such context, no
// current slide).
//
// `commit(field, value)` / `reset(field)` route per target:
//
//   theme    base fields restate the record through `editTheme`; style
//            leaves and whole effect records go through
//            `setThemeTextStyleField` (null = inherit); `box` is
//            `setThemePlacement`/`clearThemePlacement`; `transition` is
//            `setThemeTransition`.
//   context  typography and box merge the WHOLE own entry and write it
//            through `applyThemeContext` — the one place-task the Theme
//            Editor used to do in QML; a box write retires a stored inset
//            in the same edit; `transition` is `setTextContextTransition`
//            and colours/effects stay un-storable until TV.3, stated not
//            silently dropped.
//   slide    `customizeCurrentSlide` / `revertCurrentSlideField`; effect
//            MEMBERS merge into the record the controller row already
//            carries (an effect inherits only whole), `box` writes
//            `setTextBoxGeometry` — the same single placement customization
//            a drag makes — and a slide carries no transition of its own.
//   service  `setServiceDefaultTransition` only; the floor is concrete, so
//            reset names why there is nothing to inherit.
//
// A refusal changes nothing (the verbs already guarantee that), emits
// `refused(field, reason)` and returns false — an operator reads the same
// sentence the status line keeps.
class InspectorAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(DocumentController* document READ document WRITE setDocument
                   NOTIFY documentChanged)
    Q_PROPERTY(QVariantMap target READ target WRITE setTarget NOTIFY targetChanged)

public:
    explicit InspectorAdapter(QObject* parent = nullptr);

    DocumentController* document() const { return fDocument; }
    void setDocument(DocumentController* document);

    QVariantMap target() const { return fTarget; }
    void setTarget(const QVariantMap& target);

    Q_INVOKABLE QVariantMap fieldState(const QString& field) const;
    Q_INVOKABLE bool commit(const QString& field, const QVariant& value);
    Q_INVOKABLE bool reset(const QString& field);
    // The field spellings THIS target can commit, for a page that builds its
    // sections from capability rather than from guesses.
    Q_INVOKABLE QStringList fields() const;

signals:
    void documentChanged();
    void targetChanged();
    // Something an inspector reads may have moved: the documents, or the
    // cursor the slide target follows.
    void changed();
    void refused(const QString& field, const QString& reason);

private:
    void connectEdges();
    DocumentController* fDocument = nullptr;
    QVariantMap fTarget;  // {kind, id, view}
};

}  // namespace openshow::ui
