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
 * Qt Core only — no GUI — so the headless fixtures drive the very class
 * the panel binds to. All of it lives on the UI thread.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <functional>
#include <memory>

#include "openshow/domain/documents.h"
#include "openshow/domain/history.h"
#include "openshow/domain/storage.h"

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
    // The current slide's lyrics, lines joined with '\n'.
    Q_PROPERTY(QString currentSlideText READ currentSlideText NOTIFY slideTextChanged)
    // Playlists (the reference's "projects"): [{id, name, entryCount}],
    // creation order; every session keeps at least one.
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY documentsChanged)
    // The current playlist's rows in running order: [{entryId, index,
    // showId, showName, section, slideCount}]. selectPlaylist also emits
    // documentsChanged so this view rebuilds.
    Q_PROPERTY(QVariantList playlistEntries READ playlistEntries NOTIFY documentsChanged)
    Q_PROPERTY(QString currentPlaylistId READ currentPlaylistId NOTIFY navigationChanged)
    // The media library (drawer Media tab): [{id, name, sourcePath}],
    // import order. save() is where the bytes join the package.
    Q_PROPERTY(QVariantList assets READ assets NOTIFY documentsChanged)
    // The style panel (task 4.4): every style in the service, and the one
    // the current slide actually resolves through (override or show
    // default; empty map when the slide has no style anywhere). The map
    // also depends on navigation, so views of it sync on both signals —
    // the same contract as the box fields.
    Q_PROPERTY(QVariantList styles READ styles NOTIFY documentsChanged)
    Q_PROPERTY(QVariantMap currentStyle READ currentStyle NOTIFY documentsChanged)
    Q_PROPERTY(bool currentStyleIsOverride READ currentStyleIsOverride NOTIFY documentsChanged)
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
    QVariantList playlistEntries() const;
    QVariantList assets() const;
    QVariantList styles() const;
    QVariantMap currentStyle() const;
    bool currentStyleIsOverride() const;
    QString currentPlaylistId() const {
        return QString::fromStdString(fCurrentPlaylistId);
    }
    QString currentSongId() const { return QString::fromStdString(fCurrentSongId); }
    int currentSlide() const { return fCurrentSlide; }
    QString currentSlideText() const;
    bool dirty() const { return fDirty; }
    QString serviceDirectory() const {
        return QString::fromStdWString(fServiceDirectory.wstring());
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
    // Appends a slide to the current song and makes it current.
    Q_INVOKABLE bool addSlide();
    Q_INVOKABLE bool removeCurrentSlide();
    // Queues new text for the current slide; one history entry lands when
    // typing pauses or the next operation runs.
    Q_INVOKABLE void setSlideText(const QString& text);
    // Resizes/moves the current slide's authored text box (task 4.3), in
    // 1920x1080 canvas units. One history entry per call, so a drag commits
    // once on release. Fails (with status) on a bad box or no current slide.
    Q_INVOKABLE bool setSlideGeometry(double x, double y, double width, double height);

    // --- style panel (task 4.4) ---

    // Every mutation here is one history entry, and every refusal lands a
    // status line without touching the model (the 2.1 contract).
    // Points the current slide at a style — a per-slide override; the
    // panel's "Follow show" hands it back with the empty id.
    Q_INVOKABLE bool applyStyleToCurrentSlide(const QString& styleId);
    // Makes a style the whole show's default: every slide that follows the
    // show starts drawing it. This is the reuse half of "styles apply
    // consistently across slides".
    Q_INVOKABLE bool setShowDefaultStyle(const QString& styleId);
    // Edits one style in place — name, colours, font and typography — and
    // every slide using it changes at once.
    Q_INVOKABLE bool editStyle(const QString& styleId, const QString& name,
                               const QString& textColor, const QString& backgroundColor,
                               const QString& fontFamily, double fontSize,
                               const QString& align, double lineSpacing);
    // Creates a style wearing whatever the current slide draws now
    // (resolved style or built-in defaults) and overrides only that slide.
    Q_INVOKABLE bool createStyleFromCurrent(const QString& name);
    // Layering: an image behind the current slide's words; "" clears it
    // back to the style's background colour.
    Q_INVOKABLE bool setSlideBackground(const QString& assetId);

    // --- slide clipboard (task 4.5) ---

    // The reference's clipboard carries slides, not system text (text
    // fields keep their native copy & paste). Copy and cut capture the
    // current slide's lines, notes, box, style and background; paste
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
    // The drawer's Enter: insert after the current show when it is in
    // the playlist (the reference's placement rule), otherwise append.
    Q_INVOKABLE bool insertShowAfterCurrentShow(const QString& showId);
    Q_INVOKABLE bool removePlaylistEntry(const QString& entryId);
    Q_INVOKABLE bool movePlaylistEntry(const QString& entryId, int toIndex);
    // Songs matching the query by name and lyrics, best match first
    // (LibrarySearch); the full creation-order list when blank.
    Q_INVOKABLE QVariantList searchShows(const QString& query) const;
    // Media tab: register a file (bytes are copied by save(), the
    // domain's rule) and remove one from the library.
    Q_INVOKABLE bool importMediaFile(const QString& filePath);
    Q_INVOKABLE bool removeAsset(const QString& assetId);

    // --- presentation hand-off ---

    // The current slide compiled to a scene (the scene.prepare JSON), or
    // "" with the reason in status when compilation fails.
    Q_INVOKABLE QString currentSceneJson();
    // Scene JSON for one slide of the current song (per-slide Present
    // buttons); "" on failure.
    Q_INVOKABLE QString sceneJsonForSlide(const QString& slideId);

    // --- persistence ---

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

signals:
    void documentsChanged();
    void navigationChanged();
    void slideTextChanged();
    void stateChanged();
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
    void setStatus(const QString& text);
    // Task 6.14a (review R14): report the outcome of an autosave tick. A
    // refusal means the crash net has a hole NOW and must not be discarded
    // — but it lands DEFERRED, because every edit caller sets its own
    // status line synchronously after edit() returns and last write wins.
    // The flag suppresses a queued refusal the moment the scratch is
    // current again (and says so).
    void surfaceAutosaveResult(const std::string& tickError, const char* what);
    QString compileSlideJson(const openshow::domain::Id& slideId) const;
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
        std::vector<std::string> lines;
        std::string notes;
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        std::string styleId;
        std::string backgroundAssetId;
    };
    // Inserts a slide carrying the clip after the current position and
    // selects it. References the service no longer holds are dropped.
    bool pasteClip(const SlideClip& clip);

    openshow::domain::Documents fDocuments;
    std::unique_ptr<openshow::domain::History> fHistory;
    std::filesystem::path fServiceDirectory;
    std::unique_ptr<openshow::domain::Autosave> fAutosave;

    openshow::domain::Id fCurrentSongId;
    openshow::domain::Id fCurrentPlaylistId;
    int fCurrentSlide = 0;
    SlideClip fClip;

    // Debounced text edit.
    QTimer fTextCommit;
    bool fHasPendingText = false;
    QString fPendingText;

    bool fDirty = false;
    QString fStatus;
    bool fAutosaveStale = false;  // scratch behind the model: task 6.14a
};

}  // namespace openshow::ui
