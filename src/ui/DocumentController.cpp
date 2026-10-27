#include "DocumentController.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>

#include "openshow/scene/scene.h"

#include "LibrarySearch.h"

namespace openshow::ui {

namespace {

// One split of '\n' per authored line; empty text stores no lines at all,
// so a slide cleared back to nothing compiles to an interlude, not a
// slide holding one blank line.
std::vector<std::string> textToLines(const QString& text) {
    if (text.isEmpty()) {
        return {};
    }
    std::vector<std::string> lines;
    const QStringList parts = text.split(QLatin1Char('\n'));
    lines.reserve(static_cast<std::size_t>(parts.size()));
    for (const QString& part : parts) {
        lines.push_back(part.toStdString());
    }
    return lines;
}

QString linesToText(const std::vector<std::string>& lines) {
    QStringList parts;
    parts.reserve(static_cast<int>(lines.size()));
    for (const std::string& line : lines) {
        parts.append(QString::fromStdString(line));
    }
    return parts.join(QLatin1Char('\n'));
}

}  // namespace

DocumentController::DocumentController(QObject* parent, std::filesystem::path baseDirectory)
    : QObject(parent) {
    if (baseDirectory.empty()) {
        baseDirectory = std::filesystem::path(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString());
    }
    fServiceDirectory = baseDirectory / "service";
    fScheduler = std::make_unique<AutosaveScheduler>(baseDirectory / "autosave");
    fHistory = std::make_unique<openshow::domain::History>(&fDocuments);

    // Task 6.14b (review R14): durability is acknowledged on THIS thread,
    // wherever the write ran.
    connect(fScheduler.get(), &AutosaveScheduler::tickCompleted, this,
            &DocumentController::onAutosaveCompleted);

    // Task 2.3's startup rule: autosave data, when present, is newer than
    // the saved service and wins; a missing service simply begins empty.
    std::string error;
    if (openshow::domain::Autosave::exists(fScheduler->directory())) {
        if (fScheduler->recover(&fDocuments, &error)) {
            fDirty = true;  // there is unsaved work — that is why we recovered
            fStatus = QStringLiteral("recovered unsaved changes from autosave");
        } else {
            fStatus = QStringLiteral("autosave recovery failed: %1; starting empty")
                          .arg(QString::fromStdString(error));
        }
    } else if (!fServiceDirectory.empty()) {
        if (openshow::domain::Autosave::exists(fServiceDirectory)) {
            // Not the autosave package, so a real save is waiting.
            if (openshow::domain::loadService(&fDocuments, fServiceDirectory, &error)) {
                fStatus = QStringLiteral("loaded %1")
                              .arg(QString::fromStdWString(fServiceDirectory.wstring()));
            } else {
                fStatus = QStringLiteral("load failed: %1").arg(QString::fromStdString(error));
            }
        }
    }

    // Adopt whatever the load produced for navigation. No baseline tick:
    // a tick with a virgin Autosave always writes, and an autosave package
    // must mean unsaved work (the 2.3 rule) — the first committed edit is
    // what creates it.
    const std::vector<openshow::domain::Id> ids = fDocuments.showIds();
    if (!ids.empty()) {
        fCurrentSongId = ids.front();
        fCurrentSlide = 0;
    }
    seedDefaultPlaylist();

    fTextCommit.setSingleShot(true);
    fTextCommit.setInterval(600);
    connect(&fTextCommit, &QTimer::timeout, this, &DocumentController::commitPendingText);

    // Task 6.14a (review R14): the debounce above holds the session's last
    // keystrokes. Normal window close ends the loop, and the SIGTERM/SIGINT
    // poll calls app.quit() — both pass through aboutToQuit — so the
    // controller keeps the flush promise itself instead of relying on
    // whoever wired main(): commit the pending text, and let the scratch
    // finish WRITING before the process is gone (task 6.14b).
    connect(qApp, &QCoreApplication::aboutToQuit, this, &DocumentController::prepareForQuit);
}

QVariantList DocumentController::songs() const {
    QVariantList list;
    for (const openshow::domain::Id& id : fDocuments.showIds()) {
        const openshow::domain::Show* show = fDocuments.findShow(id);
        if (show == nullptr) {
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(id));
        entry.insert(QStringLiteral("name"), QString::fromStdString(show->name));
        entry.insert(QStringLiteral("slideCount"), static_cast<int>(show->slides.size()));
        list.append(entry);
    }
    return list;
}

QVariantList DocumentController::slides() const {
    QVariantList list;
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    if (show == nullptr) {
        return list;
    }
    for (std::size_t i = 0; i < show->slides.size(); ++i) {
        const openshow::domain::Slide& slide = show->slides[i];
        // The row's position is the slide's FIRST box in draw order — the one
        // the canvas and the arrow-key nudge edit until the operator can pick
        // a box (6.17a2). A slide authored down to no boxes reports the
        // default box rather than zeros, so a canvas that draws the row's
        // outline is never handed a rectangle with no size.
        const openshow::domain::TextBox fallback;
        const openshow::domain::TextBox& box =
            slide.items.empty() ? fallback : slide.items.front();
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(slide.id));
        entry.insert(QStringLiteral("index"), static_cast<int>(i));
        entry.insert(QStringLiteral("preview"),
                     QString::fromStdString(box.lines.empty() ? std::string("(empty)")
                                                              : box.lines.front()));
        entry.insert(QStringLiteral("x"), box.x);
        entry.insert(QStringLiteral("y"), box.y);
        entry.insert(QStringLiteral("width"), box.width);
        entry.insert(QStringLiteral("height"), box.height);
        entry.insert(QStringLiteral("backgroundAssetId"),
                     QString::fromStdString(slide.backgroundAssetId));
        list.append(entry);
    }
    return list;
}

