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
        // The row's position is the slide's FIRST box in draw order. The
        // canvas outlines EVERY box (currentSlideBoxes) and its gestures
        // address the selected one, but a list row names a slide, and the
        // box that represents a slide to someone scanning the list is the
        // bottom of its stack — the one a pre-6.17a slide had alone.
        // A slide authored down to no boxes reports the default box rather
        // than zeros, so a view drawing the row's outline is never handed a
        // rectangle with no size.
        const openshow::domain::SlideItem fallback;
        const openshow::domain::SlideItem& box =
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
        entry.insert(QStringLiteral("backgroundFit"),
                     QString::fromLatin1(
                         openshow::domain::backgroundFitName(slide.backgroundFit)));
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
    // The inspector edits the SELECTED box (task 6.17a2) — which is box 0
    // until the operator picks another, so a slide with one box and every
    // pre-6.17a file read the same as they always did.
    const openshow::domain::SlideItem* box = selectedBox();
    return box == nullptr ? QString() : linesToText(box->lines);
}

QString DocumentController::currentBoxKind() const {
    const openshow::domain::SlideItem* box = selectedBox();
    if (box == nullptr) {
        return QString();
    }
    return box->kind == openshow::domain::ItemKind::Image ? QStringLiteral("image")
                                                          : QStringLiteral("text");
}

QVariantMap DocumentController::currentBoxAsset() const {
    QVariantMap map;
    const openshow::domain::SlideItem* box = selectedBox();
    if (box == nullptr || box->kind != openshow::domain::ItemKind::Image) {
        return map;
    }
    const openshow::domain::Asset* asset = fDocuments.findAsset(box->assetId);
    if (asset == nullptr) {
        // The model drops image items when their asset goes, so this is a
        // file some other tool wrote; the inspector then says the picture
        // has lost its file rather than naming a stranger.
        return map;
    }
    map.insert(QStringLiteral("id"), QString::fromStdString(asset->id));
    map.insert(QStringLiteral("name"), QString::fromStdString(asset->name));
    map.insert(QStringLiteral("sourcePath"), QString::fromStdString(asset->sourcePath));
    return map;
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
    const openshow::domain::SlideItem* box = selectedBox();
    if (box == nullptr) {
        return;  // the box the words were typed into is gone; nothing to write
    }
    if (box->kind == openshow::domain::ItemKind::Image) {
        // Honest refusal (task 6.17b2): the model cannot hold words in a
        // picture, and an edit() that silently returned false would drop
        // the typed burst on the floor with no trace. The status line says
        // where the words did NOT go. Reaching this needs a caller that
        // types while a picture is selected — the inspector hides the
        // field exactly on that selection, so this is the backstop, not the
        // usual path.
        setStatus(QStringLiteral("text: the selected box shows a picture, not words"));
        return;
    }
    const openshow::domain::Id boxId = box->id;
    std::vector<std::string> lines = textToLines(fPendingText);
    std::string error;
    // The edit lands on whichever box the text was typed into, even if the
    // operator has since moved on; every path that moves the text or the
    // selection (slide navigation, selectTextBox, undo, a box edit) flushes
    // FIRST, so by the time this runs "selected" is still "typed into".
    // That flush is also what stops a burst of keystrokes from landing in
    // the box the operator tabbed to.
    edit([&] { return fDocuments.setItemLines(songId, slideId, boxId, lines, &error); });
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
    fCurrentBoxId.clear();  // the selection belongs to a slide, not to the session
    emit navigationChanged();
    emit slideTextChanged();
    emit boxSelectionChanged();
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
            fCurrentBoxId.clear();
            emit navigationChanged();
            emit slideTextChanged();
            emit boxSelectionChanged();
        }
        return;
    }
    const int clamped = index < 0 ? 0 : (index > last ? last : index);
    if (clamped == fCurrentSlide) {
        return;
    }
    fCurrentSlide = clamped;
    fCurrentBoxId.clear();
    emit navigationChanged();
    emit slideTextChanged();
    emit boxSelectionChanged();
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

// --- text-box authoring (task 6.17a2, review R17) ---------------------------

const openshow::domain::SlideItem* DocumentController::selectedBox() const {
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    if (show == nullptr || fCurrentSlide < 0 ||
        static_cast<std::size_t>(fCurrentSlide) >= show->slides.size()) {
        return nullptr;
    }
    const std::vector<openshow::domain::SlideItem>& items =
        show->slides[static_cast<std::size_t>(fCurrentSlide)].items;
    if (items.empty()) {
        return nullptr;
    }
    for (const openshow::domain::SlideItem& box : items) {
        if (box.id == fCurrentBoxId) {
            return &box;
        }
    }
    // The fallback is the bottom of the stack: a slide always has it (the
    // empty case returned above), it is the box a pre-6.17a slide had alone,
    // and because every box-scoped op resolves through this same function,
    // the box wearing the canvas's handles is necessarily the box the
    // inspector is editing.
    return &items.front();
}

