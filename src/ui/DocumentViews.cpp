/*
 * DocumentViews implementation. Every row shape here is the one the
 * controller built before task 6.19b2, moved verbatim: the keys, their
 * spellings, the fallbacks for missing shows and assets, and the comments
 * explaining why each row looks the way it does all came across unchanged,
 * because the QML binds them and the acceptance for this extraction is
 * precisely that nothing observable moved.
 */

#include "DocumentViews.h"

#include <QStringList>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <system_error>

#include "LibrarySearch.h"

namespace openshow::ui {

QVariantMap transitionRecordMap(
    const std::optional<openshow::domain::TransitionOptions>& options) {
    QVariantMap record;
    if (!options.has_value()) {
        return record;  // silence, spelled as an empty map: there is no
                        // half-record default to invent
    }
    record.insert(QStringLiteral("kind"),
                  QString::fromLatin1(openshow::domain::transitionKindName(options->kind)));
    record.insert(QStringLiteral("textDurationMs"), options->textDurationMs);
    record.insert(QStringLiteral("mediaDurationMs"), options->mediaDurationMs);
    return record;
}

namespace {

// The slide's PRIMARY lyric box (amendment 2026-11-01/2026-11-02): the
// first TEXT item in draw order, the one a theme's placement drives and
// the one whose geometry writes record a placement CUSTOMIZATION. -1 when
// the slide is pictures alone.
int firstTextIndexOf(const openshow::domain::Slide& slide) {
    for (std::size_t i = 0; i < slide.items.size(); ++i) {
        if (slide.items[i].kind != openshow::domain::ItemKind::Image) {
            return static_cast<int>(i);
        }
    }
    return -1;  // a slide of pictures alone has no lyric box
}

// Which box of a slide the chain PLACES (themes plan amendment
// 2026-11-01): the primary lyric box, and only when some layer carries a
// rectangle. -1 means every box draws where it was authored. The list
// row, the canvas's outlines and its hit test all ask this one question,
// so the chrome, the audience's pixels and the inspector's numbers cannot
// become three different answers about where a box is.
int placedIndexOf(const openshow::domain::Documents& documents,
                  const openshow::domain::Id& showId,
                  const openshow::domain::Slide& slide) {
    if (!documents.appearanceForSlide(showId, slide.id).placement.has_value()) {
        return -1;
    }
    return firstTextIndexOf(slide);
}

}  // namespace

QString DocumentViews::linesAsText(const std::vector<std::string>& lines) {
    QStringList parts;
    parts.reserve(static_cast<int>(lines.size()));
    for (const std::string& line : lines) {
        parts.append(QString::fromStdString(line));
    }
    return parts.join(QLatin1Char('\n'));
}

const openshow::domain::Slide* DocumentViews::cursorSlide(const Cursor& cursor) const {
    const openshow::domain::Show* show = fDocuments.findShow(cursor.showId);
    if (show == nullptr || cursor.slideIndex < 0 ||
        static_cast<std::size_t>(cursor.slideIndex) >= show->slides.size()) {
        return nullptr;
    }
    return &show->slides[static_cast<std::size_t>(cursor.slideIndex)];
}

const openshow::domain::Slide* DocumentViews::findSlide(const openshow::domain::Id& showId,
                                                        const openshow::domain::Id& slideId) const {
    const openshow::domain::Show* show = fDocuments.findShow(showId);
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

QVariantList DocumentViews::songs() const {
    QVariantList list;
    for (const openshow::domain::Id& id : fDocuments.showIds()) {
        const openshow::domain::Show* show = fDocuments.findShow(id);
        if (show == nullptr) {
            continue;
        }
        // A schedule-owned presentation (task BI.3) is not a Library row:
        // the importer's item lives on today's schedule and nowhere else,
        // so the Library list — and with it the Show page — never sees it.
        // A catalog-linked song (task SL.6) is not a PRESENTATION either:
        // the Library's Songs branch reads the catalog, and this service
        // show is its internal projection, not an authored deck.
        if (show->scheduleOwned || !show->catalogItemId.empty() || show->song.has_value()) {
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(id));
        entry.insert(QStringLiteral("name"), QString::fromStdString(show->name));
        entry.insert(QStringLiteral("slideCount"), static_cast<int>(show->slides.size()));
        // The show default (task TH.5): the Show page's header selector
        // shows and sets this layer for the show it is browsing.
        entry.insert(QStringLiteral("defaultThemeId"),
                     QString::fromStdString(show->defaultThemeId));
        list.append(entry);
    }
    return list;
}

QVariantList DocumentViews::slides(const openshow::domain::Id& showId) const {
    QVariantList list;
    const openshow::domain::Show* show = fDocuments.findShow(showId);
    if (show == nullptr) {
        return list;
    }
    for (std::size_t i = 0; i < show->slides.size(); ++i) {
        const openshow::domain::Slide& slide = show->slides[i];
        // The row's position is the slide's FIRST box in draw order. The
        // canvas outlines EVERY box (slideBoxes) and its gestures
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
        entry.insert(QStringLiteral("sectionLabel"), QString::fromStdString(slide.group));
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
        // The slide's own theme layer and whether it has customized
        // anything (task TH.5): the cue surfaces and the tests read the
        // result of an apply or a restore straight off the row.
        entry.insert(QStringLiteral("themeId"), QString::fromStdString(slide.themeId));
        entry.insert(QStringLiteral("customized"), !slide.customizations.empty());
        list.append(entry);
    }
    return list;
}

QVariantList DocumentViews::slideBoxes(const Cursor& cursor) const {
    QVariantList list;
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    if (slide == nullptr) {
        return list;
    }
    // The canvas outlines and handles boxes where the AUDIENCE sees them
    // (amendments 2026-11-01 and 2026-11-02): the PRIMARY lyric box's row
    // carries the rectangle the chain resolves — its own placement
    // customization, else the placing theme's rectangle, else the authored
    // one — so the chrome, the number fields and the compile are one
    // answer. Since every move of that box IS a customization, the chrome
    // follows the pointer even on a slide no theme places; the authored
    // numbers stay underneath for the day the operator reverts.
    const int primary = firstTextIndexOf(*slide);
    const std::optional<openshow::domain::BoxRect> look =
        fDocuments.appearanceForSlide(cursor.showId, slide->id).placement;
    const std::vector<openshow::domain::SlideItem>& items = slide->items;
    for (std::size_t i = 0; i < items.size(); ++i) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(items[i].id));
        entry.insert(QStringLiteral("index"), static_cast<int>(i));
        // Kind travels with the box (task 6.17b): the canvas chrome can
        // already tell a picture's outline from a text box's without
        // guessing from empty words, and the 6.17b2 editor shows the
        // picture's asset here.
        entry.insert(QStringLiteral("kind"),
                     items[i].kind == openshow::domain::ItemKind::Image
                         ? QStringLiteral("image")
                         : QStringLiteral("text"));
        entry.insert(QStringLiteral("assetId"), QString::fromStdString(items[i].assetId));
        // The PRIMARY lyric box draws at the chain's rectangle whenever one
        // speaks (customization or theme); every other box — extra text,
        // pictures — draws where it was authored.
        const bool applied = look.has_value() && static_cast<int>(i) == primary;
        entry.insert(QStringLiteral("x"), applied ? look->x : items[i].x);
        entry.insert(QStringLiteral("y"), applied ? look->y : items[i].y);
        entry.insert(QStringLiteral("width"), applied ? look->width : items[i].width);
        entry.insert(QStringLiteral("height"), applied ? look->height : items[i].height);
        entry.insert(QStringLiteral("placed"), applied);
        // Which box is the slide's PRIMARY lyric box (amendment
        // 2026-11-02): geometry writes on THIS box record a placement
        // customization, wherever the chain happens to place it; every
        // other box keeps editing its authored rectangle.
        entry.insert(QStringLiteral("primary"),
                     static_cast<int>(i) == firstTextIndexOf(*slide));
        list.append(entry);
    }
    return list;
}