QString DocumentController::currentSlideText() const {
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    if (show == nullptr || fCurrentSlide < 0 ||
        static_cast<std::size_t>(fCurrentSlide) >= show->slides.size()) {
        return QString();
    }
    // The text inspector edits the first box (see slides()).
    const openshow::domain::Slide& slide = show->slides[static_cast<std::size_t>(fCurrentSlide)];
    const openshow::domain::TextBox* box = slide.firstTextBox();
    return box == nullptr ? QString() : linesToText(box->lines);
}

openshow::domain::Id DocumentController::currentSlideId() const {
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    if (show == nullptr || fCurrentSlide < 0 ||
        static_cast<std::size_t>(fCurrentSlide) >= show->slides.size()) {
        return {};
    }
    return show->slides[static_cast<std::size_t>(fCurrentSlide)].id;
}

bool DocumentController::edit(const std::function<bool()>& mutation) {
    // A refused edit is invisible to History (the 2.1 contract: model and
    // stacks untouched), so there is nothing to record here; the caller
    // already has the domain's error string for its own status line.
    if (!fHistory->apply(mutation)) {
        return false;
    }
    fDirty = true;
    autosaveNow("edit");
    emit documentsChanged();
    emit stateChanged();
    return true;
}

void DocumentController::setStatus(const QString& text) {
    fStatus = text;
    emit stateChanged();
}

void DocumentController::surfaceAutosaveResult(const std::string& tickError, const char* what,
                                               bool deferStatus) {
    if (!tickError.empty()) {
        fAutosaveStale = true;
        const QString message =
            QStringLiteral("autosave refused after %1: %2 - unsaved work is NOT on disk")
                .arg(QString::fromLatin1(what), QString::fromStdString(tickError));
        if (!deferStatus) {
            // The background ack has no caller that will overwrite the
            // line afterwards — it is already the last word.
            setStatus(message);
            return;
        }
        // Deferred, so the refusal is the LAST word: edit callers set
        // their own status synchronously after edit() returns. The flag
        // check is the race guard — if the scratch caught up before this
        // timer ran, the stale warning must not land.
        QTimer::singleShot(0, this, [this, message] {
            if (fAutosaveStale) {
                setStatus(message);
            }
        });
        return;
    }
    if (fAutosaveStale) {
        // A successful tick means the scratch on disk matches the model
        // again: either this call wrote it, or the fingerprint proves the
        // model never drifted past the last good write.
        fAutosaveStale = false;
        setStatus(QStringLiteral("autosave recovered - the scratch on disk is current again"));
    }
}

void DocumentController::autosaveNow(const char* what) {
    if (fScheduler->active()) {
        // Task 6.14b (review R14): the UI thread's whole job is the
        // snapshot copy — cheap, immutable, and the executor keeps only
        // the newest. The result reports back through
        // onAutosaveCompleted when the write is durable (or refused).
        fScheduler->submit(fDocuments, what, fDecisionEpoch);
        return;
    }
    std::string tickError;
    fScheduler->tickInline(fDocuments, &tickError);
    surfaceAutosaveResult(tickError, what);
}

void DocumentController::onAutosaveCompleted(const QString& what, const QString& error) {
    // No caller writes the status after this slot, so the refusal lands
    // immediately (the deferral exists only for the inline edit path).
    surfaceAutosaveResult(error.toStdString(), what.toLatin1().constData(), false);
}

void DocumentController::quiesceForDecision() {
    ++fDecisionEpoch;
    fScheduler->pauseForDecision(fDecisionEpoch);
}

void DocumentController::resumeAutosave() {
    fScheduler->resume(fDecisionEpoch);
}

bool DocumentController::clearAutosave(std::string* error) {
    return fScheduler->clearNow(error);
}

void DocumentController::startBackgroundPersistence() {
    fScheduler->start();
}

void DocumentController::awaitPersistence() {
    if (fScheduler->active()) {
        fScheduler->awaitIdle();
        QCoreApplication::processEvents();  // land the queued acknowledgements
    }
}

void DocumentController::prepareForQuit() {
    flushText();
    // The shutdown promise is durable, not queued: whatever the flush
    // committed must be on disk before the process is gone.
    if (fScheduler->active()) {
        fScheduler->awaitIdle();
    }
}

qulonglong DocumentController::autosaveWrites() const {
    return static_cast<qulonglong>(fScheduler->writes());
}

qulonglong DocumentController::autosaveCoalesced() const {
    return static_cast<qulonglong>(fScheduler->coalesced());
}

bool DocumentController::createSong(const QString& name) {
    flushText();
    openshow::domain::Id created;
    std::string error;
    if (!edit([&] { created = fDocuments.createShow(name.toStdString(), &error); return !created.empty(); })) {
        setStatus(QStringLiteral("new song: %1").arg(QString::fromStdString(error)));
        return false;
    }
    fCurrentSongId = created;
    fCurrentSlide = 0;
    emit navigationChanged();
    emit slideTextChanged();
    return true;
}

