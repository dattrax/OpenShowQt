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

#include "LibrarySearch.h"

namespace openshow::ui {

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
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), QString::fromStdString(id));
        entry.insert(QStringLiteral("name"), QString::fromStdString(show->name));
        entry.insert(QStringLiteral("slideCount"), static_cast<int>(show->slides.size()));
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

QVariantList DocumentViews::slideBoxes(const Cursor& cursor) const {
    QVariantList list;
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    if (slide == nullptr) {
        return list;
    }
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
        entry.insert(QStringLiteral("x"), items[i].x);
        entry.insert(QStringLiteral("y"), items[i].y);
        entry.insert(QStringLiteral("width"), items[i].width);
        entry.insert(QStringLiteral("height"), items[i].height);
        list.append(entry);
    }
    return list;
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

QVariantList DocumentViews::styles() const {
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

QVariantMap DocumentViews::currentStyle(const Cursor& cursor) const {
    QVariantMap map;
    // The slide is resolved by the cursor's POSITION, exactly as the
    // controller's currentSlideId() did: navigation owns that index, and
    // styleForSlide takes the id it names.
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    const openshow::domain::Style* style = fDocuments.styleForSlide(
        cursor.showId, slide == nullptr ? openshow::domain::Id{} : slide->id);
    if (style == nullptr) {
        return map;
    }
    map.insert(QStringLiteral("id"), QString::fromStdString(style->id));
    map.insert(QStringLiteral("name"), QString::fromStdString(style->name));
    map.insert(QStringLiteral("textColor"), QString::fromStdString(style->textColor));
    map.insert(QStringLiteral("backgroundColor"),
               QString::fromStdString(style->backgroundColor));
    // "" means flat, and the UI's field shows itself empty for it (6.17c3).
    map.insert(QStringLiteral("backgroundGradient"),
               QString::fromStdString(style->backgroundGradient));
    map.insert(QStringLiteral("fontFamily"), QString::fromStdString(style->fontFamily));
    map.insert(QStringLiteral("fontSize"), style->fontSize);
    map.insert(QStringLiteral("align"), QString::fromStdString(style->align));
    map.insert(QStringLiteral("lineSpacing"), style->lineSpacing);
    return map;
}

bool DocumentViews::styleIsOverride(const Cursor& cursor) const {
    const openshow::domain::Slide* slide = cursorSlide(cursor);
    return slide != nullptr && !slide->styleId.empty();
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
    library.insert(QStringLiteral("count"),
                   static_cast<int>(fDocuments.showIds().size()));
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
