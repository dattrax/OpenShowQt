/*
 * DocumentController: the document session behind the lyric panel (task 3.3).
 *
 * The frontend owns the editable documents (PLAN.md's ownership split), so
 * this is the object that holds them: a domain::Documents with its History
 * (task 2.3 snapshots) and the Autosave seam, exposed to QML with just
 * enough surface to enter and navigate lyrics — songs, their slides, the
 * current slide's text, and compiled scene JSON ready for
 * WorkerController::presentSceneJson. It is deliberately not the M4
 * workspace: plain QVariantList views, create/edit/remove only, and every
 * mutation runs through History so undo covers everything the panel does.
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
    // Edit-since-explicit-save, and the directory save()/load() use.
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(QString serviceDirectory READ serviceDirectory CONSTANT)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
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
    QString currentSongId() const { return QString::fromStdString(fCurrentSongId); }
    int currentSlide() const { return fCurrentSlide; }
    QString currentSlideText() const;
    bool dirty() const { return fDirty; }
    QString serviceDirectory() const {
        return QString::fromStdWString(fServiceDirectory.wstring());
    }
    bool canUndo() const { return fHistory->canUndo(); }
    bool canRedo() const { return fHistory->canRedo(); }
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

    // --- navigation ---

    Q_INVOKABLE void selectSong(const QString& songId);
    Q_INVOKABLE void selectSlide(int index);
    // Moves within the current song, clamped; no-op without a song.
    Q_INVOKABLE void stepSlide(int delta);

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

    // Commits any queued text immediately. Exposed for tests and called
    // internally before navigation, undo, save and load.
    void flushText();

signals:
    void documentsChanged();
    void navigationChanged();
    void slideTextChanged();
    void stateChanged();

private slots:
    void commitPendingText();

private:
    bool edit(const std::function<bool()>& mutation);  // history-wrapped edit
    void setStatus(const QString& text);
    QString compileSlideJson(const openshow::domain::Id& slideId) const;
    // The current slide's id, or "" when navigation points nowhere.
    openshow::domain::Id currentSlideId() const;

    openshow::domain::Documents fDocuments;
    std::unique_ptr<openshow::domain::History> fHistory;
    std::filesystem::path fServiceDirectory;
    std::unique_ptr<openshow::domain::Autosave> fAutosave;

    openshow::domain::Id fCurrentSongId;
    int fCurrentSlide = 0;

    // Debounced text edit.
    QTimer fTextCommit;
    bool fHasPendingText = false;
    QString fPendingText;

    bool fDirty = false;
    QString fStatus;
};

}  // namespace openshow::ui