bool DocumentController::removeCurrentSong() {
    flushText();
    if (fCurrentSongId.empty()) {
        return false;
    }
    std::string error;
    if (!edit([&] { return fDocuments.removeShow(fCurrentSongId, &error); })) {
        setStatus(QStringLiteral("remove song: %1").arg(QString::fromStdString(error)));
        return false;
    }
    fCurrentSongId.clear();
    fCurrentSlide = 0;
    const std::vector<openshow::domain::Id> ids = fDocuments.showIds();
    if (!ids.empty()) {
        fCurrentSongId = ids.front();
    }
    emit navigationChanged();
    emit slideTextChanged();
    return true;
}

bool DocumentController::addSlide() {
    flushText();
    if (fCurrentSongId.empty()) {
        setStatus(QStringLiteral("add slide: select a song first"));
        return false;
    }
    openshow::domain::Id created;
    std::string error;
    if (!edit([&] { created = fDocuments.appendSlide(fCurrentSongId, &error); return !created.empty(); })) {
        setStatus(QStringLiteral("add slide: %1").arg(QString::fromStdString(error)));
        return false;
    }
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    fCurrentSlide = show != nullptr ? static_cast<int>(show->slides.size()) - 1 : 0;
    emit navigationChanged();
    emit slideTextChanged();
    return true;
}

bool DocumentController::removeCurrentSlide() {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        return false;
    }
    std::string error;
    if (!edit([&] { return fDocuments.removeSlide(fCurrentSongId, slideId, &error); })) {
        setStatus(QStringLiteral("remove slide: %1").arg(QString::fromStdString(error)));
        return false;
    }
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    const int count = show != nullptr ? static_cast<int>(show->slides.size()) : 0;
    if (fCurrentSlide >= count) {
        fCurrentSlide = count - 1;  // may become -1: no slides left
    }
    emit navigationChanged();
    emit slideTextChanged();
    return true;
}

void DocumentController::setSlideText(const QString& text) {
    if (currentSlideId().empty()) {
        return;  // typing with nothing selected edits nothing
    }
    fPendingText = text;
    fHasPendingText = true;
    fTextCommit.start();
}

void DocumentController::commitPendingText() {
    if (!fHasPendingText) {
        return;
    }
    fHasPendingText = false;
    if (fPendingText == currentSlideText()) {
        return;  // net effect of the typing burst was nothing
    }
    const openshow::domain::Id slideId = currentSlideId();
    const openshow::domain::Id songId = fCurrentSongId;
    std::vector<std::string> lines = textToLines(fPendingText);
    std::string error;
    // The edit lands on whichever slide the text was typed into, even if
    // the operator has since moved on; the guard above already flushed
    // before any navigation, so this is the timer path only.
    edit([&] { return fDocuments.setSlideLines(songId, slideId, lines, &error); });
}

void DocumentController::flushText() {
    commitPendingText();
}

void DocumentController::selectSong(const QString& songId) {
    flushText();
    const openshow::domain::Id id = songId.toStdString();
    if (id == fCurrentSongId || fDocuments.findShow(id) == nullptr) {
        return;
    }
    fCurrentSongId = id;
    fCurrentSlide = 0;
    emit navigationChanged();
    emit slideTextChanged();
}

void DocumentController::selectSlide(int index) {
    flushText();
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    if (show == nullptr) {
        return;
    }
    const int last = static_cast<int>(show->slides.size()) - 1;
    if (last < 0) {
        if (fCurrentSlide != -1) {  // a song without slides has no current index
            fCurrentSlide = -1;
            emit navigationChanged();
            emit slideTextChanged();
        }
        return;
    }
    const int clamped = index < 0 ? 0 : (index > last ? last : index);
    if (clamped == fCurrentSlide) {
        return;
    }
    fCurrentSlide = clamped;
    emit navigationChanged();
    emit slideTextChanged();
}

void DocumentController::stepSlide(int delta) {
    selectSlide(fCurrentSlide + delta);
}

