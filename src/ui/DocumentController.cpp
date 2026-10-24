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
    fAutosave = std::make_unique<openshow::domain::Autosave>(baseDirectory / "autosave");
    fHistory = std::make_unique<openshow::domain::History>(&fDocuments);

    // Task 2.3's startup rule: autosave data, when present, is newer than
    // the saved service and wins; a missing service simply begins empty.
    std::string error;
    if (openshow::domain::Autosave::exists(fAutosave->directory())) {
        if (fAutosave->recover(&fDocuments, &error)) {
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
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(slide.id));
        entry.insert(QStringLiteral("index"), static_cast<int>(i));
        entry.insert(QStringLiteral("preview"), QString::fromStdString(
            slide.lines.empty() ? std::string("(empty)") : slide.lines.front()));
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
    return linesToText(show->slides[static_cast<std::size_t>(fCurrentSlide)].lines);
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
    std::string tickError;
    fAutosave->tick(fDocuments, &tickError);
    emit documentsChanged();
    emit stateChanged();
    return true;
}

void DocumentController::setStatus(const QString& text) {
    fStatus = text;
    emit stateChanged();
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
            for (const std::string& line : slide.lines) {
                lines.append(QString::fromStdString(line));
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
    std::string error;
    if (!openshow::domain::saveService(fDocuments, fServiceDirectory, &error)) {
        setStatus(QStringLiteral("save failed: %1").arg(QString::fromStdString(error)));
        return false;
    }
    // The service now holds everything the autosave was protecting. No
    // re-tick after clear: clear() resets the fingerprint, so a tick would
    // immediately write a package claiming unsaved work that does not
    // exist. The next real edit re-creates the autosave.
    std::string clearError;
    fAutosave->clear(&clearError);
    fDirty = false;
    setStatus(QStringLiteral("saved %1")
                  .arg(QString::fromStdWString(fServiceDirectory.wstring())));
    emit stateChanged();
    return true;
}

bool DocumentController::load() {
    flushText();
    openshow::domain::Documents reloaded;
    std::string error;
    if (!openshow::domain::loadService(&reloaded, fServiceDirectory, &error)) {
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
    fAutosave->clear(&clearError);
    setStatus(QStringLiteral("loaded %1")
                  .arg(QString::fromStdWString(fServiceDirectory.wstring())));
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
    std::string tickError;
    fAutosave->tick(fDocuments, &tickError);
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
    std::string tickError;
    fAutosave->tick(fDocuments, &tickError);
    emit documentsChanged();
    emit navigationChanged();
    emit slideTextChanged();
    emit stateChanged();
}

}  // namespace openshow::ui
