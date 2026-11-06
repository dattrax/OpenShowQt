/*
 * DocumentController: the document session behind the workspace (tasks
 * 3.3, 4.2).
 *
 * The frontend owns the editable documents (PLAN.md's ownership split), so
 * this is the object that holds them: a domain::Documents with its History
 * (task 2.3 snapshots) and the Autosave seam, exposed to QML with just
 * enough surface to author and arrange — songs, their slides and text,
 * compiled scene JSON ready for WorkerController::presentSceneJson, and
 * since task 4.2 the service playlist (ordered show references the drawer
 * adds and reorders), the searchable library view, and the media registry.
 * Views stay plain QVariantLists, and every mutation runs through History
 * so undo covers everything the workspace does.
 *
 * Text editing is debounced: keystrokes park a pending value, and one
 * history entry (and one autosave candidate) is committed when typing
 * pauses or before any other operation runs — a snapshot per character
 * would exhaust the undo stack and mean nothing to an operator.
 *
 * Since task 6.19b1 the bytes are not its concern: DocumentPersistence owns
 * the scratch, the executor, the decision protocol around an explicit save or
 * load and everything the operator is told about durability. The controller
 * stays the one owner of the documents and their history — it decides what a
 * decision does to the model (which paths history's snapshots name, where
 * navigation lands, whether the session is dirty) and asks for the write.
 *
 * Qt Core only — no GUI — so the headless fixtures drive the very class
 * the panel binds to. All of it lives on the UI thread.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include <boost/json.hpp>

#include "openshow/domain/documents.h"
#include "openshow/domain/history.h"
#include "openshow/domain/storage.h"

#include "DocumentPersistence.h"
#include "DocumentViews.h"
#include "PowerPointImport.h"

namespace openshow::ui {

class DocumentController : public QObject {
    Q_OBJECT
    // Songs of the session: [{id, name, slideCount}], creation order.
    Q_PROPERTY(QVariantList songs READ songs NOTIFY documentsChanged)
    // Slides of the current song: [{id, index, preview}], presentation
    // order; empty with no song selected.
    Q_PROPERTY(QVariantList slides READ slides NOTIFY documentsChanged)
    Q_PROPERTY(QString currentSongId READ currentSongId NOTIFY navigationChanged)
    Q_PROPERTY(int currentSlide READ currentSlide NOTIFY navigationChanged)
    // The current slide's lyrics, lines joined with '\n'. Since 6.17a2 this
    // is the SELECTED box's words, not the slide's one tuple — with nothing
    // selected it is box 0, which is the box every pre-6.17a slide has.
    Q_PROPERTY(QString currentSlideText READ currentSlideText NOTIFY slideTextChanged)
    // The selected text box of the current slide: its index in DRAW order,
    // -1 when the slide has no boxes. The canvas puts handles on this box
    // only; every box-scoped edit below lands on it.
    Q_PROPERTY(int currentBox READ currentBox NOTIFY boxSelectionChanged)
    // The current slide's boxes in draw order: [{id, index, kind, assetId,
    // x, y, width, height}]. The canvas draws one outline per entry, so what
    // the operator sees framed is every box the worker will paint, in the
    // order it paints — and the kind (task 6.17b) tells a picture's outline
    // from a text box's without guessing from empty words.
    Q_PROPERTY(QVariantList currentSlideBoxes READ currentSlideBoxes
               NOTIFY documentsChanged)
    // What kind of box the selection is on ("text", "image", or "" with
    // nothing selected) and the asset an image box shows, in the same shape
    // as an assets() row (empty map when the selection is a text box). The
    // inspector reads these two to decide between the words field and the
    // picture's name (task 6.17b2); navigation, undo and every stack edit
    // re-emit the signal, so the field and the canvas outline always
    // describe the same box.
    Q_PROPERTY(QString currentBoxKind READ currentBoxKind NOTIFY boxSelectionChanged)
    Q_PROPERTY(QVariantMap currentBoxAsset READ currentBoxAsset NOTIFY boxSelectionChanged)
    // Playlists (the reference's "projects"): [{id, name, entryCount}],
    // creation order; every session keeps at least one.
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY documentsChanged)
    // The current playlist's rows in running order: [{entryId, index,
    // showId, showName, section, slideCount}]. selectPlaylist also emits
    // documentsChanged so this view rebuilds.
    Q_PROPERTY(QVariantList playlistEntries READ playlistEntries NOTIFY documentsChanged)
    // The sidebar outline (UI.2): the Library row followed by one row per
    // playlist — [{kind, id, name, count}]. A projection through
    // DocumentViews like every other row shape.
    Q_PROPERTY(QVariantList collectionOutline READ collectionOutline NOTIFY documentsChanged)
    // The cue groups of the playlist under preparation (UI.3a):
    // [{entryId, showId, showName, section, entryIndex, slides}] — one group
    // per entry, so a show listed twice is two cues with two entry ids while
    // both name the same slides. Read-only: compiling or thumbnailing these
    // cues needs no cursor change and makes none.
    Q_PROPERTY(QVariantList runningOrderCues READ runningOrderCues NOTIFY documentsChanged)
    Q_PROPERTY(QString currentPlaylistId READ currentPlaylistId NOTIFY navigationChanged)
    // The media bin (UI.4a): [{id, name, sourcePath, kind}], import order,
    // where kind is the domain's own name rule ("picture", "video", "audio",
    // "other") so the list can say what a row is. save() is where the bytes
    // join the package.
    Q_PROPERTY(QVariantList assets READ assets NOTIFY documentsChanged)
    // The theme panel (task 4.4): every theme in the service, and the one
    // the current slide actually resolves through (override or show
    // default; empty map when the slide has no theme anywhere). The map
    // also depends on navigation, so views of it sync on both signals —
    // the same contract as the box fields.
    Q_PROPERTY(QVariantList themes READ themes NOTIFY documentsChanged)
    Q_PROPERTY(QVariantMap currentTheme READ currentTheme NOTIFY documentsChanged)
    Q_PROPERTY(bool currentThemeIsOverride READ currentThemeIsOverride NOTIFY documentsChanged)
    // How the current slide's background picture fits the canvas: "cover"
    // or "contain", "" with no slide (task 6.17c1). A document fact read
    // through the theme panel's combo; the combo also listens to navigation
    // in QML, the same two-signal contract the theme fields use.
    Q_PROPERTY(QString currentBackgroundFit READ currentBackgroundFit
               NOTIFY documentsChanged)
    // Edit-since-explicit-save, and the directory save()/load() use.
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(QString serviceDirectory READ serviceDirectory CONSTANT)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    // Task 4.5: the slide clipboard's paste-readiness (copy/cut fill it;
    // the system clipboard stays with the text fields, as in the
    // reference).
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY stateChanged)
    // One-line operator note: the last save/recovery/error worth showing.
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    // A deck render is in flight (milestone PX): true between the start and
    // the finished/failed edge. Drop targets disable while true.
    Q_PROPERTY(bool powerPointImportBusy READ powerPointImportBusy
               NOTIFY powerPointImportChanged)

public:
    // baseDirectory defaults to the per-user application data location;
    // tests pass a scratch directory. Startup follows the task 2.3 rule:
    // autosave data, if any, wins over the saved service (it is newer),
    // and a missing service simply begins empty.
    explicit DocumentController(QObject* parent = nullptr,
                                std::filesystem::path baseDirectory = {});

    QVariantList songs() const;
    QVariantList slides() const;
    QVariantList playlists() const;
    QVariantList collectionOutline() const;
    QVariantList runningOrderCues() const;
    // The slide rows of any show in the library (UI.3b): the Show view
    // renders a browsed presentation's group while the editing cursor stays
    // on its own work. Read-only, through DocumentViews like every row.
    Q_INVOKABLE QVariantList slidesOf(const QString& showId) const;
    // Any service show, including prepared songs and schedule-only imports.
    Q_INVOKABLE QVariantMap showInfo(const QString& showId) const;
    QVariantList playlistEntries() const;
    QVariantList assets() const;
    QVariantList themes() const;
    QVariantMap currentTheme() const;
    bool currentThemeIsOverride() const;
    QString currentBackgroundFit() const;
    QString currentPlaylistId() const {
        return QString::fromStdString(fCurrentPlaylistId);
    }
    QString currentSongId() const { return QString::fromStdString(fCurrentSongId); }
    int currentSlide() const { return fCurrentSlide; }
    QString currentSlideText() const;
    int currentBox() const;
    QVariantList currentSlideBoxes() const;
    QString currentBoxKind() const;
    QVariantMap currentBoxAsset() const;
    bool dirty() const { return fDirty; }
    QString serviceDirectory() const {
        return QString::fromStdWString(fPersistence->serviceDirectory().wstring());
    }
    bool canUndo() const { return fHistory->canUndo(); }
    bool canRedo() const { return fHistory->canRedo(); }
    bool canPaste() const { return fClip.valid; }
    QString status() const { return fStatus; }

    // --- editing (every call goes through History) ---

    // Names must be non-blank; the new song becomes current. Returns false
    // (with status) when the domain refuses.
    Q_INVOKABLE bool createSong(const QString& name);
    Q_INVOKABLE bool removeCurrentSong();
    Q_INVOKABLE bool removeLibraryShows(const QStringList& showIds, bool permanent = false);
    void forgetDeletedCatalogItems(const QStringList& ids);
    // Appends a slide to the current song and makes it current.
    Q_INVOKABLE bool addSlide();
    Q_INVOKABLE bool removeCurrentSlide();
    // Queues new text for the current slide; one history entry lands when
    // typing pauses or the next operation runs. When the selection is a
    // picture the commit refuses out loud instead of swallowing the words
    // into a model that cannot hold them (task 6.17b2).
    Q_INVOKABLE void setSlideText(const QString& text);
    // Resizes/moves the current slide's authored text box (task 4.3), in
    // 1920x1080 canvas units. One history entry per call, so a drag commits
    // once on release. Fails (with status) on a bad box or no current slide.
    Q_INVOKABLE bool setSlideGeometry(double x, double y, double width, double height);

    // --- text-box authoring (task 6.17a2, review R17) ---
    //
    // The selection is an ID with a positional fallback: it follows its box
    // across reorder and undo, and when the box it names is gone it falls to
    // box 0 — the SAME box the canvas highlights — so what the inspector
    // edits and what wears the handles can never be two different boxes.
    // Navigation to another slide clears it (the selection belongs to a
    // slide, not to the session). selectTextBox clamps like selectSlide and,
    // being chrome, dirties nothing; add/remove/reorder/geometry each land
    // one history entry, and a refusal lands a status line without one.
    Q_INVOKABLE bool selectTextBox(int index);
    // The TOPMOST box (last in draw order) whose authored rectangle contains
    // the authored-unit point, or -1. Iterating backwards is the whole
    // hit rule: the box drawn last is the one the operator sees on top, and
    // clicking what one can see is what one meant.
    Q_INVOKABLE int boxAt(double x, double y) const;
    // Appends a box with the default geometry on top of the current slide's
    // stack and SELECTS it — an operator adds a box in order to edit it.
    Q_INVOKABLE bool addTextBox();
    Q_INVOKABLE bool removeCurrentTextBox();
    // Reorders the selected box within the current slide; toIndex is its
    // position in the FINAL list (the domain's rule), and negative indices
    // are refused here because QML's neighbours are expressions, not clamps.
    Q_INVOKABLE bool moveCurrentTextBox(int toIndex);
    // Geometry of the SELECTED box, in 1920x1080 canvas units: one history
    // entry per call, so a drag commits once on release.
    Q_INVOKABLE bool setTextBoxGeometry(double x, double y, double width, double height);
    // The media drawer's placement command (task 6.17b2): put the asset on
    // top of the current slide's stack as a new image item and SELECT it —
    // the picture analogue of addTextBox, one history entry, and the model's
    // kind discipline owns the refusals (unknown asset, video, the shared
    // eight-item ceiling), which the status line names verbatim.
    Q_INVOKABLE bool placeAssetOnSlide(const QString& assetId);

    // --- theme panel (task 4.4) ---

    // Every mutation here is one history entry, and every refusal lands a
    // status line without touching the model (the 2.1 contract).
    // Points the current slide at a theme — a per-slide override; the
    // panel's "Follow show" hands it back with the empty id.
    Q_INVOKABLE bool applyThemeToCurrentSlide(const QString& themeId);
    // Makes a theme the whole show's default: every slide that follows the
    // show starts drawing it. This is the reuse half of "themes apply
    // consistently across slides".
    Q_INVOKABLE bool setShowDefaultTheme(const QString& themeId);
    // Edits one theme in place — name, colours, font and typography — and
    // every slide using it changes at once. backgroundGradient is the
    // second colour of a gradient background or "" for flat (task 6.17c3):
    // stating it is mandatory because leaving it out would mean silently
    // flattening a gradient the operator authored.
    Q_INVOKABLE bool editTheme(const QString& themeId, const QString& name,
                               const QString& textColor, const QString& backgroundColor,
                               const QString& backgroundGradient, const QString& fontFamily,
                               double fontSize, const QString& align, double lineSpacing);
    // Where a theme puts a wearing slide's PRIMARY lyric box (amendment
    // 2026-11-01, themes plan): four numbers in the 1920x1080 authored
    // canvas, vetted like an authored box. Setting one moves the slides that
    // FOLLOW this theme and have not customized their own placement — which
    // is the whole point of "the same place on every slide" — and the Theme
    // Editor's preview shows the rectangle before anything is worn.
    Q_INVOKABLE bool setThemePlacement(const QString& themeId, double x, double y, double width,
                                       double height);
    // The theme stops placing: its colours and typography stay, wearing
    // slides go back to the boxes they were authored with (unless they
    // customized placement). A theme with no placement is refused — the same
    // wrong-click rule every other revert follows.
    Q_INVOKABLE bool clearThemePlacement(const QString& themeId);
    // The theme's OWN text style (task TS.1, theme-editor review): WHOLE-SET
    // replacement in applyThemeContext's manner — a key present becomes the
    // theme's answer, a key absent makes the theme silent (it stops carrying
    // an override that used to be there), and {} silences all six. The map
    // names: fontWeight (integer 100..900), fontStyle ("normal"/"italic"),
    // fitMode ("shrink"/"fixed"), minFontSize (1..4096), stroke and shadow
    // (whole records — stroke {enabled,color,width}, shadow {enabled,color,
    // opacity,offsetX,offsetY,blurSigma}; a fragment is refused, because an
    // effect means one thing whole). Unknown keys, stringy weights and
    // half-records are refused naming themselves, never defaulted; the
    // bands are the domain's, so what this accepts the save can store and
    // the resolver can honour.
    Q_INVOKABLE bool setThemeTextStyle(const QString& themeId, const QVariantMap& style);

    // --- transitions (task TR.1, transition contract) -----------------------
    // The service's standing answer to "how does the audience get to the next
    // scene", and the two optional per-view overrides on top of it. All three
    // verbs are undoable and all three vet through the domain's ONE sentence
    // (Documents::validTransitionOptions), so a value this controller accepts
    // is a value the save stores and the renderer resolves without a second
    // opinion.
    //
    // The service default is CONCRETE: the 500 ms words / 800 ms media
    // crossfade every present performed before this setting existed is its
    // value, so there is no "unset service default" for a resolver to invent.
    // `kind` is "fade" or "cut"; both durations are whole milliseconds
    // 0..10000, and 0 on a track means that track lands immediately.
    Q_INVOKABLE bool setServiceDefaultTransition(const QString& kind, int textDurationMs,
                                                  int mediaDurationMs);
    // A view's own answer — the Songs view writes the Theme record, the Bible
    // view its TextContext's. An INHERIT is a removal, so it is a different
    // call from setting a record: `record` carries kind/textDurationMs/
    // mediaDurationMs, and an empty map takes the override off (the view says
    // nothing about movement and whatever is under it answers). The reads wear
    // the same shape (task TR.3): `themes()` and `currentTheme` carry
    // `transitionSet` beside a `transition` record, and `themeContext` carries
    // them for the named context, so what an inspector shows is a record its
    // own write accepts. A theme that does not exist, or a context no theme of
    // that name carries, is refused naming what was not found: an override
    // silently dropped on the floor is how an operator ends up believing a
    // view has a transition it never got.
    Q_INVOKABLE bool setThemeTransition(const QString& themeId, const QVariantMap& record);
    Q_INVOKABLE bool setTextContextTransition(const QString& themeId, const QString& contextName,
                                              const QVariantMap& record);
    // The resolved answer for one slide, in the shape a settings page shows
    // next to the control ("inherited from the service: 500 / 800"). This is
    // ALSO what the cue lane asks at Go — see WorkerController::
    // setTransitionResolver — so the number an operator reads and the number
    // a cue moves with come from the same walk.
    Q_INVOKABLE QVariantMap transitionForSlide(const QString& showId, const QString& slideId) const;
    // The same answer read from a compiled scene payload (its showId/slideId
    // are its provenance), which is what a Go has in hand. A payload that
    // names no slide resolves to the service default, which is the honest
    // answer for a scene the documents cannot place.
    openshow::domain::TransitionOptions transitionForSceneJson(
        const boost::json::object& scene) const;
    // The service default on its own, for a settings page that has no slide
    // under the cursor.
    Q_INVOKABLE QVariantMap serviceDefaultTransition() const;
    // The one-FIELD merge beside the whole-set verb (task TS.4): a control
    // that edits one property of a shared block must not have to know the
    // other five, and a drag gesture that lands many values must cost ONE
    // undo, not one per intermediate. `field` names a leaf — fontWeight,
    // fontStyle, fitMode, minFontSize, strokeEnabled, strokeColor,
    // strokeWidth, shadowEnabled, shadowColor, shadowOpacity,
    // shadowOffsetX, shadowOffsetY, shadowBlurSigma — or a whole effect
    // record ("stroke"/"shadow") for the inherit button to clear. A null
    // value clears a scalar to inherit, or names the whole-record clear;
    // null on an effect MEMBER is refused (an effect means one thing
    // whole — clearing one of its members is not a sentence). Merging a
    // member into a theme that carries no such record starts from the
    // contract's defaults — exactly what the audience draws today — so
    // enabling a stroke materialises {true, black, 2}, never a fragment
    // or a leftover of some other theme. One edit() per call: the merged
    // block is vetted by the domain's one sentence before it lands, and a
    // refusal changes nothing and costs no undo.
    Q_INVOKABLE bool setThemeTextStyleField(const QString& themeId, const QString& field,
                                            const QVariant& value);
    // The theme's named TEXT CONTEXTS (task TF.4, text-flow plan): a
    // context carries typography and a box ONLY — never colours, and never
    // a line count, because how many lines fit is what the worker's
    // measurement says. `themeContext` answers {set, name, fontFamily,
    // fontSize, lineSpacing, align, x, y, width, height, inset, transition}
    // with a ...Set flag per field (silence is a real state — an unset field
    // falls through to the themes); absent set means no such context. The
    // transition rides as {kind, textDurationMs, mediaDurationMs} plus
    // `transitionSet` (task TR.3), and it is the ONE field a replacing write
    // carries over by itself: `applyThemeContext`'s absent keys are silence
    // about the field they name, and a caller editing the reading font has
    // named nothing about how the view arrives.
    // `applyThemeContext` replaces (or adds) the named entry from a map
    // whose ABSENT keys are silence, exactly like absent keys in the file;
    // the vetting is the loader's. `removeThemeContext` takes one entry
    // off and refuses when the theme carries none — an undo costs what it
    // changes. The Bible passage is set in the context named "scripture".
    Q_INVOKABLE QVariantMap themeContext(const QString& themeId, const QString& name) const;
    Q_INVOKABLE bool applyThemeContext(const QString& themeId, const QString& name,
                                       const QVariantMap& fields);
    Q_INVOKABLE bool removeThemeContext(const QString& themeId, const QString& name);
    // Creates a theme wearing whatever the current slide draws now
    // (resolved theme or built-in defaults) and overrides only that slide.
    Q_INVOKABLE bool createThemeFromCurrent(const QString& name);

    // --- theme service (task TH.4, themes plan) ---

    // The one theme the whole service wears right now; "" is none. The
    // More menu checks against it and the Theme Editor's header toggles it.
    Q_PROPERTY(QString wornThemeId READ wornThemeId NOTIFY documentsChanged)
    QString wornThemeId() const;
    // Wear one theme service-wide — over every slide's own assignment — or
    // take it all off with "". One history entry; an unknown theme is
    // refused naming itself.
    Q_INVOKABLE bool useThemeAsCurrent(const QString& themeId);
    // New, from the editor: a theme wearing the built-in look (white on
    // black, renderer defaults) that the operator then restyles.
    Q_INVOKABLE bool createBlankTheme(const QString& name);
    // Duplicate: a copy under "<name> copy" — a starting point to edit,
    // not a linked second reference to the source.
    Q_INVOKABLE bool duplicateTheme(const QString& themeId);
    // Delete. The model clears every reference to the theme (show defaults,
    // slide assignments, the worn id), and this is one history entry.
    Q_INVOKABLE bool deleteTheme(const QString& themeId);
    // The built-in sample cue compiled as that theme wears it — the Theme
    // Editor's centre preview payload. The sample is a fixture, never a
    // document: nothing is recorded and nothing is presented, so browsing
    // every theme in the service cannot touch the audience's picture.
    // Unknown id: empty string.
    // Task TV.1 gives the sample a VIEW: "" compiles the SONGS look (the
    // theme's own base fields), and a name — "scripture" is this build's
    // Bible view — compiles the sample slide naming that text context, so
    // the context's typography and box answer exactly as a passage's slide
    // will resolve them. The fixture is a COPY of the live documents with
    // the current theme cleared and the SELECTED theme set as the throwaway
    // show's default: the edited Default theme and every theme's contexts
    // sit in the chain as the audience would walk them, and whatever the
    // service happens to wear cannot bleed into another theme's sample.
    Q_INVOKABLE QString themeSampleSceneJson(const QString& themeId,
                                             const QString& contextName = QString()) const;
    // What the sample's PRIMARY text box actually resolves to — the same
    // fixture and the same resolver, answered as a row instead of pixels:
    // {ok, boxX, boxY, boxWidth, boxHeight, fontSize (0 = the renderer's
    // own default), lineSpacing, align, fontFamily, context} with `context`
    // the name of the context that layered ("" when none did). The Theme
    // Editor's overlay and box fields read THIS rectangle, so the handle
    // and the picture underneath it can never disagree about which view
    // is being edited. Unknown id or an uncompilable fixture: {ok: false}.
    Q_INVOKABLE QVariantMap themeSampleAppearance(const QString& themeId,
                                                  const QString& contextName = QString()) const;

    // The Bible importer's off-air look (task BI.5). A scripture slide
    // belongs to a young show that wears nothing of its own, so what it
    // draws is the WHOLE chain as the service stands: the worn theme, the
    // Default layer, the built-in box — and the `scripture` text context
    // the passage names, when one of those themes carries it (task TF.3).
    // These two answer through a fixture
    // COPY of the live documents with a throwaway show added — the same
    // resolution the committed slide will walk, with no history entry, no
    // dirty flag and nothing presented. `scriptureBoxStyle` returns the
    // primary text box in canvas pixels and typography — {boxWidth,
    // boxHeight, fontSize (0 = the renderer's own default), lineSpacing,
    // fontFamily} — and an empty map when the chain compiles to no text
    // box at all (the importer then blocks Add with an explanation).
    // `scriptureSlideSceneJson` compiles one GENERATED slide (lines the
    // document does not hold yet) to the scene JSON the preview transport
    // takes, so the operator sees the slide before the schedule does.
    Q_INVOKABLE QVariantMap scriptureBoxStyle() const;
    Q_INVOKABLE QString scriptureSlideSceneJson(const QStringList& lines) const;

    // The song import's off-air look (tasks SI.2/SI.3): the same fixture
    // compile with NO named text context, i.e. the Songs view the prepared
    // slide will wear. The importer measures and previews with these exact
    // numbers; `songSlideSceneJson` compiles one generated slide whose words
    // the document does not hold yet.
    Q_INVOKABLE QVariantMap songBoxStyle() const;
    Q_INVOKABLE QString songSlideSceneJson(const QStringList& lines) const;

    // Task SL.6/SL.8: prepare a shared-catalog song into THIS service. One
    // history entry. A show already linked to `catalogItemId` is reused (the
    // running order references it), renamed and re-laid from `slides`;
    // otherwise a new linked show is created. `slides` rows are
    // {lines: QStringList, group: QString, label: QString} — `group` is the
    // stanza label (the slide's group), `label` its notes text. With
    // `addToRunningOrder` the show joins the current running order at
    // `index` (-1 appends), and repeated adds are separate entries over the
    // same referenced show. Returns the service show id, or "" with status.
    Q_INVOKABLE QString upsertCatalogSong(const QString& catalogItemId,
                                          const QString& title, const QString& ccli,
                                          const QString& authors, const QString& copyright,
                                          const QVariantList& slides,
                                          bool addToRunningOrder, int index = -1);
    // The service show prepared from a catalog item, "" when none is linked.
    Q_INVOKABLE QString showForCatalogItem(const QString& catalogItemId) const;
    // Propagates a catalog rename to the linked show without re-laying the
    // words (the reference's whole point). One history entry when changed.
    Q_INVOKABLE bool renameLinkedCatalogShow(const QString& catalogItemId,
                                             const QString& title);
    // The catalog record was deleted: clear the link but keep the copied
    // attribution, so the service stays playable and reportable.
    Q_INVOKABLE bool detachLinkedCatalogShow(const QString& catalogItemId);

    // --- theme surfaces (task TH.5, themes plan) ---

    // The current slide through the whole chain, per property: the value
    // it draws and whether the SLIDE customized that property (the badge
    // the inspector carries). Field edits here write customizations, so a
    // shared theme is never silently restyled again. Empty map with no
    // current slide.
    Q_PROPERTY(QVariantMap slideAppearance READ slideAppearance NOTIFY documentsChanged)
    QVariantMap slideAppearance() const;
    // One appearance field of the current slide becomes a customization
    // (the gradient's "" is the explicit flat, the family's "" the platform
    // answer); the theme entity is left alone. The domain vets the value
    // by the theme's own contract.
    Q_INVOKABLE bool customizeCurrentSlide(const QString& field, const QVariant& value);
    // One customized property goes back to the themes: nothing customized
    // under that name is refused — a revert that reverted nothing is a
    // wrong click, and the status line says so.
    Q_INVOKABLE bool revertCurrentSlideField(const QString& field);
    // The whole slide back at the themes, one history entry.
    Q_INVOKABLE bool removeCurrentSlideCustomizations();
    // Cue-grid apply: the slide with this id wears that theme ("" follows
    // the show). Tiles browse any show in the library, so the owning show
    // is found from the slide, not from the editing cursor.
    Q_INVOKABLE bool applyThemeToSlide(const QString& slideId, const QString& themeId);
    // Bulk apply and bulk restore for a whole show: one mutation over
    // every slide, so the operator's one undo undoes the whole bulk.
    Q_INVOKABLE bool applyThemeToShow(const QString& showId, const QString& themeId);
    Q_INVOKABLE bool removeSlideCustomizationsFor(const QString& slideId);
    Q_INVOKABLE bool removeShowCustomizationsFor(const QString& showId);
    // The show page's header selector: the default of the show BEING
    // VIEWED, which while browsing is not the show under edit.
    Q_INVOKABLE bool setShowDefaultThemeFor(const QString& showId, const QString& themeId);
    // Layering: an image behind the current slide's words; "" clears it
    // back to the theme's background colour.
    Q_INVOKABLE bool setSlideBackground(const QString& assetId);
    // How the current slide's background sits on the canvas (6.17c1): the
    // two names the model spells, one history entry; a name the domain does
    // not know is refused here naming itself, before any edit is recorded.
    Q_INVOKABLE bool setSlideBackgroundFit(const QString& fit);

    // --- slide clipboard (task 4.5) ---

    // The reference's clipboard carries slides, not system text (text
    // fields keep their native copy & paste). Copy and cut capture the
    // current slide's lines, notes, box, theme and background; paste
    // inserts a slide carrying them right after the current one and
    // selects it — one undo entry, and references that have since been
    // deleted fall away instead of refusing the paste. Duplicate works
    // from the current slide directly and leaves the clipboard alone.
    Q_INVOKABLE bool copyCurrentSlide();
    Q_INVOKABLE bool cutCurrentSlide();
    Q_INVOKABLE bool pasteSlide();
    Q_INVOKABLE bool duplicateCurrentSlide();

    // --- navigation ---

    Q_INVOKABLE void selectSong(const QString& songId);
    Q_INVOKABLE void selectSlide(int index);
    // Moves within the current song, clamped; no-op without a song.
    Q_INVOKABLE void stepSlide(int delta);

    // --- playlist and library drawer (task 4.2) ---

    // A named playlist becomes current on creation. Entries address the
    // current playlist (the drawer's list); insert indices clamp, and
    // size() appends. movePlaylistEntry's toIndex is the row's position
    // after the move, clamped. The reference reorders by drag; the same
    // op serves its keyboard-less paths too.
    Q_INVOKABLE bool createPlaylist(const QString& name);
    Q_INVOKABLE void selectPlaylist(const QString& playlistId);
    Q_INVOKABLE bool addShowToPlaylist(const QString& showId);
    Q_INVOKABLE bool insertShowInPlaylist(const QString& showId, int index);
    // The Bible importer's Add (task BI.3), the plan's one destination
    // action: create the schedule-owned scripture show, fill it with the
    // generated slides and join it to the GIVEN playlist — all as one
    // edit, so one Undo removes the whole item. `playlistId` is the
    // explicit target (today's schedule); a target that vanished between
    // preview and Add is refused rather than guessed at. Each element of
    // `slides` is the slide's lines as a QStringList, or a map
    // {lines: [...], notes: "..."} when the slide carries a verse label —
    // the label belongs in the NOTES so the slide's words stay exactly the
    // passage (task BI.4's rule). No Library show is ever created, so
    // removal can never leave one behind.
    Q_INVOKABLE bool addScripturePassage(const QString& playlistId, const QString& reference,
                                         const QString& translation, const QString& attribution,
                                         const QVariantList& slides);
    // The drawer's Enter: insert after the current show when it is in
    // the playlist (the reference's placement rule), otherwise append.
    Q_INVOKABLE bool insertShowAfterCurrentShow(const QString& showId);
    Q_INVOKABLE bool removePlaylistEntry(const QString& entryId);
    // One preparation edit/undo, validated by entry identity before removal.
    Q_INVOKABLE bool removePlaylistEntries(const QStringList& entryIds);
    Q_INVOKABLE bool movePlaylistEntry(const QString& entryId, int toIndex);
    // Songs matching the query by name and lyrics, best match first
    // (LibrarySearch); the full creation-order list when blank.
    Q_INVOKABLE QVariantList searchShows(const QString& query) const;
    // Media tab: register a file (bytes are copied by save(), the
    // domain's rule) and remove one from the library.
    Q_INVOKABLE bool importMediaFile(const QString& filePath);
    Q_INVOKABLE bool importMediaUrl(const QUrl& file);
    Q_INVOKABLE bool removeAsset(const QString& assetId);
    // The bin's import filter (UI.4a), spelled from the domain's one
    // extension table so the dialog, the rows' kind labels and the pipeline's
    // own decision cannot hold three opinions about what a name means.
    // "All files" stays offered: registering a name nothing claims is
    // possible, and the bin shows it as unsupported rather than hiding it.
    Q_INVOKABLE QStringList mediaImportNameFilters() const;

    // --- PowerPoint import (PX.2) ---
    //
    // Renders a `.pptx` through the sidecar and lands one background-only
    // slide per visible page, all in ONE edit: assets, show and (for the
    // service) its running-order row undo together. The service destination
    // creates a schedule-owned show at `index` (clamped; -1 appends) and never
    // adds a Library row; the Library destination creates an ordinary show
    // and leaves the running order alone. The URL forms are what a QML drop
    // carries; non-local URLs are refused with a sentence.
    Q_INVOKABLE bool importPowerPointToService(const QString& file, int index = -1);
    Q_INVOKABLE bool importPowerPointToLibrary(const QString& file);
    Q_INVOKABLE bool importPowerPointUrlToService(const QUrl& file, int index = -1);
    Q_INVOKABLE bool importPowerPointUrlToLibrary(const QUrl& file);
    bool powerPointImportBusy() const;

    // --- presentation hand-off ---

    // The current slide compiled to a scene (the scene.prepare JSON), or
    // "" with the reason in status when compilation fails.
    Q_INVOKABLE QString currentSceneJson();
    // Scene JSON for one slide of the current song (per-slide Present
    // buttons); "" on failure.
    Q_INVOKABLE QString sceneJsonForSlide(const QString& slideId);

    // --- persistence (the durability itself is DocumentPersistence's) ---

    // Both run the decision protocol: the executor is quiesced, the write or
    // discard happens, the scratch that decision made stale is removed with
    // its survivor reported, and autosave reopens. What the controller adds is
    // what a decision does to the model it owns: which paths history's
    // snapshots name, where navigation lands, and that the session is clean.
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool load();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // Commits any queued text immediately. Called internally before
    // navigation, undo, save and load — and on aboutToQuit, connected in
    // the constructor (task 6.14a, review R14): the debounced keystrokes
    // of a closing session must reach the model, and through the tick,
    // the scratch, before the process is gone.
    void flushText();

    // Task 6.14b (review R14): scratch persistence on the bounded,
    // coalescing executor instead of on the UI thread. Production main()
    // calls this once; until then autosave runs inline (the pre-6.14b
    // behaviour unit tests rely on).
    Q_INVOKABLE void startBackgroundPersistence();

    // Blocks until background persistence is idle and delivers the
    // queued acknowledgements (fixture seam; harmless elsewhere).
    void awaitPersistence();

    // Quit edge: flush the debounce and let the durable scratch finish
    // writing before the process exits (connected on aboutToQuit).
    void prepareForQuit();

    // Persistence executor counters (tests and diagnostics): package
    // writes run, and requests merged into a later one or vetoed by a
    // decision.
    qulonglong autosaveWrites() const;
    qulonglong autosaveCoalesced() const;

signals:
    void documentsChanged();
    void navigationChanged();
    void slideTextChanged();
    // The selected text box changed — through selectTextBox, an add/remove/
    // move that took it away or moved it, or an undo/redo that restored a
    // different stack. Not a document change: QML chrome listens here, and
    // autosave must not fire for a click that edited nothing.
    void boxSelectionChanged();
    void stateChanged();
    // PX.2: a deck render started/stopped and what it produced. The finished
    // signal carries the status line the operator reads; QML uses the busy
    // property to close its drop targets.
    void powerPointImportChanged();
    void powerPointImportFinished(bool ok, const QString& message);
    // Task 6.12 (review R7): a save adopted the package-owned copies of
    // asset bytes — the map is old path -> new path. Anything still
    // holding a scene payload that names an old path must stop: the file
    // behind it is now safe to delete, and after an autosave-clear it
    // IS gone. WorkerController listens; tests can too.
    void assetsAdopted(const QVariantMap& pathMap);

private slots:
    void commitPendingText();

private:
    bool edit(const std::function<bool()>& mutation);  // history-wrapped edit
    // PX.2: one deck import. begin validates and starts the sidecar; apply
    // lands the manifest's pages as one edit and reports the operator line.
    bool beginPowerPointImport(const QString& file, bool toLibrary, int index);
    bool applyPowerPointImport(const PowerPointImport::Deck& deck, bool toLibrary, int index);
    void reportPowerPointFailure(const QString& message);
    // The navigation as a value, for the row projections (task 6.19b2). The
    // ids live here and nowhere else; DocumentViews reads them through this
    // cursor and never stores one.
    DocumentViews::Cursor viewCursor() const;
    // The current slide's SELECTED box under the fallback rule of the
    // currentBox property docs; nullptr with no slide or no boxes. Display
    // and every box-scoped op resolve through this one function — which
    // forwards to DocumentViews::selectedBox, the single resolution — which
    // is what keeps the highlighted box and the edited box identical.
    const openshow::domain::SlideItem* selectedBox() const;
    void setStatus(const QString& text);
    // The autosave edge of a committed change, handed to the persistence
    // adapter: inline while its executor is dormant (tests), coalesced and
    // off-thread once started (task 6.14b). `what` names the event for the
    // status line, which the adapter writes through this class.
    void autosaveNow(const char* what);
    QString compileSlideJson(const openshow::domain::Id& slideId) const;
    // The fixture-compile style and scene helpers (Bible and Songs view).
    QVariantMap fixtureBoxStyle(bool scripture) const;
    QString fixtureSlideSceneJson(const QStringList& lines, bool scripture) const;
    // The current slide's id, or "" when navigation points nowhere.
    openshow::domain::Id currentSlideId() const;
    // Every session has a playlist to run: seed a default "Service" when
    // neither the loaded service nor autosave brought one. Direct, not
    // history-wrapped — like the loaded state it is baseline, so undo
    // never lands on "no playlist at all".
    void seedDefaultPlaylist();
    // Re-points the current playlist when an undo/redo or removal took it
    // away.
    void anchorPlaylistNavigation();
    // The slide clipboard (task 4.5): one slide's whole presentation.
    struct SlideClip {
        bool valid = false;
        // The slide's boxes in DRAW order, with their ids cleared (task
        // 6.17a): a paste asks the store for fresh identities, because two
        // slides whose boxes shared an id would be two slides whose edits
        // name the same box on the wrong slide. Words, geometry and order are
        // what a clip carries; identity is not copyable.
        std::vector<openshow::domain::SlideItem> boxes;
        std::string notes;
        std::string themeId;
        std::string backgroundAssetId;
        // The clip fits its background the way the source slide did
        // (6.17c1) — the choice travels with the slide that authored it.
        openshow::domain::BackgroundFit backgroundFit =
            openshow::domain::BackgroundFit::Cover;
    };
    // Inserts a slide carrying the clip after the current position and
    // selects it. References the service no longer holds are dropped.
    bool pasteClip(const SlideClip& clip);

    openshow::domain::Documents fDocuments;
    // Row shapes (task 6.19b2): a stateless reader over the documents
    // beside them, so the property getters below forward to the one place
    // that spells a row. Declared after fDocuments because it holds a
    // reference to it.
    DocumentViews fViews;
    std::unique_ptr<openshow::domain::History> fHistory;
    // Durability (task 6.19b1): the scratch, its executor, the decision
    // protocol and the status line it writes. It is a child QObject, given the
    // base directory, and reads this class's documents only through the calls
    // above.
    DocumentPersistence* fPersistence = nullptr;
    // PX.2: the deck renderer and the destination the in-flight render
    // will land in. One deck at a time; the busy property is the drop
    // targets' gate.
    std::unique_ptr<PowerPointImport> fPptx;
    bool fPptxToLibrary = false;
    int fPptxIndex = -1;
    openshow::domain::Id fCurrentSongId;
    openshow::domain::Id fCurrentPlaylistId;
    int fCurrentSlide = 0;
    openshow::domain::Id fCurrentBoxId;
    SlideClip fClip;

    // Debounced text edit.
    QTimer fTextCommit;
    bool fHasPendingText = false;
    QString fPendingText;

    bool fDirty = false;
    QString fStatus;
};

}  // namespace openshow::ui
