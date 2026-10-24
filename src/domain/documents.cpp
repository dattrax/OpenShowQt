/*
 * Document operations (task 2.1) — see documents.h for the contract.
 * Every mutating entry point validates first and mutates only on
 * success, so a rejected call leaves the model untouched; that is what
 * undo (2.3) will rely on.
 */

#include "openshow/domain/documents.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace openshow::domain {

namespace {

bool blank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

// Names are stored trimmed: every consumer compares or displays them,
// and a trailing space in "Chorus " would silently fork a group.
std::string trimmed(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

void fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string idError(const char* what, const Id& id) {
    return std::string(what) + " '" + id + "' does not exist";
}

}  // namespace

// --- shows ----------------------------------------------------------------

Id Documents::createShow(const std::string& name, std::string* error) {
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "a show needs a name");
        return {};
    }
    Show show;
    show.id = nextId("show");
    show.name = clean;
    fShows.push_back(std::move(show));
    return fShows.back().id;
}

bool Documents::removeShow(const Id& showId, std::string* error) {
    const auto found = std::find_if(fShows.begin(), fShows.end(),
                                    [&](const Show& s) { return s.id == showId; });
    if (found == fShows.end()) {
        fail(error, idError("show", showId));
        return false;
    }
    fShows.erase(found);
    // Entries naming the show go with it: a playlist that could never
    // run a slide is worse than one with a hole.
    for (Playlist& playlist : fPlaylists) {
        std::erase_if(playlist.entries,
                      [&](const PlaylistEntry& e) { return e.showId == showId; });
    }
    return true;
}

bool Documents::renameShow(const Id& showId, const std::string& name, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "a show needs a name");
        return false;
    }
    show->name = clean;
    return true;
}

bool Documents::setShowNotes(const Id& showId, const std::string& notes, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    show->notes = notes;
    return true;
}

const Show* Documents::findShow(const Id& showId) const {
    const auto found = std::find_if(fShows.begin(), fShows.end(),
                                    [&](const Show& s) { return s.id == showId; });
    return found == fShows.end() ? nullptr : &*found;
}

std::vector<Id> Documents::showIds() const {
    std::vector<Id> ids;
    ids.reserve(fShows.size());
    for (const Show& show : fShows) {
        ids.push_back(show.id);
    }
    return ids;
}

// --- slides ----------------------------------------------------------------

Id Documents::appendSlide(const Id& showId, std::string* error) {
    const Show* show = findShow(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return {};
    }
    return insertSlide(showId, show->slides.size(), error);
}

Id Documents::insertSlide(const Id& showId, size_t index, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return {};
    }
    if (index > show->slides.size()) {
        fail(error, "slide index " + std::to_string(index) + " is past the end (" +
                        std::to_string(show->slides.size()) + " slides)");
        return {};
    }
    Slide slide;
    slide.id = nextId("slide");
    const Id newId = slide.id;
    show->slides.insert(show->slides.begin() + static_cast<long>(index), std::move(slide));
    return newId;
}

bool Documents::removeSlide(const Id& showId, const Id& slideId, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    show->slides.erase(show->slides.begin() + index);
    return true;
}

