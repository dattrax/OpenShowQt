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

// A box only paints if the math survives it: NaN/inf cannot be placed, and a
// non-positive width or height leaves nothing to draw (task 4.3). Shared by
// every path that places one, so the slide-level and box-level operations
// cannot drift into honouring different numbers.
bool placeableBox(double x, double y, double width, double height) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
           std::isfinite(height) && width > 0.0 && height > 0.0;
}

// Typography (task 4.4): the size convention is the domain's (0 = the
// renderer's default), the alignment is the three words the scene wire
// carries, and the line box is a sane multiplier — beyond these the
// renderer would either not place the text or stack lines into each
// other.
bool validTypography(double fontSize, const std::string& align, double lineSpacing,
                     std::string* error) {
    if (!std::isfinite(fontSize) || fontSize < 0.0) {
        fail(error, "font size must be finite and >= 0 (0 means renderer default)");
        return false;
    }
    if (!isThemeAlign(align)) {
        fail(error, "align must be \"left\", \"center\" or \"right\" (got '" + align + "')");
        return false;
    }
    if (!std::isfinite(lineSpacing) || lineSpacing < 0.25 || lineSpacing > 8.0) {
        fail(error, "line spacing must be a finite multiplier within 0.25..8");
        return false;
    }
    return true;
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

Id Documents::createScriptureShow(const std::string& name, const ScriptureProvenance& scripture,
                                  std::string* error) {
    // The provenance is the point of the item: the reference says what the
    // words are and the translation says which book they are in. An empty
    // reference or translation would make the saved words unattributable,
    // so the creation refuses rather than storing a mystery.
    const std::string cleanReference = trimmed(scripture.reference);
    const std::string cleanTranslation = trimmed(scripture.translation);
    if (cleanReference.empty()) {
        fail(error, "a scripture item needs the reference it was looked up as");
        return {};
    }
    if (cleanTranslation.empty()) {
        fail(error, "a scripture item needs the translation it was fetched in");
        return {};
    }
    Show show;
    show.id = nextId("show");
    show.name = trimmed(name).empty() ? cleanReference : trimmed(name);
    show.scheduleOwned = true;
    show.scripture = ScriptureProvenance{cleanReference, cleanTranslation, scripture.attribution};
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
    // A created slide owns ONE box with the default geometry (task 6.17a):
    // the slide an operator gets is the lyric box they could type straight
    // into before multiple boxes existed, not a slide that refuses typing.
    SlideItem box;
    box.id = nextId("text");
    slide.items.push_back(std::move(box));
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
    Slide& slide = show->slides[static_cast<size_t>(index)];
    if (slide.items.empty()) {
        // The single-text op has nothing to address. Creating a box here
        // would mean typing into a slide the operator had deliberately
        // emptied silently grew one back, so the refusal is the answer.
        fail(error, "slide '" + slideId + "' has no item to write into");
        return false;
    }
    if (slide.items.front().kind == ItemKind::Image) {
        // Task 6.17b: the legacy view addresses item 0, and item 0 can now
        // be a picture. Refusing beats the two lies — writing words INTO
        // the image item, or inventing a text box underneath it — and
        // matches setItemLines refusing words for an image item.
        fail(error, "the slide's first item '" + slide.items.front().id +
                        "' is an image; there is no text box to write into");
        return false;
    }
    slide.items.front().lines = lines;
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

bool Documents::setSlideGeometry(const Id& showId, const Id& slideId, double x, double y,
                                 double width, double height, std::string* error) {
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
    Slide& slide = show->slides[static_cast<size_t>(index)];
    if (slide.items.empty()) {
        fail(error, "slide '" + slideId + "' has no item to place");
        return false;
    }
    if (!placeableBox(x, y, width, height)) {
        fail(error, "item '" + slide.items.front().id +
                        "' needs a finite box with positive width and height");
        return false;
    }
    SlideItem& box = slide.items.front();
    box.x = x;
    box.y = y;
    box.width = width;
    box.height = height;
    return true;
}

// --- slide items (tasks 6.17a/6.17b, review R17) ------------------------------------

Id Documents::addItem(const Id& showId, const Id& slideId, const SlideItem& item,
                      std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return {};
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        fail(error, idError("slide", slideId));
        return {};
    }
    Slide& slide = show->slides[static_cast<size_t>(index)];
    // The ceiling is the scene wire's own: a ninth item would compile into a
    // scene the worker refuses, so the operator would be told at present time
    // that a slide they composed cannot be shown. The stack is ONE budget for
    // both kinds (6.17b): pictures cover words where they overlap, and what
    // bounds the composition is the message, not the kind.
    if (slide.items.size() >= kMaxSlideItems) {
        fail(error, "slide '" + slideId + "' already holds the " +
                        std::to_string(kMaxSlideItems) +
                        " items a scene can carry");
        return {};
    }
    if (!placeableBox(item.x, item.y, item.width, item.height)) {
        fail(error, "a new slide item needs finite coordinates with positive width and height");
        return {};
    }
    // Kind discipline (task 6.17b): which content fields mean something is
    // always the kind, so a half-filled item is refused at the door. An
    // image item names an asset that EXISTS and whose name the media
    // pipeline treats as a still — refusing here what the compiler would
    // otherwise refuse at present time is the same rule that made the stack
    // ceiling a model check.
    if (item.kind == ItemKind::Image) {
        if (item.assetId.empty()) {
            fail(error, "an image item needs an asset to place");
            return {};
        }
        if (!item.lines.empty()) {
            fail(error, "an image item cannot carry words");
            return {};
        }
        const Asset* asset = findAsset(item.assetId);
        if (asset == nullptr) {
            fail(error, "image item names unknown asset '" + item.assetId + "'");
            return {};
        }
        if (assetPathLooksLikeVideo(asset->sourcePath)) {
            fail(error, "asset '" + item.assetId +
                            "' is a video, and foreground items composite stills");
            return {};
        }
    } else if (!item.assetId.empty()) {
        fail(error, "a text item cannot name an asset");
        return {};
    }
    SlideItem added = item;
    // The caller's id is dropped, not adopted: pasting the same slide twice
    // must produce two sets of items with two sets of identities, or the
    // second paste's edits would move the first paste's item on another slide.
    added.id = nextId(item.kind == ItemKind::Image ? "image" : "text");
    slide.items.push_back(std::move(added));
    return slide.items.back().id;
}

bool Documents::removeItem(const Id& showId, const Id& slideId, const Id& itemId,
                             std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int slideIndex = findSlideIndex(*show, slideId);
    if (slideIndex < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    Slide& slide = show->slides[static_cast<size_t>(slideIndex)];
    const int boxIndex = findItemIndex(slide, itemId);
    if (boxIndex < 0) {
        fail(error, "slide '" + slideId + "' has no item '" + itemId + "'");
        return false;
    }
    slide.items.erase(slide.items.begin() + boxIndex);
    return true;
}

bool Documents::setItemLines(const Id& showId, const Id& slideId, const Id& itemId,
                               const std::vector<std::string>& lines, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int slideIndex = findSlideIndex(*show, slideId);
    if (slideIndex < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    Slide& slide = show->slides[static_cast<size_t>(slideIndex)];
    const int boxIndex = findItemIndex(slide, itemId);
    if (boxIndex < 0) {
        fail(error, "slide '" + slideId + "' has no item '" + itemId + "'");
        return false;
    }
    SlideItem& target = slide.items[static_cast<size_t>(boxIndex)];
    if (target.kind == ItemKind::Image) {
        // Task 6.17b: words belong to a text item. An editor wired to the
        // wrong item — or a hand-built call — gets the refusal, not words
        // the compiler would drop on the floor.
        fail(error, "item '" + itemId + "' is an image; words belong to a text item");
        return false;
    }
    target.lines = lines;
    return true;
}

bool Documents::setItemGeometry(const Id& showId, const Id& slideId, const Id& itemId,
                                 double x, double y, double width, double height,
                                   std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int slideIndex = findSlideIndex(*show, slideId);
    if (slideIndex < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    Slide& slide = show->slides[static_cast<size_t>(slideIndex)];
    const int boxIndex = findItemIndex(slide, itemId);
    if (boxIndex < 0) {
        fail(error, "slide '" + slideId + "' has no item '" + itemId + "'");
        return false;
    }
    if (!placeableBox(x, y, width, height)) {
        fail(error, "item '" + itemId + "' needs a finite box with positive width and height");
        return false;
    }
    SlideItem& box = slide.items[static_cast<size_t>(boxIndex)];
    box.x = x;
    box.y = y;
    box.width = width;
    box.height = height;
    return true;
}

bool Documents::moveItem(const Id& showId, const Id& slideId, const Id& itemId, size_t toIndex,
                           std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    const int slideIndex = findSlideIndex(*show, slideId);
    if (slideIndex < 0) {
        fail(error, idError("slide", slideId));
        return false;
    }
    Slide& slide = show->slides[static_cast<size_t>(slideIndex)];
    const int from = findItemIndex(slide, itemId);
    if (from < 0) {
        fail(error, "slide '" + slideId + "' has no item '" + itemId + "'");
        return false;
    }
    if (toIndex >= slide.items.size()) {
        fail(error, "item index " + std::to_string(toIndex) + " is past the end (" +
                        std::to_string(slide.items.size()) + " items on slide '" + slideId +
                        "')");
        return false;
    }
    const SlideItem moving = slide.items[static_cast<size_t>(from)];
    slide.items.erase(slide.items.begin() + from);
    slide.items.insert(slide.items.begin() + static_cast<long>(toIndex), moving);
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

// --- themes ----------------------------------------------------------------

Documents::Documents() { ensureDefaultTheme(); }

Theme Documents::defaultTheme() {
    Theme theme;
    theme.id = kDefaultThemeId;
    theme.name = "Default";
    theme.textColor = "#ffffff";
    theme.backgroundColor = "#000000";
    theme.backgroundGradient.clear();  // flat
    theme.fontFamily.clear();          // the platform font
    theme.fontSize = 0.0;              // the renderer's own default size
    theme.align = "center";
    theme.lineSpacing = kDefaultLineSpacing;
    // No placement: the Default theme as seeded says nothing about WHERE
    // the words go, so every un-themed slide keeps drawing its authored
    // box — seeding moves no pixel in any existing document.
    theme.placement.reset();
    return theme;
}

void Documents::ensureDefaultTheme() {
    if (findTheme(kDefaultThemeId) != nullptr) {
        return;
    }
    fThemes.push_back(defaultTheme());
}

Id Documents::createTheme(const std::string& name, const std::string& textColor,
                          const std::string& backgroundColor,
                          const std::string& backgroundGradient, const std::string& fontFamily,
                          double fontSize, const std::string& align, double lineSpacing,
                          std::string* error) {
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "a theme needs a name");
        return {};
    }
    if (!validColor(textColor, error) || !validColor(backgroundColor, nullptr)) {
        if (error && error->empty()) {
            fail(error, "background colour '" + backgroundColor + "' is not #rgb or #rrggbb");
        }
        return {};
    }
    // The gradient colour is optional, but when present it is exactly as
    // unforgiving as the other two (6.17c3): the renderer has one parse
    // for it and no fallback.
    if (!backgroundGradient.empty() && !validColor(backgroundGradient, nullptr)) {
        fail(error, "background gradient colour '" + backgroundGradient +
                        "' is not #rgb or #rrggbb");
        return {};
    }
    if (!validTypography(fontSize, align, lineSpacing, error)) {
        return {};
    }
    Theme theme;
    theme.id = nextId("theme");
    theme.name = clean;
    theme.textColor = textColor;
    theme.backgroundColor = backgroundColor;
    theme.backgroundGradient = backgroundGradient;
    theme.fontFamily = fontFamily;
    theme.fontSize = fontSize;
    theme.align = align;
    theme.lineSpacing = lineSpacing;
    fThemes.push_back(std::move(theme));
    return fThemes.back().id;
}

bool Documents::removeTheme(const Id& themeId, std::string* error) {
    // The Default theme is the floor every other lookup falls back to
    // (operator amendment 2026-11-02); removing it would recreate exactly
    // the fragility it exists to close, so the application refuses it. The
    // entry is a theme like any other in every other respect — a hand-edited
    // file that nicked it is healed by ensureDefaultTheme at the next load.
    if (themeId == kDefaultThemeId) {
        fail(error, "the default theme cannot be removed");
        return false;
    }
    const auto found = std::find_if(fThemes.begin(), fThemes.end(),
                                    [&](const Theme& s) { return s.id == themeId; });
    if (found == fThemes.end()) {
        fail(error, idError("theme", themeId));
        return false;
    }
    fThemes.erase(found);
    // Dangling theme references degrade to "no theme" rather than
    // blocking the removal: clearing is the only consistent outcome.
    for (Show& show : fShows) {
        if (show.defaultThemeId == themeId) {
            show.defaultThemeId.clear();
        }
        for (Slide& slide : show.slides) {
            if (slide.themeId == themeId) {
                slide.themeId.clear();
            }
        }
    }
    // The current theme is a reference like any other (task TH.2): with
    // the theme gone there is nothing to wear, and the service is honestly
    // back to per-slide themes.
    if (fCurrentThemeId == themeId) {
        fCurrentThemeId.clear();
    }
    return true;
}

bool Documents::renameTheme(const Id& themeId, const std::string& name, std::string* error) {
    Theme* theme = findTheme_mut(themeId);
    if (theme == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "a theme needs a name");
        return false;
    }
    theme->name = clean;
    return true;
}

bool Documents::updateTheme(const Id& themeId, const std::string& textColor,
                            const std::string& backgroundColor,
                            const std::string& backgroundGradient, const std::string& fontFamily,
                            double fontSize, const std::string& align, double lineSpacing,
                            std::string* error) {
    Theme* theme = findTheme_mut(themeId);
    if (theme == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    if (!validColor(textColor, error) || !validColor(backgroundColor, nullptr)) {
        if (error && error->empty()) {
            fail(error, "background colour '" + backgroundColor + "' is not #rgb or #rrggbb");
        }
        return false;
    }
    if (!backgroundGradient.empty() && !validColor(backgroundGradient, nullptr)) {
        fail(error, "background gradient colour '" + backgroundGradient +
                        "' is not #rgb or #rrggbb");
        return false;
    }
    if (!validTypography(fontSize, align, lineSpacing, error)) {
        return false;
    }
    theme->textColor = textColor;
    theme->backgroundColor = backgroundColor;
    theme->backgroundGradient = backgroundGradient;
    theme->fontFamily = fontFamily;
    theme->fontSize = fontSize;
    theme->align = align;
    theme->lineSpacing = lineSpacing;
    return true;
}

bool Documents::setThemePlacement(const Id& themeId, const BoxRect& rect, std::string* error) {
    Theme* theme = findTheme_mut(themeId);
    if (theme == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    // The same vetting a SlideItem's box gets (task 4.3): the renderer
    // would either not place the box or collapse it.
    if (!placeableBox(rect.x, rect.y, rect.width, rect.height)) {
        fail(error, "theme '" + themeId +
                        "' needs a finite placement with positive width and height");
        return false;
    }
    theme->placement = rect;
    return true;
}

bool Documents::clearThemePlacement(const Id& themeId, std::string* error) {
    Theme* theme = findTheme_mut(themeId);
    if (theme == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    if (!theme->placement.has_value()) {
        fail(error, "theme '" + themeId + "' carries no placement to remove");
        return false;
    }
    theme->placement.reset();
    return true;
}

bool Documents::setThemeContexts(const Id& themeId, std::vector<TextContext> contexts,
                                 std::string* error) {
    Theme* theme = findTheme_mut(themeId);
    if (theme == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    // The loader's own contract (task TF.1), kept in one place so a theme
    // this verb accepts is exactly a theme this build can save without a
    // later load refusing it. There is deliberately no line-count field to
    // vet: how many lines fit is the worker's measurement, never stored.
    for (size_t i = 0; i < contexts.size(); ++i) {
        const TextContext& context = contexts[i];
        if (context.name.empty()) {
            fail(error, "a context of theme '" + themeId + "' has no name");
            return false;
        }
        for (size_t earlier = 0; earlier < i; ++earlier) {
            if (contexts[earlier].name == context.name) {
                fail(error, "theme '" + themeId + "' names context '" + context.name +
                                "' twice");
                return false;
            }
        }
        if (context.box.has_value() && context.inset.has_value()) {
            fail(error, "context '" + context.name +
                            "' gives both a box and an inset; the inset would shrink"
                            " nothing the box did not already say");
            return false;
        }
        if (context.box.has_value() &&
            !placeableBox(context.box->x, context.box->y, context.box->width,
                          context.box->height)) {
            fail(error, "context '" + context.name +
                            "' needs a finite box with positive width and height");
            return false;
        }
        if (context.fontSize.has_value() &&
            (!std::isfinite(*context.fontSize) || *context.fontSize < 0.0)) {
            fail(error, "context '" + context.name +
                            "' carries a font size the renderer cannot honour");
            return false;
        }
        if (context.lineSpacing.has_value() && (!std::isfinite(*context.lineSpacing) ||
                                                *context.lineSpacing < 0.25 ||
                                                *context.lineSpacing > 8.0)) {
            fail(error, "context '" + context.name +
                            "' carries a line spacing the renderer cannot honour");
            return false;
        }
        if (context.align.has_value() && !isThemeAlign(*context.align)) {
            fail(error, "context '" + context.name + "' has align '" + *context.align +
                            "', not an alignment this build can draw");
            return false;
        }
        if (context.inset.has_value() &&
            (!std::isfinite(*context.inset) || *context.inset < 0.0)) {
            fail(error, "context '" + context.name +
                            "' carries an inset that shrinks nothing sane");
            return false;
        }
    }
    theme->contexts = std::move(contexts);
    return true;
}

bool Documents::setThemeTextStyle(const Id& themeId, ThemeTextStyle style, std::string* error) {
    Theme* theme = findTheme_mut(themeId);
    if (theme == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    // Same vetting the loader quotes (task TS.1): weight in band, style/fit
    // in their word sets, a finite bounded min, complete effect records.
    if (!validThemeTextStyle(style, error)) {
        return false;
    }
    theme->fontWeight = style.fontWeight;
    theme->fontStyle = style.fontStyle;
    theme->fitMode = style.fitMode;
    theme->minFontSize = style.minFontSize;
    theme->stroke = style.stroke;
    theme->shadow = style.shadow;
    return true;
}

bool Documents::setSlideTextContext(const Id& showId, const Id& slideId,
                                    const std::string& name, std::string* error) {
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
    // The name is trusted, not checked: "not yet a context" is a thing an
    // import says on purpose, and the theme that grows the name is the
    // answer — nothing wears a ghost either way.
    show->slides[static_cast<size_t>(index)].textContext = name;
    return true;
}

bool Documents::setShowDefaultTheme(const Id& showId, const Id& themeId, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    if (!themeId.empty() && findTheme(themeId) == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    show->defaultThemeId = themeId;
    return true;
}

bool Documents::setSlideTheme(const Id& showId, const Id& slideId, const Id& themeId,
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
    if (!themeId.empty() && findTheme(themeId) == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    show->slides[static_cast<size_t>(index)].themeId = themeId;
    return true;
}

const Theme* Documents::findTheme(const Id& themeId) const {
    const auto found = std::find_if(fThemes.begin(), fThemes.end(),
                                    [&](const Theme& s) { return s.id == themeId; });
    return found == fThemes.end() ? nullptr : &*found;
}

std::vector<Id> Documents::themeIds() const {
    std::vector<Id> ids;
    ids.reserve(fThemes.size());
    for (const Theme& theme : fThemes) {
        ids.push_back(theme.id);
    }
    return ids;
}

const Theme* Documents::themeForSlide(const Id& showId, const Id& slideId) const {
    const Show* show = findShow(showId);
    if (show == nullptr) {
        return nullptr;
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        return nullptr;
    }
    const Slide& slide = show->slides[static_cast<size_t>(index)];
    if (!slide.themeId.empty()) {
        return findTheme(slide.themeId);  // cleared references mean none
    }
    return show->defaultThemeId.empty() ? nullptr : findTheme(show->defaultThemeId);
}

// --- themes: current, customization, the full chain (task TH.2) ----------

bool Documents::validStroke(const TextStroke& stroke, std::string* error) {
    // A record is complete or refused; `enabled:false` is data, not noise.
    // Width 0 is a valid record that draws no ink — "as if outlined, and
    // the outline is hairline-thin" — while a negative or oversized one is
    // a value no renderer could honour (theme-editor review limits).
    if (!validColor(stroke.color, error)) {
        return false;
    }
    if (!std::isfinite(stroke.width) || stroke.width < 0.0 || stroke.width > 32.0) {
        fail(error, "stroke width must be a finite authored width within 0..32");
        return false;
    }
    return true;
}

bool Documents::validShadow(const TextShadow& shadow, std::string* error) {
    if (!validColor(shadow.color, error)) {
        return false;
    }
    if (!std::isfinite(shadow.opacity) || shadow.opacity < 0.0 || shadow.opacity > 1.0) {
        fail(error, "shadow opacity must be a finite fraction within 0..1");
        return false;
    }
    if (!std::isfinite(shadow.offsetX) || std::abs(shadow.offsetX) > 256.0 ||
        !std::isfinite(shadow.offsetY) || std::abs(shadow.offsetY) > 256.0) {
        fail(error, "shadow offsets must be finite authored widths within -256..256");
        return false;
    }
    if (!std::isfinite(shadow.blurSigma) || shadow.blurSigma < 0.0 ||
        shadow.blurSigma > 64.0) {
        fail(error, "shadow blur must be a finite authored sigma within 0..64");
        return false;
    }
    return true;
}

bool Documents::validThemeTextStyle(const ThemeTextStyle& style, std::string* error) {
    // The one sentence every door (verb, customization, loader) quotes.
    if (style.fontWeight.has_value() && !isFontWeightInBand(*style.fontWeight)) {
        fail(error, "font weight must be an integer 100..900 (got " +
                        std::to_string(*style.fontWeight) + ")");
        return false;
    }
    if (style.fontStyle.has_value() && !isFontStyleName(*style.fontStyle)) {
        fail(error, "font style must be \"normal\" or \"italic\" (got '" + *style.fontStyle +
                        "')");
        return false;
    }
    if (style.fitMode.has_value() && !isFitModeName(*style.fitMode)) {
        fail(error, "fit must be \"shrink\" or \"fixed\" (got '" + *style.fitMode + "')");
        return false;
    }
    if (style.minFontSize.has_value() &&
        (!std::isfinite(*style.minFontSize) || *style.minFontSize < 1.0 ||
         *style.minFontSize > 4096.0)) {
        fail(error, "minimum font size must be a finite size within 1..4096");
        return false;
    }
    if (style.stroke.has_value() && !validStroke(*style.stroke, error)) {
        return false;
    }
    if (style.shadow.has_value() && !validShadow(*style.shadow, error)) {
        return false;
    }
    return true;
}

bool Documents::validCustomizations(const SlideCustomization& c, std::string* error) {
    // Every check is the theme's own check on the same field, so a slide
    // can never customize itself into an appearance a theme could not
    // carry. The two empties that ARE answers — a flat gradient over a
    // graded theme, no family over a named one — are allowed exactly there.
    if (c.textColor.has_value() && !validColor(*c.textColor, error)) {
        return false;
    }
    if (c.backgroundColor.has_value() && !validColor(*c.backgroundColor, error)) {
        return false;
    }
    if (c.backgroundGradient.has_value() && !c.backgroundGradient->empty() &&
        !validColor(*c.backgroundGradient, error)) {
        return false;
    }
    if (c.fontSize.has_value() && (!std::isfinite(*c.fontSize) || *c.fontSize < 0.0)) {
        fail(error, "a customized font size must be finite and >= 0 (0 means renderer default)");
        return false;
    }
    if (c.align.has_value() && !isThemeAlign(*c.align)) {
        fail(error, "align must be \"left\", \"center\" or \"right\" (got '" + *c.align + "')");
        return false;
    }
    if (c.lineSpacing.has_value() &&
        (!std::isfinite(*c.lineSpacing) || *c.lineSpacing < 0.25 || *c.lineSpacing > 8.0)) {
        fail(error, "line spacing must be a finite multiplier within 0.25..8");
        return false;
    }
    // A customized rectangle is vetted exactly like a theme's and like an
    // authored box — one rule for every box a slide can end up drawing.
    if (c.placement.has_value() &&
        !placeableBox(c.placement->x, c.placement->y, c.placement->width, c.placement->height)) {
        fail(error, "a customized placement needs finite coordinates and "
                    "positive width and height");
        return false;
    }
    // The TS.1 style fields face the theme's own sentence, unchanged (task
    // TS.1): a slide can no more customize in an uncarryable weight, a
    // half-record stroke or an off-list fit than it can a bad colour.
    ThemeTextStyle styled;
    styled.fontWeight = c.fontWeight;
    styled.fontStyle = c.fontStyle;
    styled.fitMode = c.fitMode;
    styled.minFontSize = c.minFontSize;
    styled.stroke = c.stroke;
    styled.shadow = c.shadow;
    return validThemeTextStyle(styled, error);
}

bool Documents::setCurrentTheme(const Id& themeId, std::string* error) {
    if (!themeId.empty() && findTheme(themeId) == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    fCurrentThemeId = themeId;
    return true;
}

bool Documents::setSlideCustomizations(const Id& showId, const Id& slideId,
                                       const SlideCustomization& customizations,
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
    if (!validCustomizations(customizations, error)) {
        return false;
    }
    show->slides[static_cast<size_t>(index)].customizations = customizations;
    return true;
}

bool Documents::removeSlideCustomizations(const Id& showId, const Id& slideId,
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
    Slide& slide = show->slides[static_cast<size_t>(index)];
    if (slide.customizations.empty()) {
        fail(error, "slide '" + slideId + "' carries no customizations to remove");
        return false;
    }
    slide.customizations = SlideCustomization{};
    return true;
}

bool Documents::removeShowCustomizations(const Id& showId, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    size_t cleared = 0;
    for (Slide& slide : show->slides) {
        if (!slide.customizations.empty()) {
            slide.customizations = SlideCustomization{};
            ++cleared;
        }
    }
    if (cleared == 0) {
        // "Nothing was customized" is a wrong click worth reporting: an
        // undo step must not exist for a change that changed nothing
        // (dissolveGroup states the same rule for groups).
        fail(error, "show '" + showId + "' has no customized slides to restore");
        return false;
    }
    return true;
}

bool Documents::applyThemeToSlides(const Id& showId, const std::vector<Id>& slideIds,
                                   const Id& themeId, std::string* error) {
    Show* show = findShow_mut(showId);
    if (show == nullptr) {
        fail(error, idError("show", showId));
        return false;
    }
    if (slideIds.empty()) {
        fail(error, "applying a theme names no slides");
        return false;
    }
    if (!themeId.empty() && findTheme(themeId) == nullptr) {
        fail(error, idError("theme", themeId));
        return false;
    }
    // Resolve every target before touching any: a list with one dead id
    // changes nothing rather than half-applying.
    std::vector<size_t> targets;
    targets.reserve(slideIds.size());
    for (const Id& id : slideIds) {
        const int index = findSlideIndex(*show, id);
        if (index < 0) {
            fail(error, idError("slide", id));
            return false;
        }
        targets.push_back(static_cast<size_t>(index));
    }
    for (const size_t index : targets) {
        show->slides[index].themeId = themeId;
    }
    return true;
}

ResolvedAppearance Documents::appearanceForSlide(const Id& showId, const Id& slideId) const {
    ResolvedAppearance out;
    const Show* show = findShow(showId);
    if (show == nullptr) {
        return out;
    }
    const int index = findSlideIndex(*show, slideId);
    if (index < 0) {
        return out;
    }
    const Slide& slide = show->slides[static_cast<size_t>(index)];
    const SlideCustomization& c = slide.customizations;
    // The theme layers under the customization, in the fixed priority:
    // current theme first, then the slide's own, then the show's. A
    // reference to a vanished theme is no layer at all, the same
    // degradation themeForSlide has always applied.
    const Theme* layers[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t count = 0;
    if (!fCurrentThemeId.empty()) {
        if (const Theme* t = findTheme(fCurrentThemeId)) {
            layers[count++] = t;
        }
    }
    if (!slide.themeId.empty()) {
        if (const Theme* t = findTheme(slide.themeId)) {
            layers[count++] = t;
        }
    }
    if (!show->defaultThemeId.empty()) {
        if (const Theme* t = findTheme(show->defaultThemeId)) {
            layers[count++] = t;
        }
    }
    // The Default theme closes the chain (operator amendment 2026-11-02):
    // whatever it carries answers for slides no other layer spoke to, and a
    // reference to a vanished theme lands on it rather than on scattered
    // built-ins. As SEEDED it says colour-wise nothing the compile does not
    // already apply (white on black, centred, renderer size), so documents
    // drawn before it existed draw the same pixels; the operator tuning it
    // is the point — house defaults become a thing the document can carry.
    if (const Theme* d = findTheme(kDefaultThemeId)) {
        bool already = false;
        for (size_t i = 0; i < count; ++i) {
            already = already || layers[i] == d;
        }
        if (!already) {
            layers[count++] = d;
        }
    }
    // The slide's named text context (milestone TF) layers between the
    // customization and the themes: the first theme in chain order that
    // carries the name supplies it, and a name no theme carries is no
    // layer at all — the slide resolves exactly as it did before the name
    // existed, which is what lets an import name `scripture` today and the
    // theme grow it tomorrow. Colours never come from a context: it says
    // how text is set, and the theme's own look is untouched.
    const TextContext* ctx = nullptr;
    if (!slide.textContext.empty()) {
        for (size_t i = 0; i < count && ctx == nullptr; ++i) {
            for (const TextContext& candidate : layers[i]->contexts) {
                if (candidate.name == slide.textContext) {
                    ctx = &candidate;
                    break;
                }
            }
        }
    }
    // Which context answered is itself an answer (task TF.4): the pages
    // that say "set in the scripture context" read it here rather than
    // re-walking the chain and guessing what layered.
    if (ctx != nullptr) {
        out.contextName = ctx->name;
    }
    // Per property, the first layer with something to say wins; a layer
    // silent on a property (an empty string, a size of 0) falls through.
    // A NULL contextField is the property's way of saying a context can
    // NEVER speak it (the colours) — the pointer is never dereferenced
    // then; a null pointer-to-member dereferenced is not "no value" but
    // no definition at all.
    auto pickString = [&](const std::optional<std::string>& own,
                          const std::optional<std::string> TextContext::*contextField,
                          const std::string Theme::*field) -> std::string {
        if (own.has_value()) {
            return *own;
        }
        if (ctx != nullptr && contextField != nullptr && (ctx->*contextField).has_value()) {
            return *(ctx->*contextField);
        }
        for (size_t i = 0; i < count; ++i) {
            if (!(layers[i]->*field).empty()) {
                return layers[i]->*field;
            }
        }
        return std::string();
    };
    out.textColor = pickString(c.textColor, nullptr, &Theme::textColor);
    out.backgroundColor = pickString(c.backgroundColor, nullptr, &Theme::backgroundColor);
    out.backgroundGradient = pickString(c.backgroundGradient, nullptr, &Theme::backgroundGradient);
    out.fontFamily = pickString(c.fontFamily, &TextContext::fontFamily, &Theme::fontFamily);
    out.align = pickString(c.align, &TextContext::align, &Theme::align);
    // A customized number is an ANSWER, even 0 — "renderer default" over
    // a sized theme is a thing an operator means. A context's number is
    // an answer too, for the same reason: it is present exactly when the
    // context means it. Only an absent field falls through to the layers.
    if (c.fontSize.has_value()) {
        out.fontSize = *c.fontSize;
    } else if (ctx != nullptr && ctx->fontSize.has_value()) {
        out.fontSize = *ctx->fontSize;
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (layers[i]->fontSize != 0.0) {
                out.fontSize = layers[i]->fontSize;
                break;
            }
        }
    }
    if (c.lineSpacing.has_value()) {
        out.lineSpacing = *c.lineSpacing;
    } else if (ctx != nullptr && ctx->lineSpacing.has_value()) {
        out.lineSpacing = *ctx->lineSpacing;
    } else if (count > 0) {
        out.lineSpacing = layers[0]->lineSpacing;
    } else {
        out.lineSpacing = kDefaultLineSpacing;  // no layer spoke
    }
    // Placement (amendment 2026-11-01) resolves on the same walk. A
    // customized rectangle is an answer; then the context's own box, then
    // its inset applied to whichever box the slide would otherwise land
    // on; then the first theme that CARRIES one places the words, and a
    // theme without a rectangle is silent rather than zero-sized. When no
    // layer speaks the optional stays empty, and the compile draws the
    // slide's own authored box — so nothing moves for documents and themes
    // made before this existed.
    if (c.placement.has_value()) {
        out.placement = c.placement;
    } else if (ctx != nullptr && ctx->box.has_value()) {
        out.placement = ctx->box;
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (layers[i]->placement.has_value()) {
                out.placement = layers[i]->placement;
                break;
            }
        }
        if (ctx != nullptr && ctx->inset.has_value()) {
            // The inset shrinks the box THIS slide lands on without the
            // context: the themed rectangle if one wore the slide, else
            // the slide's own authored primary box. No box to shrink
            // (a slide with no text item at all) means no opinion.
            const SlideItem* primary = nullptr;
            for (const SlideItem& item : slide.items) {
                if (item.kind != ItemKind::Image) {
                    primary = &item;
                    break;
                }
            }
            BoxRect base = out.placement.value_or(
                primary != nullptr
                    ? BoxRect{primary->x, primary->y, primary->width, primary->height}
                    : BoxRect{});
            if (out.placement.has_value() || primary != nullptr) {
                const double side = *ctx->inset;
                base.x += side;
                base.y += side;
                base.width = std::max(1.0, base.width - 2.0 * side);
                base.height = std::max(1.0, base.height - 2.0 * side);
                out.placement = base;
            }
        }
    }
    // TS.1 style fields. Contexts say nothing of these yet (TV.3 adds
    // them), so the walk is customization first, then the first theme that
    // CARRIES the field, then the contract's concrete default. An effect
    // record is atomic: the first layer that carries one answers whole,
    // and a carried `enabled:false` is honoured as the explicit refusal it
    // is — the reason resolution reads has_value and not "is truthy".
    if (c.fontWeight.has_value()) {
        out.fontWeight = *c.fontWeight;
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (layers[i]->fontWeight.has_value()) {
                out.fontWeight = *layers[i]->fontWeight;
                break;
            }
        }
    }
    if (c.minFontSize.has_value()) {
        out.minFontSize = *c.minFontSize;
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (layers[i]->minFontSize.has_value()) {
                out.minFontSize = *layers[i]->minFontSize;
                break;
            }
        }
    }
    auto pickStyleString = [&](const std::optional<std::string>& own,
                               const std::optional<std::string> Theme::*field,
                               const char* fallback) -> std::string {
        if (own.has_value()) {
            return *own;
        }
        for (size_t i = 0; i < count; ++i) {
            if ((layers[i]->*field).has_value()) {
                return *(layers[i]->*field);
            }
        }
        return fallback;
    };
    out.fontStyle = pickStyleString(c.fontStyle, &Theme::fontStyle, "normal");
    out.fitMode = pickStyleString(c.fitMode, &Theme::fitMode, "shrink");
    if (c.stroke.has_value()) {
        out.stroke = *c.stroke;
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (layers[i]->stroke.has_value()) {
                out.stroke = *layers[i]->stroke;
                break;
            }
        }
    }
    if (c.shadow.has_value()) {
        out.shadow = *c.shadow;
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (layers[i]->shadow.has_value()) {
                out.shadow = *layers[i]->shadow;
                break;
            }
        }
    }
    return out;
}

// --- assets ----------------------------------------------------------------

Id Documents::addAsset(const std::string& name, const std::string& sourcePath,
                       std::string* error) {
    const std::string clean = trimmed(name);
    if (clean.empty()) {
        fail(error, "an asset needs a name");
        return {};
    }
    if (sourcePath.empty()) {
        fail(error, "asset '" + clean + "' has no source path");
        return {};
    }
    Asset asset;
    asset.id = nextId("asset");
    asset.name = clean;
    asset.sourcePath = sourcePath;
    fAssets.push_back(std::move(asset));
    return fAssets.back().id;
}

bool Documents::removeAsset(const Id& assetId, std::string* error) {
    const auto found = std::find_if(fAssets.begin(), fAssets.end(),
                                    [&](const Asset& a) { return a.id == assetId; });
    if (found == fAssets.end()) {
        fail(error, idError("asset", assetId));
        return false;
    }
    fAssets.erase(found);
    // Backgrounds naming the asset degrade to none, mirroring themes. A
    // FOREGROUND item cannot degrade the same way: an image item without
    // its picture is not a half-drawn item but a claim about the stack the
    // worker could never honour, so the item leaves with the asset — and
    // the undo step that restores the asset restores the item beside it.
    for (Show& show : fShows) {
        for (Slide& slide : show.slides) {
            if (slide.backgroundAssetId == assetId) {
                slide.backgroundAssetId.clear();
            }
            slide.items.erase(
                std::remove_if(slide.items.begin(), slide.items.end(),
                               [&](const SlideItem& item) {
                                   return item.kind == ItemKind::Image &&
                                          item.assetId == assetId;
                               }),
                slide.items.end());
        }
    }
    return true;
}

bool Documents::setSlideBackground(const Id& showId, const Id& slideId, const Id& assetId,
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
    if (!assetId.empty() && findAsset(assetId) == nullptr) {
        fail(error, idError("asset", assetId));
        return false;
    }
    show->slides[static_cast<size_t>(index)].backgroundAssetId = assetId;
    return true;
}

bool Documents::setSlideBackgroundFit(const Id& showId, const Id& slideId,
                                      BackgroundFit fit, std::string* error) {
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
    // The enum cannot carry a lie, and the choice means nothing or something
    // depending only on whether a picture arrives later — so a fit is legal
    // on a slide with no background, exactly as the background slot is legal
    // before the asset exists.
    show->slides[static_cast<size_t>(index)].backgroundFit = fit;
    return true;
}

const Asset* Documents::findAsset(const Id& assetId) const {
    const auto found = std::find_if(fAssets.begin(), fAssets.end(),
                                    [&](const Asset& a) { return a.id == assetId; });
    return found == fAssets.end() ? nullptr : &*found;
}

std::vector<Id> Documents::assetIds() const {
    std::vector<Id> ids;
    ids.reserve(fAssets.size());
    for (const Asset& asset : fAssets) {
        ids.push_back(asset.id);
    }
    return ids;
}

std::size_t Documents::adoptAssets(const std::vector<AssetRebind>& rebinds) {
    std::size_t adopted = 0;
    for (const AssetRebind& rebind : rebinds) {
        const auto found = std::find_if(fAssets.begin(), fAssets.end(),
                                        [&](const Asset& a) { return a.id == rebind.assetId; });
        if (found == fAssets.end() || found->sourcePath != rebind.from) {
            continue;  // removed, or already pointing somewhere newer
        }
        found->sourcePath = rebind.to;
        ++adopted;
    }
    return adopted;
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

Theme* Documents::findTheme_mut(const Id& themeId) {
    const auto found = std::find_if(fThemes.begin(), fThemes.end(),
                                    [&](const Theme& s) { return s.id == themeId; });
    return found == fThemes.end() ? nullptr : &*found;
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

int Documents::findItemIndex(const Slide& slide, const Id& itemId) {
    const auto found = std::find_if(slide.items.begin(), slide.items.end(),
                                    [&](const SlideItem& b) { return b.id == itemId; });
    return found == slide.items.end() ? -1
                                      : static_cast<int>(found - slide.items.begin());
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