int DocumentController::currentBox() const {
    const openshow::domain::SlideItem* box = selectedBox();
    if (box == nullptr) {
        return -1;
    }
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    const std::vector<openshow::domain::SlideItem>& items =
        show->slides[static_cast<std::size_t>(fCurrentSlide)].items;
    return static_cast<int>(box - items.data());
}

QVariantList DocumentController::currentSlideBoxes() const {
    QVariantList list;
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    if (show == nullptr || fCurrentSlide < 0 ||
        static_cast<std::size_t>(fCurrentSlide) >= show->slides.size()) {
        return list;
    }
    const std::vector<openshow::domain::SlideItem>& items =
        show->slides[static_cast<std::size_t>(fCurrentSlide)].items;
    for (std::size_t i = 0; i < items.size(); ++i) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(items[i].id));
        entry.insert(QStringLiteral("index"), static_cast<int>(i));
        // Kind travels with the box (task 6.17b): the canvas chrome can
        // already tell a picture's outline from a text box's without
        // guessing from empty words, and the 6.17b2 editor will show the
        // picture's asset here.
        entry.insert(QStringLiteral("kind"),
                     items[i].kind == openshow::domain::ItemKind::Image
                         ? QStringLiteral("image")
                         : QStringLiteral("text"));
        entry.insert(QStringLiteral("assetId"), QString::fromStdString(items[i].assetId));
        entry.insert(QStringLiteral("x"), items[i].x);
        entry.insert(QStringLiteral("y"), items[i].y);
        entry.insert(QStringLiteral("width"), items[i].width);
        entry.insert(QStringLiteral("height"), items[i].height);
        list.append(entry);
    }
    return list;
}

bool DocumentController::selectTextBox(int index) {
    // Characters typed into the previous box belong to that box: they land
    // in the model before the selection moves, or a burst of typing would
    // commit into whatever box the operator tabbed to.
    flushText();
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    if (show == nullptr || fCurrentSlide < 0 ||
        static_cast<std::size_t>(fCurrentSlide) >= show->slides.size()) {
        setStatus(QStringLiteral("text box: no current slide"));
        return false;
    }
    const std::vector<openshow::domain::SlideItem>& items =
        show->slides[static_cast<std::size_t>(fCurrentSlide)].items;
    if (items.empty()) {
        setStatus(QStringLiteral("text box: this slide has no text box to select"));
        return false;
    }
    const int last = static_cast<int>(items.size()) - 1;
    const int clamped = index < 0 ? 0 : (index > last ? last : index);
    if (items[static_cast<std::size_t>(clamped)].id == fCurrentBoxId) {
        return true;  // already there
    }
    fCurrentBoxId = items[static_cast<std::size_t>(clamped)].id;
    // Selection is chrome: no history entry, no dirty flag, no autosave.
    // The inspector's content moved with the selection, so it repaints too.
    emit boxSelectionChanged();
    emit slideTextChanged();
    return true;
}

int DocumentController::boxAt(double x, double y) const {
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    if (show == nullptr || fCurrentSlide < 0 ||
        static_cast<std::size_t>(fCurrentSlide) >= show->slides.size()) {
        return -1;
    }
    const std::vector<openshow::domain::SlideItem>& items =
        show->slides[static_cast<std::size_t>(fCurrentSlide)].items;
    // Back to front: the box drawn last paints over the others, so it is the
    // box under the pointer wherever they overlap, and the click should land
    // on what the operator can see.
    for (std::size_t i = items.size(); i-- > 0;) {
        const openshow::domain::SlideItem& box = items[i];
        if (x >= box.x && x <= box.x + box.width && y >= box.y && y <= box.y + box.height) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool DocumentController::addTextBox() {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("add text box: no current slide"));
        return false;
    }
    std::string error;
    openshow::domain::Id newId;
    if (!edit([&] {
            newId = fDocuments.addItem(fCurrentSongId, slideId, openshow::domain::SlideItem{},
                                          &error);
            return !newId.empty();
        })) {
        setStatus(QStringLiteral("add text box: %1").arg(QString::fromStdString(error)));
        return false;
    }
    fCurrentBoxId = newId;
    emit boxSelectionChanged();
    emit slideTextChanged();
    return true;
}