bool Documents::moveSlide(const Id& showId, const Id& slideId, size_t toIndex,
                          std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int from = findSlideIndex(*show, slideId);
    if (from < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    // Positions name destinations in the final list, so the ceiling is
    // the unchanged size, and the element's own slot is a legal target.
    if (toIndex >= show->slides.size()) {
        fail(error, "slide index " + std::to_string(toIndex) + " is past the end (" +
                        std::to_string(show->slides.size()) + " slides)");
        return false;
    }
    const Slide moving = show->slides[static_cast<size_t>(from)];
    show->slides.erase(show->slides.begin() + from);
    show->slides.insert(show->slides.begin() + static_cast<long>(toIndex), moving);
    return true;
}

bool Documents::setSlideLines(const Id& showId, const Id& slideId,
                              const std::vector<std::string>& lines, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    show->slides[static_cast<size_t>(index)].lines = lines;
    return true;
}

bool Documents::setSlideNotes(const Id& showId, const Id& slideId, const std::string& notes,
                              std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    show->slides[static_cast<size_t>(index)].notes = notes;
    return true;
}

// --- groups ----------------------------------------------------------------

bool Documents::setSlideGroup(const Id& showId, const Id& slideId, const std::string& group,
                              std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    show->slides[static_cast<size_t>(index)].group = trimmed(group);
    return true;
}

bool Documents::renameGroup(const Id& showId, const std::string& from, const std::string& to,
                            std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const std::string target = trimmed(to);
    if (target.empty()) {
        fail(error, "a group needs a name; dissolving is dissolveGroup()");
        return false;
    }
    bool touched = false;
    for (Slide& slide : show->slides) {
        if (slide.group == from) {
            slide.group = target;
            touched = true;
        }
    }
    if (!touched) {
        fail(error, "no slide of show '" + showId + "' is in group '" + from + "'");
        return false;
    }
    return true;
}

bool Documents::dissolveGroup(const Id& showId, const std::string& group, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    bool touched = false;
    for (Slide& slide : show->slides) {
        if (slide.group == group) {
            slide.group.clear();
            touched = true;
        }
    }
    if (!touched) {
        fail(error, "no slide of show '" + showId + "' is in group '" + group + "'");
        return false;
    }
    return true;
}

std::vector<std::string> Documents::groups(const Id& showId) const {
    std::vector<std::string> labels;
    const Show* show = findShow(showId);
    if (show == nullptr) {
        return labels;
    }
    for (const Slide& slide : show->slides) {
        if (!slide.group.empty() &&
            std::find(labels.begin(), labels.end(), slide.group) == labels.end()) {
            labels.push_back(slide.group);
        }
    }
    return labels;
}

std::vector<Id> Documents::slidesInGroup(const Id& showId, const std::string& group) const {
    std::vector<Id> ids;
    const Show* show = findShow(showId);
    if (show == nullptr) {
        return ids;
    }
    for (const Slide& slide : show->slides) {
        if (slide.group == group) {
            ids.push_back(slide.id);
        }
    }
    return ids;
}

// --- styles ----------------------------------------------------------------

Id Documents::createStyle(const std::string& name, const std::string& textColor,
                          const std::string& backgroundColor, const std::string& fontFamily,
                          double fontSize, std::string* error) {
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "a style needs a name");
        return {};
    }
    if (!validColor(textColor, error) || !validColor(backgroundColor, nullptr)) {
        if (error && error->empty()) {
            fail(error, "background colour '" + backgroundColor + "' is not #rgb or #rrggbb");
        }
        return {};
    }
    if (!std::isfinite(fontSize) || fontSize < 0.0) {
        fail(error, "font size must be finite and >= 0 (0 means renderer default)");
        return {};
    }
    Style style;
    style.id = nextId("style");
    style.name = clean;
    style.textColor = textColor;
    style.backgroundColor = backgroundColor;
    style.fontFamily = fontFamily;
    style.fontSize = fontSize;
    fStyles.push_back(std::move(style));
    return fStyles.back().id;
}

bool Documents::removeStyle(const Id& styleId, std::string* error) {
    const auto found = std::find_if(fStyles.begin(), fStyles.end(),
                                    [&](const Style& s) { return s.id == styleId; });
    if (found == fStyles.end()) {
        fail(error, idError("style", styleId));
        return false;
    }
    fStyles.erase(found);
    // Dangling style references degrade to "no style" rather than
    // blocking the removal: clearing is the only consistent outcome.
    for (Show& show : fShows) {
        if (show.defaultStyleId == styleId) {
            show.defaultStyleId.clear();
        }
        for (Slide& slide : show.slides) {
            if (slide.styleId == styleId) {
                slide.styleId.clear();
            }
        }
    }
    return true;
}

bool Documents::renameStyle(const Id& styleId, const std::string& name, std::string* error) {
    Style* style = findStyle_mut(styleId);
    if (style == nullptr) {
        fail(error, idError("style", styleId));
        return false;
    }
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "a style needs a name");
        return false;
    }
    style->name = clean;
    return true;
}

bool Documents::updateStyle(const Id& styleId, const std::string& textColor,
                            const std::string& backgroundColor, const std::string& fontFamily,
                            double fontSize, std::string* error) {
    Style* style = findStyle_mut(styleId);
    if (style == nullptr) {
        fail(error, idError("style", styleId));
        return false;
    }
    if (!validColor(textColor, error) || !validColor(backgroundColor, nullptr)) {
        if (error && error->empty()) {
            fail(error, "background colour '" + backgroundColor + "' is not #rgb or #rrggbb");
        }
        return false;
    }
    if (!std::isfinite(fontSize) || fontSize < 0.0) {
        fail(error, "font size must be finite and >= 0 (0 means renderer default)");
        return false;
    }
    style->textColor = textColor;
    style->backgroundColor = backgroundColor;
    style->fontFamily = fontFamily;
    style->fontSize = fontSize;
    return true;
}