bool DocumentController::setSlideGeometry(double x, double y, double width, double height) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("text box: no current slide"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setSlideGeometry(fCurrentSongId, slideId, x, y, width, height,
                                               &error);
        })) {
        setStatus(QStringLiteral("text box: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

// --- style panel (task 4.4) -------------------------------------------------

QVariantList DocumentController::styles() const {
    QVariantList list;
    for (const openshow::domain::Id& id : fDocuments.styleIds()) {
        const openshow::domain::Style* style = fDocuments.findStyle(id);
        if (style == nullptr) {
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(style->id));
        entry.insert(QStringLiteral("name"), QString::fromStdString(style->name));
        entry.insert(QStringLiteral("textColor"), QString::fromStdString(style->textColor));
        entry.insert(QStringLiteral("backgroundColor"),
                     QString::fromStdString(style->backgroundColor));
        entry.insert(QStringLiteral("fontFamily"), QString::fromStdString(style->fontFamily));
        entry.insert(QStringLiteral("fontSize"), style->fontSize);
        entry.insert(QStringLiteral("align"), QString::fromStdString(style->align));
        entry.insert(QStringLiteral("lineSpacing"), style->lineSpacing);
        list.append(entry);
    }
    return list;
}

// The current slide's domain Slide, or nullptr. Navigation already keeps
// fCurrentSlide inside the show, but every panel re-checks rather than
// trusting a stale index across a slide removal.
namespace {

const openshow::domain::Slide* findCurrentSlide(const openshow::domain::Documents& documents,
                                                const openshow::domain::Id& showId,
                                                const openshow::domain::Id& slideId) {
    const openshow::domain::Show* show = documents.findShow(showId);
    if (show == nullptr || slideId.empty()) {
        return nullptr;
    }
    for (const openshow::domain::Slide& slide : show->slides) {
        if (slide.id == slideId) {
            return &slide;
        }
    }
    return nullptr;
}

}  // namespace

QVariantMap DocumentController::currentStyle() const {
    QVariantMap map;
    const openshow::domain::Style* style =
        fDocuments.styleForSlide(fCurrentSongId, currentSlideId());
    if (style == nullptr) {
        return map;
    }
    map.insert(QStringLiteral("id"), QString::fromStdString(style->id));
    map.insert(QStringLiteral("name"), QString::fromStdString(style->name));
    map.insert(QStringLiteral("textColor"), QString::fromStdString(style->textColor));
    map.insert(QStringLiteral("backgroundColor"),
               QString::fromStdString(style->backgroundColor));
    map.insert(QStringLiteral("fontFamily"), QString::fromStdString(style->fontFamily));
    map.insert(QStringLiteral("fontSize"), style->fontSize);
    map.insert(QStringLiteral("align"), QString::fromStdString(style->align));
    map.insert(QStringLiteral("lineSpacing"), style->lineSpacing);
    return map;
}

bool DocumentController::currentStyleIsOverride() const {
    const openshow::domain::Slide* slide =
        findCurrentSlide(fDocuments, fCurrentSongId, currentSlideId());
    return slide != nullptr && !slide->styleId.empty();
}

bool DocumentController::applyStyleToCurrentSlide(const QString& styleId) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("style: no current slide"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setSlideStyle(fCurrentSongId, slideId, styleId.toStdString(),
                                            &error);
        })) {
        setStatus(QStringLiteral("style: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::setShowDefaultStyle(const QString& styleId) {
    flushText();
    if (fCurrentSongId.empty()) {
        setStatus(QStringLiteral("style: no current song"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setShowDefaultStyle(fCurrentSongId, styleId.toStdString(),
                                                  &error);
        })) {
        setStatus(QStringLiteral("style: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::editStyle(const QString& styleId, const QString& name,
                                   const QString& textColor, const QString& backgroundColor,
                                   const QString& fontFamily, double fontSize,
                                   const QString& align, double lineSpacing) {
    flushText();
    const std::string cleanName = name.trimmed().toStdString();
    if (cleanName.empty()) {
        setStatus(QStringLiteral("edit style: a style needs a name"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            // Appearance first: it is the part the renderer cannot honour
            // a lie about, and its refusal must leave the name alone too.
            if (!fDocuments.updateStyle(styleId.toStdString(), textColor.toStdString(),
                                        backgroundColor.toStdString(), fontFamily.toStdString(),
                                        fontSize, align.toStdString(), lineSpacing, &error)) {
                return false;
            }
            const openshow::domain::Style* style =
                fDocuments.findStyle(styleId.toStdString());
            if (style != nullptr && style->name != cleanName) {
                return fDocuments.renameStyle(styleId.toStdString(), cleanName, &error);
            }
            return true;
        })) {
        setStatus(QStringLiteral("edit style: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::createStyleFromCurrent(const QString& name) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("new style: no current slide"));
        return false;
    }
    // Snapshot what the slide draws now — resolved style or the built-in
    // defaults the renderer would reach without one — before history runs.
    const openshow::domain::Style* base = fDocuments.styleForSlide(fCurrentSongId, slideId);
    const std::string text = base != nullptr ? base->textColor : "#ffffff";
    const std::string background = base != nullptr ? base->backgroundColor : "#000000";
    const std::string family = base != nullptr ? base->fontFamily : "";
    const double size = base != nullptr ? base->fontSize : 0.0;
    const std::string alignment = base != nullptr ? base->align : "center";
    const double spacing =
        base != nullptr ? base->lineSpacing : openshow::domain::kDefaultLineSpacing;
    std::string error;
    openshow::domain::Id created;
    if (!edit([&] {
            created = fDocuments.createStyle(name.toStdString(), text, background, family,
                                             size, alignment, spacing, &error);
            if (created.empty()) {
                return false;
            }
            if (!fDocuments.setSlideStyle(fCurrentSongId, slideId, created, &error)) {
                // Nothing may survive a refused half of one history entry.
                fDocuments.removeStyle(created, nullptr);
                return false;
            }
            return true;
        })) {
        setStatus(QStringLiteral("new style: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::setSlideBackground(const QString& assetId) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("background: no current slide"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setSlideBackground(fCurrentSongId, slideId,
                                                 assetId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("background: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

// --- slide clipboard (task 4.5) ---------------------------------------------

bool DocumentController::copyCurrentSlide() {
    flushText();
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    const openshow::domain::Id slideId = currentSlideId();
    if (show == nullptr || slideId.empty()) {
        setStatus(QStringLiteral("copy: no current slide"));
        return false;
    }
    for (const openshow::domain::Slide& slide : show->slides) {
        if (slide.id != slideId) {
            continue;
        }
        fClip.valid = true;
        fClip.boxes = slide.items;
        for (openshow::domain::TextBox& box : fClip.boxes) {
            box.id.clear();  // a paste re-identifies (task 6.17a)
        }
        fClip.notes = slide.notes;
        fClip.styleId = slide.styleId;
        fClip.backgroundAssetId = slide.backgroundAssetId;
        emit stateChanged();  // canPaste
        return true;
    }
    setStatus(QStringLiteral("copy: no current slide"));
    return false;
}

bool DocumentController::cutCurrentSlide() {
    // Capture first; the removal is the only edit, so one undo restores
    // the slide exactly where it was.
    if (!copyCurrentSlide()) {
        return false;
    }
    return removeCurrentSlide();
}

bool DocumentController::pasteClip(const SlideClip& clip) {
    flushText();
    const openshow::domain::Id song = fCurrentSongId;
    if (song.empty()) {
        setStatus(QStringLiteral("paste: no current song"));
        return false;
    }
    const int at = fCurrentSlide >= 0 ? fCurrentSlide + 1 : 0;
    // A style or asset deleted since the capture falls away rather than
    // refusing the whole paste.
    std::string styleId = clip.styleId;
    if (!styleId.empty() && fDocuments.findStyle(styleId) == nullptr) {
        styleId.clear();
    }
    std::string background = clip.backgroundAssetId;
    if (!background.empty() && fDocuments.findAsset(background) == nullptr) {
        background.clear();
    }
    openshow::domain::Id inserted;
    std::string error;
    if (!edit([&] {
            inserted = fDocuments.insertSlide(song, static_cast<size_t>(at), &error);
            if (inserted.empty()) {
                return false;
            }
            // History will not roll a refused mutation back for us, so a
            // half-filled slide must leave on its own.
            bool ok = fDocuments.setSlideNotes(song, inserted, clip.notes, &error);
            // The inserted slide came with one default box. It takes the
            // clip's first box, and every further box is added on top of it
            // (task 6.17a), so a pasted slide keeps the original's draw order
            // while every box on it is a fresh identity: addTextBox drops the
            // id the clip held, which is what lets the same slide be pasted
            // twice.
            for (std::size_t i = 0; ok && i < clip.boxes.size(); ++i) {
                const openshow::domain::TextBox& box = clip.boxes[i];
                if (i == 0) {
                    ok = fDocuments.setSlideLines(song, inserted, box.lines, &error) &&
                         fDocuments.setSlideGeometry(song, inserted, box.x, box.y, box.width,
                                                     box.height, &error);
                } else {
                    ok = !fDocuments.addTextBox(song, inserted, box, &error).empty();
                }
            }
            if (ok && clip.boxes.empty()) {
                // A copied slide with no boxes at all pastes as a slide with
                // no boxes: the default box the new slide arrived with goes,
                // because a copy that quietly adds a box is not a copy.
                const openshow::domain::Show* made = fDocuments.findShow(song);
                if (made != nullptr) {
                    for (const openshow::domain::Slide& slide : made->slides) {
                        if (slide.id == inserted && !slide.items.empty()) {
                            ok = fDocuments.removeTextBox(song, inserted, slide.items.front().id,
                                                          &error);
                            break;
                        }
                    }
                }
            }
            if (ok) {
                ok = fDocuments.setSlideStyle(song, inserted, styleId, &error);
            }
            if (ok) {
                ok = fDocuments.setSlideBackground(song, inserted, background, &error);
            }
            if (!ok) {
                fDocuments.removeSlide(song, inserted, nullptr);
            }
            return ok;
        })) {
        setStatus(QStringLiteral("paste: %1").arg(QString::fromStdString(error)));
        return false;
    }
    if (const openshow::domain::Show* show = fDocuments.findShow(song)) {
        for (size_t i = 0; i < show->slides.size(); ++i) {
            if (show->slides[i].id == inserted) {
                fCurrentSlide = static_cast<int>(i);
                break;
            }
        }
    }
    emit navigationChanged();
    emit slideTextChanged();
    return true;
}

bool DocumentController::pasteSlide() {
    if (!fClip.valid) {
        setStatus(QStringLiteral("paste: the clipboard is empty"));
        return false;
    }
    return pasteClip(fClip);
}

bool DocumentController::duplicateCurrentSlide() {
    flushText();
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    const openshow::domain::Id slideId = currentSlideId();
    SlideClip clip;
    bool found = false;
    if (show != nullptr && !slideId.empty()) {
        for (const openshow::domain::Slide& slide : show->slides) {
            if (slide.id == slideId) {
                clip.boxes = slide.items;
                for (openshow::domain::TextBox& box : clip.boxes) {
                    box.id.clear();  // the duplicate gets its own (task 6.17a)
                }
                clip.notes = slide.notes;
                clip.styleId = slide.styleId;
                clip.backgroundAssetId = slide.backgroundAssetId;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        setStatus(QStringLiteral("duplicate: no current slide"));
        return false;
    }
    return pasteClip(clip);
}

// --- playlist and library drawer (task 4.2) --------------------------------

QVariantList DocumentController::playlists() const {
    QVariantList list;
    for (const openshow::domain::Id& id : fDocuments.playlistIds()) {
        const openshow::domain::Playlist* playlist = fDocuments.findPlaylist(id);
        if (playlist == nullptr) {
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(id));
        entry.insert(QStringLiteral("name"), QString::fromStdString(playlist->name));
        entry.insert(QStringLiteral("entryCount"), static_cast<int>(playlist->entries.size()));
        list.append(entry);
    }
    return list;
}

QVariantList DocumentController::playlistEntries() const {
    QVariantList list;
    const openshow::domain::Playlist* playlist = fDocuments.findPlaylist(fCurrentPlaylistId);
    if (playlist == nullptr) {
        return list;
    }
    int index = 0;
    for (const openshow::domain::PlaylistEntry& row : playlist->entries) {
        const openshow::domain::Show* show = fDocuments.findShow(row.showId);
        QVariantMap entry;
        entry.insert(QStringLiteral("entryId"), QString::fromStdString(row.id));
        entry.insert(QStringLiteral("index"), index++);
        entry.insert(QStringLiteral("showId"), QString::fromStdString(row.showId));
        entry.insert(QStringLiteral("showName"),
                     show != nullptr ? QString::fromStdString(show->name) : QStringLiteral("?"));
        entry.insert(QStringLiteral("slideCount"),
                     show != nullptr ? static_cast<int>(show->slides.size()) : 0);
        entry.insert(QStringLiteral("section"), QString::fromStdString(row.section));
        list.append(entry);
    }
    return list;
}

QVariantList DocumentController::assets() const {
    QVariantList list;
    for (const openshow::domain::Id& id : fDocuments.assetIds()) {
        const openshow::domain::Asset* asset = fDocuments.findAsset(id);
        if (asset == nullptr) {
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(id));
        entry.insert(QStringLiteral("name"), QString::fromStdString(asset->name));
        entry.insert(QStringLiteral("sourcePath"), QString::fromStdString(asset->sourcePath));
        list.append(entry);
    }
    return list;
}

bool DocumentController::createPlaylist(const QString& name) {
    flushText();
    openshow::domain::Id created;
    std::string error;
    if (!edit([&] {
            created = fDocuments.createPlaylist(name.toStdString(), &error);
            return !created.empty();
        })) {
        setStatus(QStringLiteral("new playlist: %1").arg(QString::fromStdString(error)));
        return false;
    }
    fCurrentPlaylistId = created;
    emit navigationChanged();
    return true;
}

void DocumentController::selectPlaylist(const QString& playlistId) {
    const openshow::domain::Id id = playlistId.toStdString();
    if (id == fCurrentPlaylistId || fDocuments.findPlaylist(id) == nullptr) {
        return;
    }
    fCurrentPlaylistId = id;
    emit navigationChanged();
    // playlistEntries follows the current playlist and its NOTIFY is
    // documentsChanged, so the drawer's rows need this nudge too.
    emit documentsChanged();
}

bool DocumentController::addShowToPlaylist(const QString& showId) {
    // An index past the end clamps to append inside insertShowInPlaylist.
    return insertShowInPlaylist(showId, -1);
}

bool DocumentController::insertShowInPlaylist(const QString& showId, int index) {
    flushText();
    if (fCurrentPlaylistId.empty()) {
        setStatus(QStringLiteral("playlist: nothing to add to"));
        return false;
    }
    const openshow::domain::Playlist* playlist = fDocuments.findPlaylist(fCurrentPlaylistId);
    const std::size_t count = playlist != nullptr ? playlist->entries.size() : 0;
    // -1 (and anything past the end) appends, like the domain's insert.
    const std::size_t at =
        index < 0 ? count
                  : std::min(static_cast<std::size_t>(index), count);
    const openshow::domain::Id id = showId.toStdString();
    std::string error;
    if (!edit([&] {
            return !fDocuments
                        .insertPlaylistEntry(fCurrentPlaylistId, id, std::string(), at, &error)
                        .empty();
        })) {
        setStatus(QStringLiteral("add to playlist: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::insertShowAfterCurrentShow(const QString& showId) {
    // The reference's Enter-from-search rule: land after the current show
    // when the playlist contains it, otherwise append.
    flushText();
    const openshow::domain::Playlist* playlist = fDocuments.findPlaylist(fCurrentPlaylistId);
    if (playlist != nullptr) {
        for (std::size_t i = 0; i < playlist->entries.size(); ++i) {
            if (playlist->entries[i].showId == fCurrentSongId) {
                return insertShowInPlaylist(showId, static_cast<int>(i) + 1);
            }
        }
    }
    return insertShowInPlaylist(showId, -1);
}

bool DocumentController::removePlaylistEntry(const QString& entryId) {
    flushText();
    std::string error;
    if (!edit([&] {
            return fDocuments.removePlaylistEntry(fCurrentPlaylistId, entryId.toStdString(),
                                                  &error);
        })) {
        setStatus(QStringLiteral("remove from playlist: %1")
                      .arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::movePlaylistEntry(const QString& entryId, int toIndex) {
    flushText();
    const openshow::domain::Playlist* playlist = fDocuments.findPlaylist(fCurrentPlaylistId);
    if (playlist == nullptr || playlist->entries.empty()) {
        return false;
    }
    // toIndex is the row's position after the move; clamp so keyboard and
    // drop paths can overshoot safely.
    const int last = static_cast<int>(playlist->entries.size()) - 1;
    const int clamped = toIndex < 0 ? 0 : (toIndex > last ? last : toIndex);
    std::string error;
    if (!edit([&] {
            return fDocuments.movePlaylistEntry(fCurrentPlaylistId, entryId.toStdString(),
                                                static_cast<std::size_t>(clamped), &error);
        })) {
        setStatus(QStringLiteral("reorder playlist: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

QVariantList DocumentController::searchShows(const QString& query) const {
    QList<LibraryRow> rows;
    for (const openshow::domain::Id& id : fDocuments.showIds()) {
        const openshow::domain::Show* show = fDocuments.findShow(id);
        if (show == nullptr) {
            continue;
        }
        LibraryRow row;
        row.id = QString::fromStdString(id);
        row.name = QString::fromStdString(show->name);
        QStringList lines;
        for (const openshow::domain::Slide& slide : show->slides) {
            // Every box's words are searchable (task 6.17a): an index over
            // box 0 only would hide a slide whose text lives in the box the
            // operator added second — the search would say the show is empty.
            for (const openshow::domain::TextBox& box : slide.items) {
                for (const std::string& line : box.lines) {
                    lines.append(QString::fromStdString(line));
                }
            }
        }
        row.content = lines.join(QLatin1Char('\n'));
        rows.append(row);
    }
    QVariantList list;
    for (const LibraryRow& row : librarysearch::filter(rows, query)) {
        const openshow::domain::Show* show = fDocuments.findShow(row.id.toStdString());
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), row.id);
        entry.insert(QStringLiteral("name"), row.name);
        entry.insert(QStringLiteral("slideCount"),
                     show != nullptr ? static_cast<int>(show->slides.size()) : 0);
        list.append(entry);
    }
    return list;
}

bool DocumentController::importMediaFile(const QString& filePath) {
    flushText();
    const QString name = QFileInfo(filePath).fileName();
    openshow::domain::Id created;
    std::string error;
    if (!edit([&] {
            created = fDocuments.addAsset(name.toStdString(), filePath.toStdString(), &error);
            return !created.empty();
        })) {
        setStatus(QStringLiteral("import media: %1").arg(QString::fromStdString(error)));
        return false;
    }
    setStatus(QStringLiteral("added %1 to the media library (save copies the bytes)")
                  .arg(name));
    return true;
}

bool DocumentController::removeAsset(const QString& assetId) {
    flushText();
    std::string error;
    if (!edit([&] { return fDocuments.removeAsset(assetId.toStdString(), &error); })) {
        setStatus(QStringLiteral("remove media: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

void DocumentController::seedDefaultPlaylist() {
    if (fDocuments.playlistIds().empty()) {
        fDocuments.createPlaylist("Service");
    }
    anchorPlaylistNavigation();
}

void DocumentController::anchorPlaylistNavigation() {
    if (!fCurrentPlaylistId.empty() &&
        fDocuments.findPlaylist(fCurrentPlaylistId) != nullptr) {
        return;
    }
    const std::vector<openshow::domain::Id> ids = fDocuments.playlistIds();
    fCurrentPlaylistId = ids.empty() ? openshow::domain::Id{} : ids.front();
}

QString DocumentController::compileSlideJson(const openshow::domain::Id& slideId) const {
    openshow::scene::Scene scene;
    std::string error;
    if (!openshow::scene::compileSlide(fDocuments, fCurrentSongId, slideId, &scene, &error)) {
        return QString();
    }
    const boost::json::value json = openshow::scene::toJson(scene);
    return QString::fromStdString(boost::json::serialize(json));
}

QString DocumentController::currentSceneJson() {
    flushText();  // the presentation must show what the operator just typed
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("present: nothing is selected"));
        return QString();
    }
    const QString json = compileSlideJson(slideId);
    if (json.isEmpty()) {
        setStatus(QStringLiteral("present: the slide did not compile"));
    }
    return json;
}

QString DocumentController::sceneJsonForSlide(const QString& slideId) {
    flushText();
    return compileSlideJson(slideId.toStdString());
}

bool DocumentController::save() {
    flushText();
    // Decision protocol (task 6.14b, review R14): before this save is
    // even attempted, the executor is quiesced — queued autosaves are
    // dropped, an in-flight write finishes, and the epoch vetoes anything
    // stamped older. So the scratch clear below can never race an older
    // autosave into resurrecting what this save supersedes.
    quiesceForDecision();
    std::string error;
    std::vector<openshow::domain::AssetRebind> rebinds;
    if (!openshow::domain::saveServiceAndAdopt(fDocuments, fServiceDirectory, &rebinds,
                                               &error)) {
        resumeAutosave();
        setStatus(QStringLiteral("save failed: %1").arg(QString::fromStdString(error)));
        return false;
    }
    // The model now names the package's own copies. The history snapshots
    // have to hear it too (task 6.12, review R7): the next lines delete the
    // autosave tree, which may be exactly where a pre-save snapshot points,
    // and an undo must not hand the model back a path to a directory that
    // no longer exists — that undo looks fine until the following present,
    // save or autosave tick tries to read the file.
    fHistory->adoptAssets(rebinds);
    // The service now holds everything the autosave was protecting. No
    // re-tick after clear: clear() resets the fingerprint, so a tick would
    // immediately write a package claiming unsaved work that does not
    // exist. The next real edit re-creates the autosave.
    fDirty = false;
    std::string clearError;
    if (!clearAutosave(&clearError)) {
        // The save COMMITTED; what failed is deleting the scratch the
        // save made stale. A later startup would recover what it holds as
        // if it were unsaved work — R14's silent resurrection — so the
        // survivor must be named, not swallowed.
        setStatus(QStringLiteral("saved %1, but the stale autosave could NOT be removed (%2); "
                                 "a later start may recover work this save already replaced")
                      .arg(QString::fromStdWString(fServiceDirectory.wstring()),
                           QString::fromStdString(clearError)));
    } else {
        setStatus(QStringLiteral("saved %1")
                      .arg(QString::fromStdWString(fServiceDirectory.wstring())));
    }
    resumeAutosave();
    if (!rebinds.empty()) {
        // Downstream caches of compiled scenes (the worker controller's
        // payload mirror) must stop naming the paths these files came from.
        QVariantMap adopted;
        for (const openshow::domain::AssetRebind& rebind : rebinds) {
            adopted.insert(QString::fromStdString(rebind.from),
                           QString::fromStdString(rebind.to));
        }
        emit assetsAdopted(adopted);
    }
    emit stateChanged();
    return true;
}

bool DocumentController::load() {
    flushText();
    // Same decision protocol as save() (task 6.14b): this load DISCARDS
    // local work, and no autosave older than that decision may survive
    // — queued, in flight, or acknowledged.
    quiesceForDecision();
    openshow::domain::Documents reloaded;
    std::string error;
    if (!openshow::domain::loadService(&reloaded, fServiceDirectory, &error)) {
        resumeAutosave();
        setStatus(QStringLiteral("load failed: %1").arg(QString::fromStdString(error)));
        return false;
    }
    fDocuments = std::move(reloaded);
    fHistory->clear();
    fDirty = false;
    const std::vector<openshow::domain::Id> ids = fDocuments.showIds();
    fCurrentSongId = ids.empty() ? openshow::domain::Id{} : ids.front();
    fCurrentSlide = 0;
    seedDefaultPlaylist();
    // Discarding local changes must also discard their autosave trail —
    // otherwise the next startup would recover the edits just thrown away.
    std::string clearError;
    if (!clearAutosave(&clearError)) {
        // Same contract as save() (task 6.14a, R14): the load itself
        // succeeded; the undeleted scratch is a resurrection risk and is
        // reported, never silently kept.
        setStatus(QStringLiteral("loaded %1, but the stale autosave could NOT be removed (%2); "
                                 "a later start may recover work this load just discarded")
                      .arg(QString::fromStdWString(fServiceDirectory.wstring()),
                           QString::fromStdString(clearError)));
    } else {
        setStatus(QStringLiteral("loaded %1")
                      .arg(QString::fromStdWString(fServiceDirectory.wstring())));
    }
    resumeAutosave();
    emit documentsChanged();
    emit navigationChanged();
    emit slideTextChanged();
    emit stateChanged();
    return true;
}

void DocumentController::undo() {
    flushText();
    std::string error;
    if (!fHistory->undo(&error)) {
        setStatus(QStringLiteral("undo: %1").arg(QString::fromStdString(error)));
        return;
    }
    // The restored state may have removed the current song or slide;
    // re-anchor navigation without losing the position where possible.
    if (fDocuments.findShow(fCurrentSongId) == nullptr) {
        const std::vector<openshow::domain::Id> ids = fDocuments.showIds();
        fCurrentSongId = ids.empty() ? openshow::domain::Id{} : ids.front();
        fCurrentSlide = 0;
        emit navigationChanged();
    } else {
        const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
        const int count = show != nullptr ? static_cast<int>(show->slides.size()) : 0;
        if (fCurrentSlide >= count) {
            fCurrentSlide = count - 1;
            emit navigationChanged();
        }
    }
    // Undo/redo can take the current playlist away too.
    anchorPlaylistNavigation();
    fDirty = true;
    autosaveNow("undo");
    emit documentsChanged();
    emit navigationChanged();
    emit slideTextChanged();
    emit stateChanged();
}

void DocumentController::redo() {
    flushText();
    std::string error;
    if (!fHistory->redo(&error)) {
        setStatus(QStringLiteral("redo: %1").arg(QString::fromStdString(error)));
        return;
    }
    if (fDocuments.findShow(fCurrentSongId) == nullptr) {
        const std::vector<openshow::domain::Id> ids = fDocuments.showIds();
        fCurrentSongId = ids.empty() ? openshow::domain::Id{} : ids.front();
        fCurrentSlide = 0;
        emit navigationChanged();
    } else {
        const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
        const int count = show != nullptr ? static_cast<int>(show->slides.size()) : 0;
        if (fCurrentSlide >= count) {
            fCurrentSlide = count - 1;
            emit navigationChanged();
        }
    }
    // Undo/redo can take the current playlist away too.
    anchorPlaylistNavigation();
    fDirty = true;
    autosaveNow("redo");
    emit documentsChanged();
    emit navigationChanged();
    emit slideTextChanged();
    emit stateChanged();
}

}  // namespace openshow::ui
