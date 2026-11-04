#include "DocumentController.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>

#include "openshow/scene/scene.h"

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

// '\n'-joined lines is DocumentViews::linesAsText now (task 6.19b2): the
// inspector's text and the debounce comparison read through the same one
// conversion. Splitting text back into lines stays here — that is the
// edit's side of the seam, and this file is the only caller.

}  // namespace

DocumentController::DocumentController(QObject* parent, std::filesystem::path baseDirectory)
    : QObject(parent), fViews(fDocuments) {
    if (baseDirectory.empty()) {
        baseDirectory = std::filesystem::path(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString());
    }
    // Durability is the adapter's (task 6.19b1); the one thing it needs back is
    // the line the operator reads, and that stays this class's property.
    fPersistence = new DocumentPersistence(
        DocumentPersistence::Sink{[this](const QString& text) { setStatus(text); }},
        baseDirectory, this);
    fHistory = std::make_unique<openshow::domain::History>(&fDocuments);

    // Task 2.3's startup rule: autosave data, when present, is newer than
    // the saved service and wins; a missing service simply begins empty.
    const DocumentPersistence::Startup startup = fPersistence->open(&fDocuments);
    fDirty = startup.recoveredUnsaved;
    fStatus = startup.status;

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

// --- view rows (the shapes are DocumentViews'; task 6.19b2) -----------------
//
// These property getters forward: the row keys, the missing-show and
// missing-asset fallbacks and the search projection live in one reader over
// the documents, and the cursor below is the only way navigation reaches
// it. The controller keeps deciding WHERE the operator is and WHAT changes
// the model; how a row is spelled is not its concern.

QVariantList DocumentController::songs() const {
    return fViews.songs();
}

QVariantList DocumentController::slides() const {
    return fViews.slides(fCurrentSongId);
}

QString DocumentController::currentSlideText() const {
    return fViews.boxText(selectedBox());
}

QString DocumentController::currentBoxKind() const {
    return fViews.boxKind(selectedBox());
}

QVariantMap DocumentController::currentBoxAsset() const {
    return fViews.boxAsset(selectedBox());
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

// --- durability (the adapter is task 6.19b1) ---------------------------------
//
// Every one of these forwards: the scratch, its executor, the decision protocol
// and the wording of a refusal or a survivor are DocumentPersistence's. What
// stays here is only the call each of the controller's own operations makes on
// the way out of a committed change, and the two decision points (save, load)
// where a durability answer changes what the controller owns.

void DocumentController::autosaveNow(const char* what) {
    fPersistence->autosave(fDocuments, what);
}

void DocumentController::startBackgroundPersistence() {
    fPersistence->start();
}

void DocumentController::awaitPersistence() {
    fPersistence->awaitIdleAndDeliver();
}

void DocumentController::prepareForQuit() {
    flushText();
    // The shutdown promise is durable, not queued: whatever the flush
    // committed must be on disk before the process is gone.
    fPersistence->awaitDurable();
}

qulonglong DocumentController::autosaveWrites() const {
    return fPersistence->writes();
}

qulonglong DocumentController::autosaveCoalesced() const {
    return fPersistence->coalesced();
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

DocumentViews::Cursor DocumentController::viewCursor() const {
    DocumentViews::Cursor cursor;
    cursor.showId = fCurrentSongId;
    cursor.slideIndex = fCurrentSlide;
    cursor.boxId = fCurrentBoxId;
    cursor.playlistId = fCurrentPlaylistId;
    return cursor;
}

const openshow::domain::SlideItem* DocumentController::selectedBox() const {
    return fViews.selectedBox(viewCursor());
}

int DocumentController::currentBox() const {
    return fViews.selectedBoxIndex(viewCursor());
}

QVariantList DocumentController::currentSlideBoxes() const {
    return fViews.slideBoxes(viewCursor());
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
    const openshow::domain::Slide& slide = show->slides[static_cast<std::size_t>(fCurrentSlide)];
    const std::vector<openshow::domain::SlideItem>& items = slide.items;
    // A box the chain places (amendment 2026-11-01) is grabbed where it
    // DRAWS: the outline under the pointer is the audience's rectangle, and
    // a hit rule that read the authored one would put the handles somewhere
    // the operator can see nothing.
    const int placed = fViews.placedBoxIndex(viewCursor());
    const std::optional<openshow::domain::BoxRect> look =
        placed < 0 ? std::optional<openshow::domain::BoxRect>()
                   : fDocuments.appearanceForSlide(fCurrentSongId, slide.id).placement;
    // Back to front: the box drawn last paints over the others, so it is the
    // box under the pointer wherever they overlap, and the click should land
    // on what the operator can see.
    for (std::size_t i = items.size(); i-- > 0;) {
        const openshow::domain::SlideItem& box = items[i];
        const bool applied = look.has_value() && static_cast<int>(i) == placed;
        const double left = applied ? look->x : box.x;
        const double top = applied ? look->y : box.y;
        const double width = applied ? look->width : box.width;
        const double height = applied ? look->height : box.height;
        if (x >= left && x <= left + width && y >= top && y <= top + height) {
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
    // The PRIMARY lyric box is never moved (amendment 2026-11-02, the
    // operator's "positional information still persisted"): dragging it or
    // typing its numbers says "this slide wants its OWN spot", and the four
    // numbers land as one placement CUSTOMIZATION whether or not any theme
    // places — so whatever the operator did is always revertible, back to
    // the theme or to the authored rectangle, instead of rewriting the
    // slide's own geometry out from under the undo. The authored rectangle
    // survives underneath. Every other box — an extra lyric box, a picture —
    // is geometry exactly as it always was.
    const DocumentViews::Cursor cursor = viewCursor();
    const int primary = fViews.primaryBoxIndex(cursor);
    if (primary >= 0 && fViews.selectedBoxIndex(cursor) == primary) {
        openshow::domain::SlideCustomization changed;
        bool onSlide = false;
        const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
        if (show != nullptr) {
            for (const openshow::domain::Slide& slide : show->slides) {
                if (slide.id == slideId) {
                    changed = slide.customizations;
                    onSlide = true;
                    break;
                }
            }
        }
        if (!onSlide) {
            setStatus(QStringLiteral("text box: no current slide"));
            return false;
        }
        changed.placement = openshow::domain::BoxRect{x, y, width, height};
        if (!edit([&] {
                return fDocuments.setSlideCustomizations(fCurrentSongId, slideId, changed,
                                                         &error);
            })) {
            setStatus(QStringLiteral("text box: %1").arg(QString::fromStdString(error)));
            return false;
        }
        return true;
    }
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

// --- theme panel (task 4.4) -------------------------------------------------

QVariantList DocumentController::themes() const {
    return fViews.themes();
}

QVariantMap DocumentController::currentTheme() const {
    return fViews.currentTheme(viewCursor());
}

bool DocumentController::currentThemeIsOverride() const {
    return fViews.themeIsOverride(viewCursor());
}

bool DocumentController::applyThemeToCurrentSlide(const QString& themeId) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("theme: no current slide"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setSlideTheme(fCurrentSongId, slideId, themeId.toStdString(),
                                            &error);
        })) {
        setStatus(QStringLiteral("theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::setShowDefaultTheme(const QString& themeId) {
    flushText();
    if (fCurrentSongId.empty()) {
        setStatus(QStringLiteral("theme: no current song"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setShowDefaultTheme(fCurrentSongId, themeId.toStdString(),
                                                  &error);
        })) {
        setStatus(QStringLiteral("theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::editTheme(const QString& themeId, const QString& name,
                                   const QString& textColor, const QString& backgroundColor,
                                   const QString& backgroundGradient,
                                   const QString& fontFamily, double fontSize,
                                   const QString& align, double lineSpacing) {
    flushText();
    const std::string cleanName = name.trimmed().toStdString();
    if (cleanName.empty()) {
        setStatus(QStringLiteral("edit theme: a theme needs a name"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            // Appearance first: it is the part the renderer cannot honour
            // a lie about, and its refusal must leave the name alone too.
            if (!fDocuments.updateTheme(themeId.toStdString(), textColor.toStdString(),
                                        backgroundColor.toStdString(),
                                        backgroundGradient.toStdString(),
                                        fontFamily.toStdString(),
                                        fontSize, align.toStdString(), lineSpacing, &error)) {
                return false;
            }
            const openshow::domain::Theme* theme =
                fDocuments.findTheme(themeId.toStdString());
            if (theme != nullptr && theme->name != cleanName) {
                return fDocuments.renameTheme(themeId.toStdString(), cleanName, &error);
            }
            return true;
        })) {
        setStatus(QStringLiteral("edit theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::setThemePlacement(const QString& themeId, double x, double y,
                                           double width, double height) {
    flushText();
    std::string error;
    const openshow::domain::BoxRect rect{x, y, width, height};
    if (!edit([&] {
            return fDocuments.setThemePlacement(themeId.toStdString(), rect, &error);
        })) {
        setStatus(QStringLiteral("theme placement: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::clearThemePlacement(const QString& themeId) {
    flushText();
    std::string error;
    if (!edit([&] {
            return fDocuments.clearThemePlacement(themeId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("theme placement: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

namespace {

// The style verb's strict SHAPE check (task TS.1). The bands belong to the
// domain (setThemeTextStyle quotes validThemeTextStyle); this function only
// decides whether the QVariantMap even has the shape of that sentence —
// unknown keys refused naming themselves, a weight that must BE an integer
// rather than become one ("700" from a text field and 700 from a spin box
// are not the same answer, and defaulting the difference is how a theme
// gets a value nobody typed), and effect maps required member-for-member
// whole. A shape this refuses is a theme the save would refuse right back.
bool styleFromMap(const QVariantMap& style, openshow::domain::ThemeTextStyle* out,
                  std::string* error) {
    static const QStringList kAllowed{QStringLiteral("fontWeight"), QStringLiteral("fontStyle"),
                                      QStringLiteral("fitMode"), QStringLiteral("minFontSize"),
                                      QStringLiteral("stroke"), QStringLiteral("shadow")};
    for (const QString& key : style.keys()) {
        if (!kAllowed.contains(key)) {
            *error = "'" + key.toStdString() + "' is not a text-style field";
            return false;
        }
    }
    if (style.contains(QStringLiteral("fontWeight"))) {
        const QVariant value = style.value(QStringLiteral("fontWeight"));
        const int type = value.userType();
        const bool integral = type == QMetaType::Int || type == QMetaType::UInt ||
                              type == QMetaType::LongLong || type == QMetaType::ULongLong;
        if (!integral) {
            *error = "font weight must be an integer, not a string or a decimal";
            return false;
        }
        out->fontWeight = value.toInt();
    }
    if (style.contains(QStringLiteral("fontStyle"))) {
        out->fontStyle = style.value(QStringLiteral("fontStyle")).toString().toStdString();
    }
    if (style.contains(QStringLiteral("fitMode"))) {
        out->fitMode = style.value(QStringLiteral("fitMode")).toString().toStdString();
    }
    if (style.contains(QStringLiteral("minFontSize"))) {
        const QVariant value = style.value(QStringLiteral("minFontSize"));
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok) {
            *error = "minimum font size must be a number";
            return false;
        }
        out->minFontSize = number;
    }
    if (style.contains(QStringLiteral("stroke"))) {
        const QVariant value = style.value(QStringLiteral("stroke"));
        if (value.userType() != QMetaType::QVariantMap ) {
            *error = "stroke is a whole record: {enabled, color, width}";
            return false;
        }
        const QVariantMap record = value.toMap();
        static const QStringList kMembers{QStringLiteral("enabled"), QStringLiteral("color"),
                                          QStringLiteral("width")};
        for (const QString& key : record.keys()) {
            if (!kMembers.contains(key)) {
                *error = "stroke has no member '" + key.toStdString() + "'";
                return false;
            }
        }
        openshow::domain::TextStroke stroke;
        for (const QString& member : kMembers) {
            if (!record.contains(member)) {
                *error = "stroke is a whole record: member '" + member.toStdString() +
                         "' is missing";
                return false;
            }
        }
        if (record.value(QStringLiteral("enabled")).userType() != QMetaType::Bool) {
            *error = "stroke 'enabled' must be true or false";
            return false;
        }
        stroke.enabled = record.value(QStringLiteral("enabled")).toBool();
        stroke.color = record.value(QStringLiteral("color")).toString().toStdString();
        bool ok = false;
        stroke.width = record.value(QStringLiteral("width")).toDouble(&ok);
        if (!ok) {
            *error = "stroke 'width' must be a number";
            return false;
        }
        out->stroke = stroke;
    }
    if (style.contains(QStringLiteral("shadow"))) {
        const QVariant value = style.value(QStringLiteral("shadow"));
        if (value.userType() != QMetaType::QVariantMap ) {
            *error = "shadow is a whole record: {enabled, color, opacity, offsetX, offsetY, "
                     "blurSigma}";
            return false;
        }
        const QVariantMap record = value.toMap();
        static const QStringList kMembers{QStringLiteral("enabled"), QStringLiteral("color"),
                                          QStringLiteral("opacity"), QStringLiteral("offsetX"),
                                          QStringLiteral("offsetY"), QStringLiteral("blurSigma")};
        for (const QString& key : record.keys()) {
            if (!kMembers.contains(key)) {
                *error = "shadow has no member '" + key.toStdString() + "'";
                return false;
            }
        }
        openshow::domain::TextShadow shadow;
        for (const QString& member : kMembers) {
            if (!record.contains(member)) {
                *error = "shadow is a whole record: member '" + member.toStdString() +
                         "' is missing";
                return false;
            }
        }
        if (record.value(QStringLiteral("enabled")).userType() != QMetaType::Bool) {
            *error = "shadow 'enabled' must be true or false";
            return false;
        }
        shadow.enabled = record.value(QStringLiteral("enabled")).toBool();
        shadow.color = record.value(QStringLiteral("color")).toString().toStdString();
        bool ok = false;
        shadow.opacity = record.value(QStringLiteral("opacity")).toDouble(&ok);
        if (!ok) {
            *error = "shadow 'opacity' must be a number";
            return false;
        }
        shadow.offsetX = record.value(QStringLiteral("offsetX")).toDouble(&ok);
        if (!ok) {
            *error = "shadow 'offsetX' must be a number";
            return false;
        }
        shadow.offsetY = record.value(QStringLiteral("offsetY")).toDouble(&ok);
        if (!ok) {
            *error = "shadow 'offsetY' must be a number";
            return false;
        }
        shadow.blurSigma = record.value(QStringLiteral("blurSigma")).toDouble(&ok);
        if (!ok) {
            *error = "shadow 'blurSigma' must be a number";
            return false;
        }
        out->shadow = shadow;
    }
    return true;
}

}  // namespace

bool DocumentController::setThemeTextStyle(const QString& themeId, const QVariantMap& style) {
    flushText();
    openshow::domain::ThemeTextStyle next;
    std::string error;
    if (!styleFromMap(style, &next, &error)) {
        setStatus(QStringLiteral("theme style: %1").arg(QString::fromStdString(error)));
        return false;
    }
    // One verb, whole set (task TS.1): the domain vets the bands — weight in
    // band, the two word sets, the bounded minimum, complete effect records —
    // and the edit() wrapper is what makes the undo cost what it changes.
    if (!edit([&] {
            return fDocuments.setThemeTextStyle(themeId.toStdString(), next, &error);
        })) {
        setStatus(QStringLiteral("theme style: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

QVariantMap DocumentController::themeContext(const QString& themeId, const QString& name) const {
    QVariantMap out;
    out.insert(QStringLiteral("set"), false);
    const openshow::domain::Theme* theme = fDocuments.findTheme(themeId.toStdString());
    if (theme == nullptr || name.trimmed().isEmpty()) {
        return out;
    }
    for (const openshow::domain::TextContext& context : theme->contexts) {
        if (context.name != name.trimmed().toStdString()) {
            continue;
        }
        out.insert(QStringLiteral("set"), true);
        out.insert(QStringLiteral("name"), QString::fromStdString(context.name));
        out.insert(QStringLiteral("fontFamily"),
                   context.fontFamily.has_value()
                       ? QString::fromStdString(*context.fontFamily)
                       : QString());
        out.insert(QStringLiteral("fontFamilySet"), context.fontFamily.has_value());
        out.insert(QStringLiteral("fontSize"), context.fontSize.value_or(0.0));
        out.insert(QStringLiteral("fontSizeSet"), context.fontSize.has_value());
        out.insert(QStringLiteral("lineSpacing"), context.lineSpacing.value_or(0.0));
        out.insert(QStringLiteral("lineSpacingSet"), context.lineSpacing.has_value());
        out.insert(QStringLiteral("align"),
                   context.align.has_value() ? QString::fromStdString(*context.align)
                                             : QString());
        out.insert(QStringLiteral("alignSet"), context.align.has_value());
        const openshow::domain::BoxRect box = context.box.value_or(openshow::domain::BoxRect{});
        out.insert(QStringLiteral("boxSet"), context.box.has_value());
        out.insert(QStringLiteral("x"), box.x);
        out.insert(QStringLiteral("y"), box.y);
        out.insert(QStringLiteral("width"), box.width);
        out.insert(QStringLiteral("height"), box.height);
        out.insert(QStringLiteral("insetSet"), context.inset.has_value());
        out.insert(QStringLiteral("inset"), context.inset.value_or(0.0));
        return out;
    }
    return out;  // no entry with that name: `set` false, nothing else
}

bool DocumentController::applyThemeContext(const QString& themeId, const QString& name,
                                           const QVariantMap& fields) {
    flushText();
    std::string error;
    const std::string contextName = name.trimmed().toStdString();
    if (contextName.empty()) {
        setStatus(QStringLiteral("reading context: a context needs a name"));
        return false;
    }
    if (!edit([&] {
            // The merge reads the CURRENT set inside the edit: the entry
            // named here is replaced (or added) and everything else —
            // every other context, field for field — travels untouched,
            // because the domain mutator, like the file, replaces the
            // whole set. Absent keys are silence in the new entry, and
            // the mutator's vetting is the loader's, so what the editor
            // accepts the next save cannot refuse.
            const openshow::domain::Theme* current =
                fDocuments.findTheme(themeId.toStdString());
            if (current == nullptr) {
                error = "no theme '" + themeId.toStdString() + "'";
                return false;
            }
            std::vector<openshow::domain::TextContext> contexts = current->contexts;
            openshow::domain::TextContext context;
            context.name = contextName;
            if (fields.contains(QStringLiteral("fontFamily"))) {
                context.fontFamily = fields.value(QStringLiteral("fontFamily")).toString().toStdString();
            }
            if (fields.contains(QStringLiteral("fontSize"))) {
                context.fontSize = fields.value(QStringLiteral("fontSize")).toDouble();
            }
            if (fields.contains(QStringLiteral("lineSpacing"))) {
                context.lineSpacing = fields.value(QStringLiteral("lineSpacing")).toDouble();
            }
            if (fields.contains(QStringLiteral("align"))) {
                context.align = fields.value(QStringLiteral("align")).toString().toStdString();
            }
            if (fields.contains(QStringLiteral("inset"))) {
                context.inset = fields.value(QStringLiteral("inset")).toDouble();
            }
            if (fields.contains(QStringLiteral("box"))) {
                const QVariantMap box = fields.value(QStringLiteral("box")).toMap();
                openshow::domain::BoxRect rect;
                rect.x = box.value(QStringLiteral("x")).toDouble();
                rect.y = box.value(QStringLiteral("y")).toDouble();
                rect.width = box.value(QStringLiteral("width")).toDouble();
                rect.height = box.value(QStringLiteral("height")).toDouble();
                context.box = rect;
            }
            bool replaced = false;
            for (openshow::domain::TextContext& existing : contexts) {
                if (existing.name == contextName) {
                    existing = context;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                contexts.push_back(std::move(context));
            }
            return fDocuments.setThemeContexts(themeId.toStdString(), std::move(contexts),
                                               &error);
        })) {
        setStatus(QStringLiteral("reading context: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::removeThemeContext(const QString& themeId, const QString& name) {
    flushText();
    std::string error;
    const std::string contextName = name.trimmed().toStdString();
    if (!edit([&] {
            const openshow::domain::Theme* current =
                fDocuments.findTheme(themeId.toStdString());
            if (current == nullptr) {
                error = "no theme '" + themeId.toStdString() + "'";
                return false;
            }
            std::vector<openshow::domain::TextContext> kept;
            for (const openshow::domain::TextContext& context : current->contexts) {
                if (context.name != contextName) {
                    kept.push_back(context);
                }
            }
            if (kept.size() == current->contexts.size()) {
                error = "theme '" + themeId.toStdString() + "' carries no context '" +
                        contextName + "' to remove";
                return false;
            }
            return fDocuments.setThemeContexts(themeId.toStdString(), std::move(kept), &error);
        })) {
        setStatus(QStringLiteral("reading context: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::createThemeFromCurrent(const QString& name) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("new theme: no current slide"));
        return false;
    }
    // Snapshot what the slide draws now — the whole resolved chain, from the
    // slide's own customizations down, or the built-in defaults the renderer
    // would reach where no layer spoke — before history runs (task TH.3:
    // "wear what this slide draws" is the chain's answer, not one layer's).
    const openshow::domain::ResolvedAppearance base =
        fDocuments.appearanceForSlide(fCurrentSongId, slideId);
    const std::string text = base.textColor.empty() ? "#ffffff" : base.textColor;
    const std::string background = base.backgroundColor.empty() ? "#000000"
                                                                : base.backgroundColor;
    // A theme made from a gradient slide starts WITH the gradient: "wear
    // whatever the current slide draws now" includes its second colour
    // (task 6.17c3), and a built-in default has never had one.
    const std::string gradient = base.backgroundGradient;
    const std::string family = base.fontFamily;
    const double size = base.fontSize;
    const std::string alignment = base.align.empty() ? "center" : base.align;
    const double spacing = base.lineSpacing;
    // "Whatever this slide draws now" includes WHERE its words are
    // (amendment 2026-11-01): the placement the chain resolves becomes the
    // new theme's own, so the slides that go on wearing it hold the line.
    const std::optional<openshow::domain::BoxRect> placement = base.placement;
    // "What the slide draws now" includes HOW the words are set (task
    // TS.1): the chain's CONCRETE style answers become the new theme's own,
    // so a theme taken from a styled slide keeps the weight, the slant,
    // the fit policy, the minimum and both effect records — a disabled
    // record travels as the explicit refusal it is, and the new theme
    // wearing itself later draws exactly the pixels this slide draws now.
    const openshow::domain::ThemeTextStyle style{
        base.fontWeight, base.fontStyle, base.fitMode, base.minFontSize, base.stroke,
        base.shadow};
    std::string error;
    openshow::domain::Id created;
    if (!edit([&] {
            created = fDocuments.createTheme(name.toStdString(), text, background, gradient,
                                             family, size, alignment, spacing, &error);
            if (created.empty()) {
                return false;
            }
            if (placement.has_value() &&
                !fDocuments.setThemePlacement(created, *placement, &error)) {
                fDocuments.removeTheme(created, nullptr);
                return false;
            }
            if (!fDocuments.setThemeTextStyle(created, style, &error)) {
                fDocuments.removeTheme(created, nullptr);
                return false;
            }
            if (!fDocuments.setSlideTheme(fCurrentSongId, slideId, created, &error)) {
                // Nothing may survive a refused half of one history entry.
                fDocuments.removeTheme(created, nullptr);
                return false;
            }
            return true;
        })) {
        setStatus(QStringLiteral("new theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

// --- theme service (task TH.4, themes plan) --------------------------------

QString DocumentController::wornThemeId() const {
    return QString::fromStdString(fDocuments.currentThemeId());
}

bool DocumentController::useThemeAsCurrent(const QString& themeId) {
    flushText();
    std::string error;
    if (!edit([&] {
            return fDocuments.setCurrentTheme(themeId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("current theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::createBlankTheme(const QString& name) {
    flushText();
    std::string error;
    if (!edit([&] {
            // The built-in look is the compile's own floor, not the panel's
            // guess: white on black, platform font, renderer size, centred.
            return !fDocuments.createTheme(name.toStdString(), "#ffffff", "#000000", "", "",
                                           0.0, "center",
                                           openshow::domain::kDefaultLineSpacing,
                                           &error)
                            .empty();
        })) {
        setStatus(QStringLiteral("new theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::duplicateTheme(const QString& themeId) {
    flushText();
    std::string error;
    if (!edit([&] {
            const openshow::domain::Theme* original =
                fDocuments.findTheme(themeId.toStdString());
            if (original == nullptr) {
                error = "no theme '" + themeId.toStdString() + "'";
                return false;
            }
            // The copy is taken UP FRONT: createTheme below appends to the
            // very container this pointer reads through, and a pointer into
            // a vector that reallocates is a pointer into freed memory.
            // (It read its fields through stale bytes for as long as the
            // duplicate only carried placement; reading six more fields,
            // including a vector of contexts, is how that luck ends.)
            const openshow::domain::Theme source = *original;
            const openshow::domain::Id copy = fDocuments.createTheme(
                source.name + " copy", source.textColor, source.backgroundColor,
                source.backgroundGradient, source.fontFamily, source.fontSize, source.align,
                source.lineSpacing, &error);
            if (copy.empty()) {
                return false;
            }
            // A duplicate that lost the placement (amendment 2026-11-01)
            // would restyle a show and quietly move nothing — "a starting
            // point to edit" means the whole look, where the words live
            // included. Nothing survives a refused half of one entry.
            if (source.placement.has_value() &&
                !fDocuments.setThemePlacement(copy, *source.placement, &error)) {
                fDocuments.removeTheme(copy, nullptr);
                return false;
            }
            // The contexts travel too (task TS.1 closes the older gap the
            // duplication had left): the plan's inheritance rule says
            // unknown context names survive theme duplication, and a copy
            // that reads as a song under a Bible context is a copy that
            // lied about the look.
            if (!source.contexts.empty() &&
                !fDocuments.setThemeContexts(copy, source.contexts, &error)) {
                fDocuments.removeTheme(copy, nullptr);
                return false;
            }
            // And the TS.1 style block: weight, slant, fit policy, the
            // minimum size and both effect records, whole — including a
            // disabled record, because THAT is the source theme's answer
            // ("no outline, whatever the chain says") and a duplicate that
            // dropped it would start drawing one.
            openshow::domain::ThemeTextStyle style;
            style.fontWeight = source.fontWeight;
            style.fontStyle = source.fontStyle;
            style.fitMode = source.fitMode;
            style.minFontSize = source.minFontSize;
            style.stroke = source.stroke;
            style.shadow = source.shadow;
            if (!fDocuments.setThemeTextStyle(copy, style, &error)) {
                fDocuments.removeTheme(copy, nullptr);
                return false;
            }
            return true;
        })) {
        setStatus(QStringLiteral("duplicate theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::deleteTheme(const QString& themeId) {
    flushText();
    std::string error;
    if (!edit([&] {
            return fDocuments.removeTheme(themeId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("delete theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

namespace {

// The Theme Editor's sample fixture (task TV.1): a COPY of the live
// documents with one throwaway show added, the current theme cleared, and
// the SELECTED theme set as that show's default. Copying the real records
// — rather than hand-cloning the fields the editor happens to show — is
// what makes the sample honest: the edited Default theme is the chain's
// last layer exactly as in the audience's picture, every theme's contexts
// travel whole, and clearing the current theme keeps a theme the operator
// is BROWSING from being out-resolved by a theme the service is WEARING.
// The slide names the selected view's text context ("" for Songs is no
// name at all), so the first theme in chain order carrying it supplies its
// typography and box exactly as a passage's slide resolves them. The copy
// dies with the call: no history, no dirty flag, nothing presented.
bool compileThemeSample(const openshow::domain::Documents& documents,
                        const openshow::domain::Id& themeId, const std::string& view,
                        openshow::scene::Scene* scene,
                        openshow::domain::ResolvedAppearance* look = nullptr) {
    const openshow::domain::Theme* source = documents.findTheme(themeId);
    if (source == nullptr) {
        return false;
    }
    openshow::domain::Documents fixture = documents;
    std::string error;
    if (!fixture.setCurrentTheme({}, &error)) {
        return false;
    }
    const openshow::domain::Id show = fixture.createShow("Theme sample");
    const openshow::domain::Id slide = fixture.appendSlide(show);
    if (show.empty() || slide.empty()) {
        return false;
    }
    if (!fixture.setShowDefaultTheme(show, themeId, &error) ||
        (!view.empty() && !fixture.setSlideTextContext(show, slide, view, &error))) {
        return false;
    }
    // Songs sample: one word to read the colour and face in, one scale line
    // to read the tracking and the wrap, one blank to read the line box.
    // Bible sample (task TV.1): the reading shape the context is FOR — a
    // reference line, running text long enough to wrap at a lectern size,
    // numerals and Unicode, and a licence-style tail — so the operator sees
    // the passage's measure, not the song's pangram, under the Bible box.
    std::vector<std::string> lines;
    if (view.empty()) {
        lines = {source->name,
                 "The quick brown fox jumps over the lazy dog 0123456789",
                 ""};
    } else {
        lines = {source->name + " — 1 Corinthians 1:1-3",
                 "Paul, a prisoner of Christ Jesus, called to be an apostle — sent not from "
                 "any human authority, but through Jesus Christ and God the Father who raised "
                 "him from the dead. 0123456789",
                 "Grace to you and peace from God our Father and the Lord Jesus Christ. "
                 "χάρις ὑμῖν καὶ εἰρήνη",
                 "© OpenShow sample text — placeholder attribution",
                 ""};
    }
    if (!fixture.setSlideLines(show, slide, lines)) {
        return false;
    }
    if (look != nullptr) {
        *look = fixture.appearanceForSlide(show, slide);
    }
    return openshow::scene::compileSlide(fixture, show, slide, scene);
}

}  // namespace

QString DocumentController::themeSampleSceneJson(const QString& themeId,
                                                 const QString& contextName) const {
    openshow::scene::Scene scene;
    if (!compileThemeSample(fDocuments, themeId.toStdString(), contextName.toStdString(),
                            &scene)) {
        return QString();
    }
    return QString::fromStdString(boost::json::serialize(openshow::scene::toJson(scene)));
}

QVariantMap DocumentController::themeSampleAppearance(const QString& themeId,
                                                      const QString& contextName) const {
    QVariantMap out;
    out.insert(QStringLiteral("ok"), false);
    openshow::scene::Scene scene;
    openshow::domain::ResolvedAppearance look;
    if (!compileThemeSample(fDocuments, themeId.toStdString(), contextName.toStdString(),
                            &scene, &look)) {
        return out;
    }
    // The primary lyric box is the FIRST TEXT item, exactly as the compile
    // and the Edit canvas place it — the picture's own rectangle, so the
    // overlay's handles sit where the audience's words will.
    for (const openshow::scene::SceneItem& item : scene.items) {
        if (item.isImage) {
            continue;
        }
        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("boxX"), item.x);
        out.insert(QStringLiteral("boxY"), item.y);
        out.insert(QStringLiteral("boxWidth"), item.width);
        out.insert(QStringLiteral("boxHeight"), item.height);
        out.insert(QStringLiteral("fontSize"), item.fontSize);
        out.insert(QStringLiteral("lineSpacing"), item.lineSpacing);
        out.insert(QStringLiteral("align"), QString::fromStdString(item.align));
        out.insert(QStringLiteral("fontFamily"), QString::fromStdString(item.fontFamily));
        out.insert(QStringLiteral("context"), QString::fromStdString(look.contextName));
        return out;
    }
    return out;  // no text box at all: the page keeps the box it was shown
}

// --- the Bible importer's off-air compile (task BI.5) -----------------------

namespace {

// A COPY of the live service with one throwaway slide added. A scripture
// slide joins the schedule owning no theme and customising nothing, so its
// appearance is the chain as the service stands — worn theme, Default
// layer, built-in box — and a copy of the real documents is what walks
// that chain identically. It also names the `scripture` text context
// (task TF.3), exactly as the added slides do: when the service's themes
// carry a reading-size scripture context, the measurement style and the
// preview here resolve through it, and the box the engine packs into is
// the box the audience will draw into; when no theme carries the name,
// it resolves as no layer at all and everything is as it stood before TF.
// createShow stands in for createScriptureShow because the scripture
// fields (ownership, provenance) speak nothing the compile reads. The
// copy dies with the call: no history, no dirty flag, no output.
bool compileScriptureFixture(const openshow::domain::Documents& documents,
                             const std::vector<std::string>& lines,
                             openshow::scene::Scene* scene, std::string* contextName = nullptr) {
    openshow::domain::Documents fixture = documents;
    const openshow::domain::Id show = fixture.createShow("Scripture preview");
    const openshow::domain::Id slide = fixture.appendSlide(show);
    if (show.empty() || slide.empty() || !fixture.setSlideLines(show, slide, lines) ||
        !fixture.setSlideTextContext(show, slide, "scripture")) {
        return false;
    }
    if (contextName != nullptr) {
        // Which theme carried the name is the resolver's answer, and the
        // page prints exactly that word (task TF.4).
        *contextName = fixture.appearanceForSlide(show, slide).contextName;
    }
    return openshow::scene::compileSlide(fixture, show, slide, scene);
}

}  // namespace

QVariantMap DocumentController::scriptureBoxStyle() const {
    // One word is enough: the primary box's rectangle and typography are
    // the chain's answer regardless of the words, and this compiles the
    // same first text item a generated slide's words would fill.
    openshow::scene::Scene scene;
    std::string contextName;
    QVariantMap style;
    if (!compileScriptureFixture(fDocuments, {"a"}, &scene, &contextName)) {
        return style;
    }
    style.insert(QStringLiteral("context"), QString::fromStdString(contextName));
    for (const openshow::scene::SceneItem& item : scene.items) {
        if (item.isImage) {
            continue;  // the primary lyric box is the first TEXT item
        }
        style.insert(QStringLiteral("boxWidth"), item.width);
        style.insert(QStringLiteral("boxHeight"), item.height);
        style.insert(QStringLiteral("fontSize"), item.fontSize);
        style.insert(QStringLiteral("lineSpacing"), item.lineSpacing);
        style.insert(QStringLiteral("fontFamily"), QString::fromStdString(item.fontFamily));
        // Task TS.3: the style block off the SAME item — the packer that
        // measures with these numbers and the slide that draws these
        // words then cannot drift, because both read one SceneItem. The
        // effect records stay in the domain's terms ("#rrggbb"); the wire
        // packing is the controller's seam.
        style.insert(QStringLiteral("fontWeight"), item.fontWeight);
        style.insert(QStringLiteral("fontStyle"), QString::fromStdString(item.fontStyle));
        style.insert(QStringLiteral("fitMode"), QString::fromStdString(item.fitMode));
        style.insert(QStringLiteral("minFontSize"), item.minFontSize);
        QVariantMap stroke;
        stroke.insert(QStringLiteral("enabled"), item.stroke.enabled);
        stroke.insert(QStringLiteral("color"), QString::fromStdString(item.stroke.color));
        stroke.insert(QStringLiteral("width"), item.stroke.width);
        style.insert(QStringLiteral("stroke"), stroke);
        QVariantMap shadow;
        shadow.insert(QStringLiteral("enabled"), item.shadow.enabled);
        shadow.insert(QStringLiteral("color"), QString::fromStdString(item.shadow.color));
        shadow.insert(QStringLiteral("opacity"), item.shadow.opacity);
        shadow.insert(QStringLiteral("offsetX"), item.shadow.offsetX);
        shadow.insert(QStringLiteral("offsetY"), item.shadow.offsetY);
        shadow.insert(QStringLiteral("blurSigma"), item.shadow.blurSigma);
        style.insert(QStringLiteral("shadow"), shadow);
        return style;
    }
    return style;  // no text box at all: the importer refuses with its own sentence
}

QString DocumentController::scriptureSlideSceneJson(const QStringList& lines) const {
    std::vector<std::string> authored;
    authored.reserve(lines.size());
    for (const QString& line : lines) {
        authored.push_back(line.toStdString());
    }
    openshow::scene::Scene scene;
    if (!compileScriptureFixture(fDocuments, authored, &scene)) {
        return QString();
    }
    return QString::fromStdString(boost::json::serialize(openshow::scene::toJson(scene)));
}

// --- theme surfaces (task TH.5, themes plan) --------------------------------

namespace {

// The show that owns a slide. Cue tiles browse the whole library, so a
// theme asked for on a tile belongs to whoever holds that slide — never
// assumed from the editing cursor.
openshow::domain::Id ownerShowOf(const openshow::domain::Documents& documents,
                                 const openshow::domain::Id& slideId) {
    for (const openshow::domain::Id& showId : documents.showIds()) {
        const openshow::domain::Show* show = documents.findShow(showId);
        if (show == nullptr) {
            continue;
        }
        for (const openshow::domain::Slide& slide : show->slides) {
            if (slide.id == slideId) {
                return showId;
            }
        }
    }
    return {};
}

} // namespace

QVariantMap DocumentController::slideAppearance() const {
    QVariantMap map;
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        return map;
    }
    // Values through the chain — customization, worn theme, the slide's
    // own, the show's — and the flags straight from the slide's sparse
    // set, which is what says WHICH property the operator touched.
    const openshow::domain::ResolvedAppearance look =
        fDocuments.appearanceForSlide(fCurrentSongId, slideId);
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    const openshow::domain::SlideCustomization* touched = nullptr;
    if (show != nullptr) {
        for (const openshow::domain::Slide& slide : show->slides) {
            if (slide.id == slideId) {
                touched = &slide.customizations;
                break;
            }
        }
    }
    const auto field = [&map](const char* key, const QVariant& value, bool customized) {
        QVariantMap entry;
        entry.insert(QStringLiteral("value"), value);
        entry.insert(QStringLiteral("customized"), customized);
        map.insert(QLatin1String(key), entry);
    };
    const auto has = [touched](const char* key) {
        if (touched == nullptr) {
            return false;
        }
        const std::string name(key);
        if (name == "textColor") return touched->textColor.has_value();
        if (name == "backgroundColor") return touched->backgroundColor.has_value();
        if (name == "backgroundGradient") return touched->backgroundGradient.has_value();
        if (name == "fontFamily") return touched->fontFamily.has_value();
        if (name == "fontSize") return touched->fontSize.has_value();
        if (name == "align") return touched->align.has_value();
        if (name == "lineSpacing") return touched->lineSpacing.has_value();
        if (name == "placement") return touched->placement.has_value();
        // The TS.1 style fields (task TS.1): the slide's sparse set answers
        // by name, and "stroke"/"shadow" name one WHOLE record each — the
        // badge reads "customized" exactly when the slide spoke.
        return touched->hasField(name);
    };
    field("textColor", QString::fromStdString(look.textColor), has("textColor"));
    field("backgroundColor", QString::fromStdString(look.backgroundColor),
          has("backgroundColor"));
    field("backgroundGradient", QString::fromStdString(look.backgroundGradient),
          has("backgroundGradient"));
    field("fontFamily", QString::fromStdString(look.fontFamily), has("fontFamily"));
    field("fontSize", look.fontSize, has("fontSize"));
    field("align", QString::fromStdString(look.align), has("align"));
    field("lineSpacing", look.lineSpacing, has("lineSpacing"));
    // Placement (amendment 2026-11-01): the rectangle the chain resolves for
    // the slide's PRIMARY lyric box, empty when no layer places it, with the
    // flag saying whether the SLIDE wrote its own. The Edit page's geometry
    // fields and their badge read this row like every other.
    QVariantMap placed;
    if (look.placement.has_value()) {
        placed.insert(QStringLiteral("x"), look.placement->x);
        placed.insert(QStringLiteral("y"), look.placement->y);
        placed.insert(QStringLiteral("width"), look.placement->width);
        placed.insert(QStringLiteral("height"), look.placement->height);
    }
    field("placement", placed, has("placement"));
    // The TS.1 style fields: the chain's CONCRETE answers (the resolved
    // record is never silent — the defaults are the contract's own), with
    // the customized flag from the slide's sparse set. The effects travel
    // as whole maps, `enabled` first-class, because "disabled here over an
    // inherited effect" is a value the inspector must be able to show.
    field("fontWeight", look.fontWeight, has("fontWeight"));
    field("fontStyle", QString::fromStdString(look.fontStyle), has("fontStyle"));
    field("fitMode", QString::fromStdString(look.fitMode), has("fitMode"));
    field("minFontSize", look.minFontSize, has("minFontSize"));
    QVariantMap strokeRow;
    strokeRow.insert(QStringLiteral("enabled"), look.stroke.enabled);
    strokeRow.insert(QStringLiteral("color"), QString::fromStdString(look.stroke.color));
    strokeRow.insert(QStringLiteral("width"), look.stroke.width);
    field("stroke", strokeRow, has("stroke"));
    QVariantMap shadowRow;
    shadowRow.insert(QStringLiteral("enabled"), look.shadow.enabled);
    shadowRow.insert(QStringLiteral("color"), QString::fromStdString(look.shadow.color));
    shadowRow.insert(QStringLiteral("opacity"), look.shadow.opacity);
    shadowRow.insert(QStringLiteral("offsetX"), look.shadow.offsetX);
    shadowRow.insert(QStringLiteral("offsetY"), look.shadow.offsetY);
    shadowRow.insert(QStringLiteral("blurSigma"), look.shadow.blurSigma);
    field("shadow", shadowRow, has("shadow"));
    return map;
}

bool DocumentController::customizeCurrentSlide(const QString& field, const QVariant& value) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    const openshow::domain::SlideCustomization* previous = nullptr;
    if (slideId.empty() || show == nullptr) {
        setStatus(QStringLiteral("customize: no current slide"));
        return false;
    }
    for (const openshow::domain::Slide& slide : show->slides) {
        if (slide.id == slideId) {
            previous = &slide.customizations;
            break;
        }
    }
    openshow::domain::SlideCustomization changed = *previous;
    const std::string name = field.toStdString();
    if (name == "fontSize" || name == "lineSpacing") {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (!ok) {
            setStatus(QStringLiteral("customize: %1 must be a number")
                          .arg(field));
            return false;
        }
        (name == "fontSize" ? changed.fontSize : changed.lineSpacing) = number;
    } else if (name == "textColor" || name == "backgroundColor" ||
               name == "backgroundGradient" || name == "fontFamily" || name == "align") {
        (name == "textColor"          ? changed.textColor
         : name == "backgroundColor"  ? changed.backgroundColor
         : name == "backgroundGradient" ? changed.backgroundGradient
         : name == "fontFamily"       ? changed.fontFamily
                                      : changed.align)
            = value.toString().toStdString();
    } else if (name == "fontWeight" || name == "fontStyle" || name == "fitMode" ||
               name == "minFontSize" || name == "stroke" || name == "shadow") {
        // The TS.1 fields (task TS.1): ONE field's edit passes the theme
        // verb's own shape check by way of a one-field map, so a stringy
        // weight, an off-list word or half an effect record is refused
        // naming itself here exactly as setThemeTextStyle refuses it — and
        // the bands land in setSlideCustomizations' domain vetting, the
        // same sentence one call away. A customization is as strict as a
        // theme because it has to be able to become one.
        QVariantMap single;
        single.insert(field, value);
        openshow::domain::ThemeTextStyle styled;
        std::string shapeError;
        if (!styleFromMap(single, &styled, &shapeError)) {
            setStatus(QStringLiteral("customize: %1").arg(QString::fromStdString(shapeError)));
            return false;
        }
        if (styled.fontWeight.has_value()) {
            changed.fontWeight = styled.fontWeight;
        }
        if (styled.fontStyle.has_value()) {
            changed.fontStyle = styled.fontStyle;
        }
        if (styled.fitMode.has_value()) {
            changed.fitMode = styled.fitMode;
        }
        if (styled.minFontSize.has_value()) {
            changed.minFontSize = styled.minFontSize;
        }
        if (styled.stroke.has_value()) {
            changed.stroke = styled.stroke;
        }
        if (styled.shadow.has_value()) {
            changed.shadow = styled.shadow;
        }
    } else {
        setStatus(QStringLiteral("customize: %1 is not an appearance field").arg(field));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setSlideCustomizations(fCurrentSongId, slideId, changed, &error);
        })) {
        setStatus(QStringLiteral("customize: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::revertCurrentSlideField(const QString& field) {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    const openshow::domain::Show* show = fDocuments.findShow(fCurrentSongId);
    const openshow::domain::SlideCustomization* previous = nullptr;
    if (slideId.empty() || show == nullptr) {
        setStatus(QStringLiteral("revert: no current slide"));
        return false;
    }
    for (const openshow::domain::Slide& slide : show->slides) {
        if (slide.id == slideId) {
            previous = &slide.customizations;
            break;
        }
    }
    openshow::domain::SlideCustomization changed = *previous;
    const std::string name = field.toStdString();
    bool wasCustomized = false;
    if (name == "fontSize") {
        wasCustomized = changed.fontSize.has_value();
        changed.fontSize.reset();
    } else if (name == "lineSpacing") {
        wasCustomized = changed.lineSpacing.has_value();
        changed.lineSpacing.reset();
    } else if (name == "textColor" || name == "backgroundColor" ||
               name == "backgroundGradient" || name == "fontFamily" || name == "align") {
        std::optional<std::string>& slot =
            name == "textColor"            ? changed.textColor
            : name == "backgroundColor"    ? changed.backgroundColor
            : name == "backgroundGradient" ? changed.backgroundGradient
            : name == "fontFamily"         ? changed.fontFamily
                                           : changed.align;
        wasCustomized = slot.has_value();
        slot.reset();
    } else if (name == "placement") {
        // One unit (amendment 2026-11-01): the whole rectangle goes back to
        // the themes at once — there is no such thing as half a placed box.
        wasCustomized = changed.placement.has_value();
        changed.placement.reset();
    } else if (openshow::domain::isCustomizableField(name)) {
        // The TS.1 style fields revert through the record's own name list
        // (task TS.1): hasField/clearField speak every customizable name,
        // and "stroke"/"shadow" carry the whole record back to the themes —
        // half an effect is not a thing, so half a revert never is either.
        wasCustomized = changed.hasField(name);
        changed.clearField(name);
    } else {
        setStatus(QStringLiteral("revert: %1 is not an appearance field").arg(field));
        return false;
    }
    if (!wasCustomized) {
        setStatus(QStringLiteral("revert: this slide has no %1 customization to revert")
                      .arg(field));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setSlideCustomizations(fCurrentSongId, slideId, changed, &error);
        })) {
        setStatus(QStringLiteral("revert: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::removeCurrentSlideCustomizations() {
    flushText();
    const openshow::domain::Id slideId = currentSlideId();
    if (slideId.empty()) {
        setStatus(QStringLiteral("restore: no current slide"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.removeSlideCustomizations(fCurrentSongId, slideId, &error);
        })) {
        setStatus(QStringLiteral("restore: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::applyThemeToSlide(const QString& slideId, const QString& themeId) {
    flushText();
    const openshow::domain::Id owner = ownerShowOf(fDocuments, slideId.toStdString());
    if (owner.empty()) {
        setStatus(QStringLiteral("theme: no slide '%1' to theme").arg(slideId));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.applyThemeToSlides(owner, {slideId.toStdString()},
                                                 themeId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::applyThemeToShow(const QString& showId, const QString& themeId) {
    flushText();
    const openshow::domain::Show* show = fDocuments.findShow(showId.toStdString());
    if (show == nullptr) {
        setStatus(QStringLiteral("theme: no show '%1'").arg(showId));
        return false;
    }
    std::vector<openshow::domain::Id> slideIds;
    slideIds.reserve(show->slides.size());
    for (const openshow::domain::Slide& slide : show->slides) {
        slideIds.push_back(slide.id);
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.applyThemeToSlides(showId.toStdString(), slideIds,
                                                 themeId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("theme: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::removeSlideCustomizationsFor(const QString& slideId) {
    flushText();
    const openshow::domain::Id owner = ownerShowOf(fDocuments, slideId.toStdString());
    if (owner.empty()) {
        setStatus(QStringLiteral("restore: no slide '%1'").arg(slideId));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.removeSlideCustomizations(owner, slideId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("restore: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::removeShowCustomizationsFor(const QString& showId) {
    flushText();
    std::string error;
    if (!edit([&] {
            return fDocuments.removeShowCustomizations(showId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("restore: %1").arg(QString::fromStdString(error)));
        return false;
    }
    return true;
}

bool DocumentController::setShowDefaultThemeFor(const QString& showId,
                                                const QString& themeId) {
    flushText();
    if (showId.isEmpty()) {
        setStatus(QStringLiteral("theme: no show selected"));
        return false;
    }
    std::string error;
    if (!edit([&] {
            return fDocuments.setShowDefaultTheme(showId.toStdString(),
                                                  themeId.toStdString(), &error);
        })) {
        setStatus(QStringLiteral("theme: %1").arg(QString::fromStdString(error)));
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
    return fViews.backgroundFit(viewCursor());
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
        const openshow::domain::Slide* slide = fViews.findSlide(fCurrentSongId, slideId);
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
        fClip.themeId = slide.themeId;
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
    // A theme or asset deleted since the capture falls away rather than
    // refusing the whole paste.
    std::string themeId = clip.themeId;
    if (!themeId.empty() && fDocuments.findTheme(themeId) == nullptr) {
        themeId.clear();
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
            // the capture falls away with it, like the theme and the
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
                ok = fDocuments.setSlideTheme(song, inserted, themeId, &error);
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
                clip.themeId = slide.themeId;
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
    return fViews.playlists();
}

QVariantList DocumentController::collectionOutline() const {
    return fViews.collectionOutline();
}

QVariantList DocumentController::runningOrderCues() const {
    return fViews.runningOrderCues(fCurrentPlaylistId);
}

QVariantList DocumentController::slidesOf(const QString& showId) const {
    return fViews.slides(showId.toStdString());
}

QVariantList DocumentController::playlistEntries() const {
    return fViews.playlistEntries(fCurrentPlaylistId);
}

QVariantList DocumentController::assets() const {
    return fViews.assets();
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

bool DocumentController::addScripturePassage(const QString& playlistId, const QString& reference,
                                             const QString& translation,
                                             const QString& attribution,
                                             const QVariantList& slides) {
    flushText();
    // The plan's explicit target: the importer never picks a schedule for
    // the operator, and an Add whose target vanished since the preview is
    // invalidated, not redirected.
    const openshow::domain::Id target = playlistId.toStdString();
    if (target.empty() || fDocuments.findPlaylist(target) == nullptr) {
        setStatus(QStringLiteral("add passage: today's schedule does not exist — select or "
                                 "create it first"));
        return false;
    }
    if (reference.trimmed().isEmpty() || translation.trimmed().isEmpty()) {
        setStatus(QStringLiteral("add passage: a reference and a translation are required"));
        return false;
    }
    if (slides.isEmpty()) {
        setStatus(QStringLiteral("add passage: no slides were generated to add"));
        return false;
    }
    // Shapes are checked before the edit opens: a row that is not a list
    // of strings (with optional notes) is a caller bug, and refusing here
    // means the document never sees a half-built slide.
    struct SlideSpec {
        std::vector<std::string> lines;
        std::string notes;  // the verse label, in NOTES — never on the slide
    };
    std::vector<SlideSpec> boxes;
    boxes.reserve(slides.size());
    for (const QVariant& row : slides) {
        SlideSpec spec;
        QVariantList lines;
        if (row.typeId() == QMetaType::QVariantMap) {
            const QVariantMap map = row.toMap();
            lines = map.value(QStringLiteral("lines")).toList();
            spec.notes = map.value(QStringLiteral("notes")).toString().toStdString();
        } else if (row.canConvert<QVariantList>()) {
            lines = row.toList();
        } else {
            setStatus(QStringLiteral("add passage: a slide's content is not a list of lines"));
            return false;
        }
        spec.lines.reserve(lines.size());
        for (const QVariant& line : lines) {
            spec.lines.push_back(line.toString().toStdString());
        }
        boxes.push_back(std::move(spec));
    }
    openshow::domain::Id created;
    std::string error;
    if (!edit([&] {
            created = fDocuments.createScriptureShow(
                reference.trimmed().toStdString(),
                openshow::domain::ScriptureProvenance{reference.trimmed().toStdString(),
                                                      translation.trimmed().toStdString(),
                                                      attribution.toStdString()},
                &error);
            if (created.empty()) {
                return false;
            }
            // History will not roll a refused mutation back for us (the
            // paste rule), so any later step that fails takes the whole
            // young show with it — the document never keeps half a passage.
            // Every slide names the `scripture` text context (task TF.3):
            // the reading typography and box the pagination was measured
            // against are the context's, and playback must resolve them
            // through the same chain the preview walked. A service whose
            // themes carry no such context resolves the name as no layer,
            // exactly as it drew before contexts existed.
            for (const SlideSpec& spec : boxes) {
                const openshow::domain::Id slide = fDocuments.appendSlide(created, &error);
                if (slide.empty() ||
                    !fDocuments.setSlideLines(created, slide, spec.lines, &error) ||
                    (!spec.notes.empty() &&
                     !fDocuments.setSlideNotes(created, slide, spec.notes, &error)) ||
                    !fDocuments.setSlideTextContext(created, slide, "scripture", &error)) {
                    fDocuments.removeShow(created, nullptr);
                    created.clear();
                    return false;
                }
            }
            if (fDocuments.addPlaylistEntry(target, created, std::string(), &error).empty()) {
                fDocuments.removeShow(created, nullptr);
                created.clear();
                return false;
            }
            return true;
        })) {
        setStatus(QStringLiteral("add passage: %1").arg(QString::fromStdString(error)));
        return false;
    }
    // The schedule gained a row; if that schedule is on screen, it must say so.
    if (fCurrentPlaylistId == target) {
        emit navigationChanged();
    }
    return true;
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
    return fViews.searchShows(query);
}

bool DocumentController::importMediaUrl(const QUrl& file) {
    if (!file.isLocalFile() || file.toLocalFile().isEmpty()) {
        setStatus(QStringLiteral("Choose a media file on this computer"));
        return false;
    }
    return importMediaFile(file.toLocalFile());
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

QStringList DocumentController::mediaImportNameFilters() const {
    // One table (openshow::domain) spells the kinds, so this filter, the
    // bin's labels and the pipeline's decision cannot drift apart: a dialog
    // that listed "*.mp4" by hand would be a fourth place to change when the
    // media pipeline learns a container, and the fourth is the one that lies.
    auto glob = [](const char* const* names) {
        QStringList patterns;
        for (; *names != nullptr; ++names) {
            patterns << QStringLiteral("*.") + QString::fromLatin1(*names);
        }
        return patterns.join(QLatin1Char(' '));
    };
    return {QStringLiteral("Media files (%1 %2 %3)")
                .arg(glob(openshow::domain::kPictureExtensions),
                     glob(openshow::domain::kVideoExtensions),
                     glob(openshow::domain::kAudioExtensions)),
            QStringLiteral("All files (*)")};
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
    // A slide compiles in its OWNING show's context (UI.3a). Thumbnails and
    // cues read slides of any show in the library, so compilation may not
    // depend on the editing cursor; slide ids are unique across the
    // document, so the owner is simply whoever contains the id. The current
    // show answers first because it answers most calls.
    openshow::scene::Scene scene;
    std::string error;
    if (openshow::scene::compileSlide(fDocuments, fCurrentSongId, slideId, &scene, &error)) {
        const boost::json::value json = openshow::scene::toJson(scene);
        return QString::fromStdString(boost::json::serialize(json));
    }
    for (const openshow::domain::Id& showId : fDocuments.showIds()) {
        if (openshow::scene::compileSlide(fDocuments, showId, slideId, &scene, &error)) {
            const boost::json::value json = openshow::scene::toJson(scene);
            return QString::fromStdString(boost::json::serialize(json));
        }
    }
    return QString();
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
    // The durability half is one call (task 6.19b1): quiesce, write, remove the
    // scratch this supersedes, reopen, and say what happened. What a save does to
    // the model this class owns is the part that stays here.
    DocumentPersistence::SaveAttempt attempt = fPersistence->saveToService(&fDocuments);
    if (!attempt.saved) {
        setStatus(attempt.status);
        return false;
    }
    // The model now names the package's own copies. The history snapshots
    // have to hear it too (task 6.12, review R7): the next lines deleted the
    // autosave tree, which may be exactly where a pre-save snapshot points,
    // and an undo must not hand the model back a path to a directory that
    // no longer exists — that undo looks fine until the following present,
    // save or autosave tick tries to read the file. (The removal ran inside
    // the adapter, before this rewrite, with nothing in between able to read
    // a snapshot: the state that matters is the one save() returns with.)
    fHistory->adoptAssets(attempt.rebinds);
    fDirty = false;
    setStatus(attempt.status);
    if (!attempt.rebinds.empty()) {
        // Downstream caches of compiled scenes (the worker controller's
        // payload mirror) must stop naming the paths these files came from.
        QVariantMap adopted;
        for (const openshow::domain::AssetRebind& rebind : attempt.rebinds) {
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
    // One call for the durability and the decision it carries (task 6.19b1):
    // this load DISCARDS local work, so the executor is quiesced and the trail
    // the discarded edits left is removed, with any survivor reported in the
    // same words. The documents arrive separately, and only now does the
    // session give up what it had.
    DocumentPersistence::LoadAttempt attempt = fPersistence->loadFromService();
    if (!attempt.loaded) {
        setStatus(attempt.status);
        return false;
    }
    fDocuments = std::move(attempt.documents);
    fHistory->clear();
    fDirty = false;
    const std::vector<openshow::domain::Id> ids = fDocuments.showIds();
    fCurrentSongId = ids.empty() ? openshow::domain::Id{} : ids.front();
    fCurrentSlide = 0;
    seedDefaultPlaylist();
    setStatus(attempt.status);
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