bool Documents::setShowDefaultStyle(const Id& showId, const Id& styleId, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    if (!styleId.empty() && findStyle(styleId) == nullptr) {
        fail(error, idError("style", styleId));
        return false;
    }
    show->defaultStyleId = styleId;
    return true;
}

bool Documents::setSlideStyle(const Id& showId, const Id& slideId, const Id& styleId,
                              std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    if (!styleId.empty() && findStyle(styleId) == nullptr) {
        fail(error, idError("style", styleId));
        return false;
    }
    show->slides[static_cast<size_t>(index)].styleId = styleId;
    return true;
}

const Style* Documents::findStyle(const Id& styleId) const {
    const auto found = std::find_if(fStyles.begin(), fStyles.end(),
                                    [&](const Style& s) { return s.id == styleId; });
    return found == fStyles.end() ? nullptr : &*found;
}

std::vector<Id> Documents::styleIds() const {
    std::vector<Id> ids;
    ids.reserve(fStyles.size());
    for (const Style& style : fStyles) {
        ids.push_back(style.id);
    }
    return ids;
}

const Style* Documents::styleForSlide(const Id& showId, const Id& slideId) const {
    const Show* show = findShow(showId);
    if (show == nullptr) {
        return nullptr;
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        return nullptr;
    }
    const Slide& slide = show->slides[static_cast<size_t>(index)];
    if (!slide.styleId.empty()) {
        return findStyle(slide.styleId);  // cleared references mean none
    }
    return show->defaultStyleId.empty() ? nullptr : findStyle(show->defaultStyleId);
}

// --- playlists ---------------------------------------------------------------

Id Documents::createPlaylist(const std::string& name, std::string* error) {
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "a playlist needs a name");
        return {};
    }
    Playlist playlist;
    playlist.id = nextId("playlist");
    playlist.name = clean;
    fPlaylists.push_back(std::move(playlist));
    return fPlaylists.back().id;
}

bool Documents::removePlaylist(const Id& playlistId, std::string* error) {
    const auto found = std::find_if(fPlaylists.begin(), fPlaylists.end(),
                                    [&](const Playlist& p) { return p.id == playlistId; });
    if (found == fPlaylists.end()) {
        fail(error, idError("playlist", playlistId));
        return false;
    }
    fPlaylists.erase(found);
    return true;
}

bool Documents::renamePlaylist(const Id& playlistId, const std::string& name,
                               std::string* error) {
    Playlist* playlist = findPlaylist_mut(playlistId);
    if (playlist == nullptr) {
        fail(error, idError("playlist", playlistId));
        return false;
    }
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "a playlist needs a name");
        return false;
    }
    playlist->name = clean;
    return true;
}

Id Documents::addPlaylistEntry(const Id& playlistId, const Id& showId,
                               const std::string& section, std::string* error) {
    Playlist* playlist = findPlaylist_mut(playlistId);
    if (playlist == nullptr) {
        fail(error, idError("playlist", playlistId));
        return {};
    }
    return insertPlaylistEntry(playlistId, showId, section, playlist->entries.size(), error);
}

Id Documents::insertPlaylistEntry(const Id& playlistId, const Id& showId,
                                  const std::string& section, size_t index,
                                  std::string* error) {
    Playlist* playlist = findPlaylist_mut(playlistId);
    if (playlist == nullptr) {
        fail(error, idError("playlist", playlistId));
        return {};
    }
    if (findShow(showId) == nullptr) {
        fail(error, idError("show", showId));
        return {};
    }
    if (index > playlist->entries.size()) {
        fail(error, "entry index " + std::to_string(index) + " is past the end (" +
                        std::to_string(playlist->entries.size()) + " entries)");
        return {};
    }
    PlaylistEntry entry;
    entry.id = nextId("entry");
    entry.showId = showId;
    entry.section = trimmed(section);
    const Id newId = entry.id;
    playlist->entries.insert(playlist->entries.begin() + static_cast<long>(index),
                             std::move(entry));
    return newId;
}