int DocumentViews::placedBoxIndex(const Cursor& cursor) const {
    // The cursor's slide through the same rule every other view uses: which
    // box (if any) the chain places. See placedIndexOf above.
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    if (slide == nullptr) {
        return -1;
    }
    return placedIndexOf(fDocuments, cursor.showId, *slide);
}

int DocumentViews::primaryBoxIndex(const Cursor& cursor) const {
    // The slide's PRIMARY lyric box — the first text item — whether or not
    // any layer places it (amendment 2026-11-02). The geometry write asks
    // this, so every move of the lyric box is revertible to the theme (or
    // to the authored rectangle) instead of silently rewriting the slide.
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    if (slide == nullptr) {
        return -1;
    }
    return firstTextIndexOf(*slide);
}

QString DocumentViews::backgroundFit(const Cursor& cursor) const {
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    return slide == nullptr
               ? QString()
               : QString::fromLatin1(
                     openshow::domain::backgroundFitName(slide->backgroundFit));
}

const openshow::domain::SlideItem* DocumentViews::selectedBox(const Cursor& cursor) const {
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    if (slide == nullptr) {
        return nullptr;
    }
    const std::vector<openshow::domain::SlideItem>& items = slide->items;
    if (items.empty()) {
        return nullptr;
    }
    for (const openshow::domain::SlideItem& box : items) {
        if (box.id == cursor.boxId) {
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

int DocumentViews::selectedBoxIndex(const Cursor& cursor) const {
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    if (slide == nullptr) {
        return -1;
    }
    const openshow::domain::SlideItem* box = selectedBox(cursor);
    if (box == nullptr) {
        return -1;
    }
    return static_cast<int>(box - slide->items.data());
}

QString DocumentViews::boxText(const openshow::domain::SlideItem* box) const {
    // The inspector edits the SELECTED box (task 6.17a2) — which is box 0
    // until the operator picks another, so a slide with one box and every
    // pre-6.17a file read the same as they always did.
    return box == nullptr ? QString() : linesAsText(box->lines);
}

QString DocumentViews::boxKind(const openshow::domain::SlideItem* box) const {
    if (box == nullptr) {
        return QString();
    }
    return box->kind == openshow::domain::ItemKind::Image ? QStringLiteral("image")
                                                          : QStringLiteral("text");
}

QVariantMap DocumentViews::boxAsset(const openshow::domain::SlideItem* box) const {
    QVariantMap map;
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

namespace {

// The TS.1 style fields on a theme row (task TS.1), written the placement
// row's way: a ...Set flag answers "does this theme speak?" and the value
// follows — the scalars at the contract's default when silent, the effect
// records as maps that are EMPTY when the theme carries none (a record has
// no honest zero to spell instead, and `enabled:false` is a REAL record
// the flag distinguishes from silence). Shared by the list row and the
// wearing-theme row so the two can never disagree about one theme.
void insertThemeStyleRow(QVariantMap& entry, const openshow::domain::Theme& theme) {
    entry.insert(QStringLiteral("fontWeightSet"), theme.fontWeight.has_value());
    entry.insert(QStringLiteral("fontWeight"),
                 theme.fontWeight.value_or(openshow::domain::kDefaultFontWeight));
    entry.insert(QStringLiteral("fontStyleSet"), theme.fontStyle.has_value());
    entry.insert(QStringLiteral("fontStyle"),
                 QString::fromStdString(theme.fontStyle.value_or("normal")));
    entry.insert(QStringLiteral("fitModeSet"), theme.fitMode.has_value());
    entry.insert(QStringLiteral("fitMode"),
                 QString::fromStdString(theme.fitMode.value_or("shrink")));
    entry.insert(QStringLiteral("minFontSizeSet"), theme.minFontSize.has_value());
    entry.insert(QStringLiteral("minFontSize"),
                 theme.minFontSize.value_or(openshow::domain::kDefaultMinFontSize));
    entry.insert(QStringLiteral("strokeSet"), theme.stroke.has_value());
    QVariantMap stroke;
    if (theme.stroke.has_value()) {
        stroke.insert(QStringLiteral("enabled"), theme.stroke->enabled);
        stroke.insert(QStringLiteral("color"), QString::fromStdString(theme.stroke->color));
        stroke.insert(QStringLiteral("width"), theme.stroke->width);
    }
    entry.insert(QStringLiteral("stroke"), stroke);
    entry.insert(QStringLiteral("shadowSet"), theme.shadow.has_value());
    QVariantMap shadow;
    if (theme.shadow.has_value()) {
        shadow.insert(QStringLiteral("enabled"), theme.shadow->enabled);
        shadow.insert(QStringLiteral("color"), QString::fromStdString(theme.shadow->color));
        shadow.insert(QStringLiteral("opacity"), theme.shadow->opacity);
        shadow.insert(QStringLiteral("offsetX"), theme.shadow->offsetX);
        shadow.insert(QStringLiteral("offsetY"), theme.shadow->offsetY);
        shadow.insert(QStringLiteral("blurSigma"), theme.shadow->blurSigma);
    }
    entry.insert(QStringLiteral("shadow"), shadow);
}

// The theme's OWN answer about how a wearing cue moves (task TR.3). The row
// carries the record in the shape `setThemeTransition` takes, plus the
// ...Set flag that separates "this theme says nothing" from a record that
// happens to match what it would inherit — the same distinction every other
// inspector field keeps, because an operator deciding whether to press
// Inherit needs to know whether there is anything of their own to take off.
void insertThemeTransitionRow(QVariantMap& entry, const openshow::domain::Theme& theme) {
    entry.insert(QStringLiteral("transitionSet"), theme.transition.has_value());
    entry.insert(QStringLiteral("transition"), transitionRecordMap(theme.transition));
}

}  // namespace

QVariantList DocumentViews::themes() const {
    QVariantList list;
    for (const openshow::domain::Id& id : fDocuments.themeIds()) {
        const openshow::domain::Theme* theme = fDocuments.findTheme(id);
        if (theme == nullptr) {
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(theme->id));
        entry.insert(QStringLiteral("name"), QString::fromStdString(theme->name));
        entry.insert(QStringLiteral("textColor"), QString::fromStdString(theme->textColor));
        entry.insert(QStringLiteral("backgroundColor"),
                     QString::fromStdString(theme->backgroundColor));
        // The gradient rides the list row (task TH.4) so the Theme Editor's
        // inspector and its keep-the-rest edits see every field of the row.
        entry.insert(QStringLiteral("backgroundGradient"),
                     QString::fromStdString(theme->backgroundGradient));
        entry.insert(QStringLiteral("fontFamily"), QString::fromStdString(theme->fontFamily));
        entry.insert(QStringLiteral("fontSize"), theme->fontSize);
        entry.insert(QStringLiteral("align"), QString::fromStdString(theme->align));
        entry.insert(QStringLiteral("lineSpacing"), theme->lineSpacing);
        // Placement (amendment 2026-11-01, themes plan): the rectangle this
        // theme gives a wearing slide's PRIMARY lyric box. placementSet is
        // the honest "no opinion" answer — the map is empty then, because a
        // rectangle has no honest zero to spell instead.
        entry.insert(QStringLiteral("placementSet"), theme->placement.has_value());
        QVariantMap rect;
        if (theme->placement.has_value()) {
            rect.insert(QStringLiteral("x"), theme->placement->x);
            rect.insert(QStringLiteral("y"), theme->placement->y);
            rect.insert(QStringLiteral("width"), theme->placement->width);
            rect.insert(QStringLiteral("height"), theme->placement->height);
        }
        entry.insert(QStringLiteral("placement"), rect);
        insertThemeStyleRow(entry, *theme);
        insertThemeTransitionRow(entry, *theme);
        // The seeded Default theme (amendment 2026-11-02): editable like
        // any theme, but the application will not delete it, and the
        // editor's Delete button says so before the domain refuses.
        entry.insert(QStringLiteral("builtin"),
                     theme->id == openshow::domain::Documents::kDefaultThemeId);
        list.append(entry);
    }
    return list;
}

QVariantMap DocumentViews::currentTheme(const Cursor& cursor) const {
    QVariantMap map;
    // The slide is resolved by the cursor's POSITION, exactly as the
    // controller's currentSlideId() did: navigation owns that index. The
    // theme the panel shows is the chain's top LIVE layer (task TH.3):
    // the service's current theme when one is worn, else the slide's own,
    // else the show's. The panel edits the theme entity, so it names the
    // entity the slide is actually wearing; field-level customizations
    // surface on top of this view when the inspector grows them (TH.5).
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    const openshow::domain::Theme* theme = nullptr;
    if (!fDocuments.currentThemeId().empty()) {
        theme = fDocuments.findTheme(fDocuments.currentThemeId());
    }
    if (theme == nullptr) {
        theme = fDocuments.themeForSlide(cursor.showId,
                                         slide == nullptr ? openshow::domain::Id{}
                                                          : slide->id);
    }
    if (theme == nullptr) {
        return map;
    }
    map.insert(QStringLiteral("id"), QString::fromStdString(theme->id));
    map.insert(QStringLiteral("name"), QString::fromStdString(theme->name));
    map.insert(QStringLiteral("textColor"), QString::fromStdString(theme->textColor));
    map.insert(QStringLiteral("backgroundColor"),
               QString::fromStdString(theme->backgroundColor));
    // "" means flat, and the UI's field shows itself empty for it (6.17c3).
    map.insert(QStringLiteral("backgroundGradient"),
               QString::fromStdString(theme->backgroundGradient));
    map.insert(QStringLiteral("fontFamily"), QString::fromStdString(theme->fontFamily));
    map.insert(QStringLiteral("fontSize"), theme->fontSize);
    map.insert(QStringLiteral("align"), QString::fromStdString(theme->align));
    map.insert(QStringLiteral("lineSpacing"), theme->lineSpacing);
    // The style block rides both rows (task TS.1), one helper keeping the
    // list and the wearing-theme view telling the same story about one
    // theme's weight, face, fit and effects.
    insertThemeStyleRow(map, *theme);
    // ... and so does the transition record (task TR.3): the theme the Show
    // page says is wearing a slide is the theme whose move a Go on that slide
    // reads, so the two rows must tell one story about it too.
    insertThemeTransitionRow(map, *theme);
    return map;
}

bool DocumentViews::themeIsOverride(const Cursor& cursor) const {
    // A slide's own theme is an override only while it is the chain's top
    // LIVE layer (task TH.3): a worn current theme speaks first, and a
    // panel showing the current theme must not claim the slide overrides.
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    return fDocuments.currentThemeId().empty() && slide != nullptr &&
           !slide->themeId.empty();
}

QVariantList DocumentViews::playlists() const {
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

QVariantList DocumentViews::collectionOutline() const {
    QVariantList list;
    QVariantMap library;
    library.insert(QStringLiteral("kind"), QStringLiteral("library"));
    library.insert(QStringLiteral("id"), QString());
    library.insert(QStringLiteral("name"), QStringLiteral("Library"));
    // The count is the Library's own (task BI.3): schedule-owned scripture
    // items live on their playlist rows, not in this bin.
    int libraryShows = 0;
    for (const openshow::domain::Id& id : fDocuments.showIds()) {
        const openshow::domain::Show* show = fDocuments.findShow(id);
        if (show != nullptr && !show->scheduleOwned && show->catalogItemId.empty()) {
            ++libraryShows;
        }
    }
    library.insert(QStringLiteral("count"), libraryShows);
    list.append(library);
    for (const QVariant& row : playlists()) {
        const QVariantMap playlist = row.toMap();
        QVariantMap outline;
        outline.insert(QStringLiteral("kind"), QStringLiteral("playlist"));
        outline.insert(QStringLiteral("id"), playlist.value(QStringLiteral("id")));
        outline.insert(QStringLiteral("name"), playlist.value(QStringLiteral("name")));
        outline.insert(QStringLiteral("count"), playlist.value(QStringLiteral("entryCount")));
        list.append(outline);
    }
    return list;
}

QVariantList DocumentViews::playlistEntries(const openshow::domain::Id& playlistId) const {
    QVariantList list;
    const openshow::domain::Playlist* playlist = fDocuments.findPlaylist(playlistId);
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

QVariantList DocumentViews::runningOrderCues(const openshow::domain::Id& playlistId) const {
    QVariantList list;
    const openshow::domain::Playlist* playlist = fDocuments.findPlaylist(playlistId);
    if (playlist == nullptr) {
        return list;
    }
    int index = 0;
    for (const openshow::domain::PlaylistEntry& row : playlist->entries) {
        const openshow::domain::Show* show = fDocuments.findShow(row.showId);
        QVariantMap group;
        group.insert(QStringLiteral("entryId"), QString::fromStdString(row.id));
        group.insert(QStringLiteral("showId"), QString::fromStdString(row.showId));
        group.insert(QStringLiteral("showName"),
                     show != nullptr ? QString::fromStdString(show->name) : QStringLiteral("?"));
        group.insert(QStringLiteral("section"), QString::fromStdString(row.section));
        group.insert(QStringLiteral("entryIndex"), index++);
        // The stale row (its show gone) reads as a cue with no slides — the
        // operator sees the broken cue rather than it quietly vanishing.
        group.insert(QStringLiteral("slides"), slides(row.showId));
        list.append(group);
    }
    return list;
}

QVariantList DocumentViews::assets() const {
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
        // The kind the bin filters and labels by (UI.4a), from the domain's
        // one name rule — the same answer the model refuses a video with and
        // the scene compiles a background with, so a row cannot promise a
        // thing the pipeline will not do.
        entry.insert(QStringLiteral("kind"),
                     QString::fromLatin1(openshow::domain::assetKindName(
                         openshow::domain::assetPathKind(asset->sourcePath))));
        // And whether the file is where that name says it is (task UI.4b).
        // One stat per row, no decoding: whether the BYTES play is the
        // worker's probe's answer and this view never presumes to give it —
        // it only declines to call a file that is plainly absent a cue the
        // operator can use. Renamed and deleted files are what this reports,
        // and it is the difference between "the service lost the file" and
        // "this build cannot use the file", which the bin says separately.
        std::error_code present;
        entry.insert(QStringLiteral("exists"),
                     !asset->sourcePath.empty()
                         && std::filesystem::exists(asset->sourcePath, present));
        list.append(entry);
    }
    return list;
}

QVariantList DocumentViews::searchShows(const QString& query) const {
    QList<LibraryRow> rows;
    for (const openshow::domain::Id& id : fDocuments.showIds()) {
        const openshow::domain::Show* show = fDocuments.findShow(id);
        if (show == nullptr) {
            continue;
        }
        // The drawer searches the PRESENTATIONS Library (tasks BI.3 and
        // SL.6); a schedule-owned scripture item is found where it lives,
        // and a catalog song is found by the catalog's own title filter.
        if (show->scheduleOwned || !show->catalogItemId.empty()) {
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

}  // namespace openshow::ui