bool DocumentController::removeCurrentTextBox() {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    const openshow::domain::SlideItem* box = selectedBox();
    if (slideId.empty() || box == nullptr) {
        setStatus(QStringLiteral("delete text box: this slide has no text box to delete"));
        return false;
    }
    const openshow::domain::Id boxId = box->id;
    std::string error;
    if (!edit([&] { return fDocuments.removeItem(fCurrentSongId, slideId, boxId, &error); })) {
        setStatus(QStringLiteral("delete text box: %1").arg(QString::fromStdString(error)));
        return false;
    }
    // The id is deliberately left in place: it names nothing now, so the
    // selection resolves to the fallback — until an UNDO puts the box back,
    // at which point the selection is on the box it was deleted from again,
    // which is exactly where the operator is looking.
    emit boxSelectionChanged();
    emit slideTextChanged();
    return true;
}

bool DocumentController::moveCurrentTextBox(int toIndex) {
    flushText();
    const openshow::domain::SlideItem* box = selectedBox();
    if (box == nullptr) {
        setStatus(QStringLiteral("reorder text box: this slide has no text box to move"));
        return false;
    }
    if (toIndex < 0) {
        setStatus(QStringLiteral("reorder text box: index %1 is negative").arg(toIndex));
        return false;
    }
    const openshow::domain::Id slideId = currentSlideId();
    const openshow::domain::Id boxId = box->id;
    std::string error;
    if (!edit([&] {
            return fDocuments.moveItem(fCurrentSongId, slideId, boxId,
                                          static_cast<std::size_t>(toIndex), &error);
        })) {
        setStatus(QStringLiteral("reorder text box: %1").arg(QString::fromStdString(error)));
        return false;
    }
    // The box kept its identity and the selection follows identity, so the
    // handles stay on it — but its index moved, and chrome keys off the
    // index.
    emit boxSelectionChanged();
    return true;
}