bool Documents::removePlaylistEntry(const Id& playlistId, const Id& entryId,
                                    std::string* error) {
    Playlist* playlist = findPlaylist_mut(playlistId);
    if (playlist == nullptr) {
        fail(error, idError("playlist", playlistId));
        return false;
    }
    const int index = findEntryIndex(*playlist, entryId);
    if (index < 0) {
        fail(error, idError("entry", entryId));
        return false;
    }
    playlist->entries.erase(playlist->entries.begin() + index);
    return true;
}

bool Documents::movePlaylistEntry(const Id& playlistId, const Id& entryId, size_t toIndex,
                                  std::string* error) {
    Playlist* playlist = findPlaylist_mut(playlistId);
    if (playlist == nullptr) {
        fail(error, idError("playlist", playlistId));
        return false;
    }
    const int from = findEntryIndex(*playlist, entryId);
    if (from < 0) {
        fail(error, idError("entry", entryId));
        return false;
    }
    if (toIndex >= playlist->entries.size()) {
        fail(error, "entry index " + std::to_string(toIndex) + " is past the end (" +
                        std::to_string(playlist->entries.size()) + " entries)");
        return false;
    }
    const PlaylistEntry moving = playlist->entries[static_cast<size_t>(from)];
    playlist->entries.erase(playlist->entries.begin() + from);
    playlist->entries.insert(playlist->entries.begin() + static_cast<long>(toIndex), moving);
    return true;
}

bool Documents::setPlaylistEntrySection(const Id& playlistId, const Id& entryId,
                                        const std::string& section, std::string* error) {
    Playlist* playlist = findPlaylist_mut(playlistId);
    if (playlist == nullptr) {
        fail(error, idError("playlist", playlistId));
        return false;
    }
    const int index = findEntryIndex(*playlist, entryId);
    if (index < 0) {
        fail(error, idError("entry", entryId));
        return false;
    }
    playlist->entries[static_cast<size_t>(index)].section = trimmed(section);
    return true;
}

const Playlist* Documents::findPlaylist(const Id& playlistId) const {
    const auto found = std::find_if(fPlaylists.begin(), fPlaylists.end(),
                                    [&](const Playlist& p) { return p.id == playlistId; });
    return found == fPlaylists.end() ? nullptr : &*found;
}

std::vector<Id> Documents::playlistIds() const {
    std::vector<Id> ids;
    ids.reserve(fPlaylists.size());
    for (const Playlist& playlist : fPlaylists) {
        ids.push_back(playlist.id);
    }
    return ids;
}

// --- internals ----------------------------------------------------------------

Show* Documents::findShow_mut(const Id& showId) {
    const auto found = std::find_if(fShows.begin(), fShows.end(),
                                    [&](const Show& s) { return s.id == showId; });
    return found == fShows.end() ? nullptr : &*found;
}

Style* Documents::findStyle_mut(const Id& styleId) {
    const auto found = std::find_if(fStyles.begin(), fStyles.end(),
                                    [&](const Style& s) { return s.id == styleId; });
    return found == fStyles.end() ? nullptr : &*found;
}

Playlist* Documents::findPlaylist_mut(const Id& playlistId) {
    const auto found = std::find_if(fPlaylists.begin(), fPlaylists.end(),
                                    [&](const Playlist& p) { return p.id == playlistId; });
    return found == fPlaylists.end() ? nullptr : &*found;
}

int Documents::findSlideIndex(const Show& show, const Id& slideId) {
    const auto found = std::find_if(show.slides.begin(), show.slides.end(),
                                    [&](const Slide& s) { return s.id == slideId; });
    return found == show.slides.end() ? -1
                                      : static_cast<int>(found - show.slides.begin());
}

int Documents::findEntryIndex(const Playlist& playlist, const Id& entryId) {
    const auto found = std::find_if(playlist.entries.begin(), playlist.entries.end(),
                                    [&](const PlaylistEntry& e) { return e.id == entryId; });
    return found == playlist.entries.end() ? -1
                                           : static_cast<int>(found - playlist.entries.begin());
}

bool Documents::validColor(const std::string& color, std::string* error) {
    const bool shapeOk = color.size() == 4 || color.size() == 7;
    const bool hexOk =
        shapeOk &&
        std::all_of(color.begin() + 1, color.end(),
                    [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
    if (!hexOk && error != nullptr) {
        fail(error, "colour '" + color + "' is not #rgb or #rrggbb");
    }
    return hexOk;
}

Id Documents::nextId(const char* kind) {
    return std::string(kind) + std::to_string(fNextOrdinal++);
}

}  // namespace openshow::domain