bool DocumentController::setTextBoxGeometry(double x, double y, double width, double height) {
    flushText();
    const openshow::domain::SlideItem* box = selectedBox();
    if (box == nullptr) {
        setStatus(QStringLiteral("text box: this slide has no text box to resize"));
        return false;
    }
    const openshow::domain::Id slideId = currentSlideId();
    const openshow::domain::Id boxId = box->id;
    std::string error;
    if (!edit([&] {
            return fDocuments.setItemGeometry(fCurrentSongId, slideId, boxId, x, y, width,
                                                 height, &error);
        })) {
        setStatus(QStringLiteral("text box: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::placeAssetOnSlide(const QString& assetId) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("place picture: no current slide"));
        return false;
    }
    openshow::domain::SlideItem picture;
    picture.kind = openshow::domain::ItemKind::Image;
    picture.assetId = assetId.toStdString();
    // A half-canvas box centred on the stage, not the lyric default: a new
    // full-frame picture would sit exactly on the existing default box's
    // outline and read as "nothing happened", while this one shows up as a
    // visibly new box and still large enough to judge the picture in. The
    // operator drags it where they want — the same geometry commit every
    // other box gets.
    picture.x = 480.0;
    picture.y = 270.0;
    picture.width = 960.0;
    picture.height = 540.0;
    std::string error;
    openshow::domain::Id newId;
    // The domain owns every content rule here (unknown asset, video, the
    // shared eight-item ceiling), so the status line is its refusal, not a
    // second opinion written at the panel that could drift from it.
    if (!edit([&] {
            newId = fDocuments.addItem(fCurrentSongId, slideId, picture, &error);
            return !newId.empty();
        })) {
        setStatus(QStringLiteral("place picture: %1").arg(QString::fromStdString(error)));
        return false;
    }
    // Placed and selected: an operator drops a picture in order to place
    // it, so the handles go to it immediately — and the inspector flips to
    // its asset as the selection signal repaints it.
    fCurrentBoxId = newId;
    setStatus(QStringLiteral("picture placed on the slide"));
    emit boxSelectionChanged();
    emit slideTextChanged();
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

QString DocumentController::currentBackgroundFit() const {
    const openshow::domain::Slide* slide =
        findCurrentSlide(fDocuments, fCurrentSongId, currentSlideId());
    return slide == nullptr
               ? QString()
               : QString::fromLatin1(
                     openshow::domain::backgroundFitName(slide->backgroundFit));
}

bool DocumentController::setSlideBackgroundFit(const QString& fit) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("background fit: no current slide"));
        return false;
    }
    openshow::domain::BackgroundFit parsed;
    // Refused before the model sees it: the enum cannot carry a lie, and a
    // name the DOMAIN does not spell is not this editor's fit either — one
    // name list, checked by the function that owns it (6.17b1's rule).
    if (!openshow::domain::backgroundFitFromName(fit.toStdString(), &parsed)) {
        setStatus(QStringLiteral("background fit: '%1' is not a fit this build draws")
                      .arg(fit));
        return false;
    }
    // Blur on a VIDEO background is refused here, at the seam, not only at
    // the compile that happens when the slide goes to air (6.17c2): the
    // operator who picks "blurred backdrop" on a video clip should hear
    // "this build does not blur playing frames" from the editor, not watch
    // the presenter fail. The name test above, the asset test here — the
    // domain owns both rules, this is only where they are enforced early.
    if (parsed == openshow::domain::BackgroundFit::Blur) {
        const openshow::domain::Slide* slide =
            findCurrentSlide(fDocuments, fCurrentSongId, slideId);
        const openshow::domain::Asset* asset =
            slide == nullptr || slide->backgroundAssetId.empty()
                ? nullptr
                : fDocuments.findAsset(slide->backgroundAssetId);
        if (asset != nullptr && openshow::domain::assetPathLooksLikeVideo(asset->sourcePath)) {
            setStatus(QStringLiteral(
                "background fit: this build does not blur playing frames — "
                "set an image background first"));
            return false;
        }
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setSlideBackgroundFit(fCurrentSongId, slideId, parsed,
                                                    &error);
        })) {
        setStatus(QStringLiteral("background fit: %1").arg(QString::fromStdString(error)));
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
        for (openshow::domain::SlideItem& box : fClip.boxes) {
            box.id.clear();  // a paste re-identifies (task 6.17a)
        }
        fClip.notes = slide.notes;
        fClip.styleId = slide.styleId;
        fClip.backgroundAssetId = slide.backgroundAssetId;
        fClip.backgroundFit = slide.backgroundFit;
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
            // The inserted slide came with one default text box, and the
            // clip's stack REPLACES it wholesale. Task 6.17b made that the
            // only honest route: the legacy single-text view addresses words
            // and the clip's first item may be a picture. Remove the default
            // box, then re-add every clip item in draw order through
            // addItem, which issues the fresh identity (addTextBox always
            // dropped the id the clip held — that is what lets the same
            // slide be pasted twice) and enforces the kind rules the copied
            // stack already satisfies. An item whose asset was deleted since
            // the capture falls away with it, like the style and the
            // background reference do.
            if (ok) {
                const openshow::domain::Show* made = fDocuments.findShow(song);
                if (made != nullptr) {
                    for (const openshow::domain::Slide& slide : made->slides) {
                        if (slide.id == inserted && !slide.items.empty()) {
                            ok = fDocuments.removeItem(song, inserted,
                                                       slide.items.front().id, &error);
                            break;
                        }
                    }
                }
            }
            for (std::size_t i = 0; ok && i < clip.boxes.size(); ++i) {
                openshow::domain::SlideItem box = clip.boxes[i];
                if (box.kind == openshow::domain::ItemKind::Image &&
                    fDocuments.findAsset(box.assetId) == nullptr) {
                    continue;
                }
                ok = !fDocuments.addItem(song, inserted, box, &error).empty();
            }
            if (ok) {
                ok = fDocuments.setSlideStyle(song, inserted, styleId, &error);
            }
            if (ok) {
                ok = fDocuments.setSlideBackground(song, inserted, background, &error);
            }
            if (ok) {
                // The clip's fit rides along (6.17c1): pasting a contained
                // slide gets a contained slide, not one silently redrawn
                // cropped.
                ok = fDocuments.setSlideBackgroundFit(song, inserted, clip.backgroundFit,
                                                      &error);
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
                for (openshow::domain::SlideItem& box : clip.boxes) {
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
            for (const openshow::domain::SlideItem& box : slide.items) {
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
    fCurrentBoxId.clear();  // a fresh file is a fresh selection
    emit documentsChanged();
    emit navigationChanged();
    emit slideTextChanged();
    emit boxSelectionChanged();
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
    // The restored stack may no longer hold the selected box (or may have
    // just brought it back, undo of a delete — the id is still there either
    // way and the fallback resolves it). Chrome must repaint regardless.
    emit boxSelectionChanged();
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
    emit boxSelectionChanged();  // same rule as undo
    emit stateChanged();
}

}  // namespace openshow::ui
