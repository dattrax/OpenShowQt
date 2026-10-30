/*
 * DocumentViews: the row shapes the operator's lists read (task 6.19b2).
 *
 * DocumentController owns the editable documents, the navigation into them
 * and every mutation. This class owns the SHAPE of what the views display:
 * the song rows, a song's slide rows, the current slide's box rows, the
 * style list and the current slide's resolved style, the playlists and
 * their entries with each referenced show resolved, the media registry,
 * the library search, and the current box's kind and asset. A row's keys
 * have exactly one author here, so the running order, the drawer, the
 * slide list, the canvas chrome and the inspector cannot drift toward
 * five subtly different spellings of the same fact.
 *
 * It is a reader, not a second model: it holds a const reference to the
 * controller's Documents, keeps no state of its own, emits nothing and
 * caches nothing — every call walks the model as it is right now. Where a
 * shape depends on WHERE the operator is, the navigation arrives as a
 * Cursor the caller passes in; the controller builds that cursor from the
 * ids it owns, and nothing else may, which is what keeps "the box wearing
 * the canvas's handles is the box the inspector edits" a property of the
 * code rather than a hope: selectedBox() is the ONE resolution of the
 * selection with its positional fallback, and the controller's own
 * box-scoped edits resolve through it too.
 *
 * Qt Core only — no GUI — like the controller itself, so the headless
 * fixtures drive the very rows the panels bind to.
 */

#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "openshow/domain/documents.h"

namespace openshow::ui {

class DocumentViews {
public:
    // Where the operator is, as a value: the controller builds this from the
    // ids it owns and passes it to the location-shaped reads below.
    struct Cursor {
        openshow::domain::Id showId;      // the current song
        int slideIndex = -1;              // index into that song's slides
        openshow::domain::Id boxId;       // selected box; empty resolves to box 0
        openshow::domain::Id playlistId;  // the current playlist
    };

    explicit DocumentViews(const openshow::domain::Documents& documents)
        : fDocuments(documents) {}

    // --- shows and slides ---
    QVariantList songs() const;
    // The rows of showId's slides: position, first-line preview and the
    // first box's geometry (see the comment in slides() for why the FIRST
    // box represents a slide in a list), plus the background asset and fit.
    QVariantList slides(const openshow::domain::Id& showId) const;
    // Every box of cursor's slide, in draw order: id, index, kind, asset
    // and geometry. The canvas outlines these rows and addresses its
    // gestures by index.
    QVariantList slideBoxes(const Cursor& cursor) const;
    // The slide named by slideId inside showId, or nullptr. Callers re-check
    // rather than trusting a stale index across a slide removal.
    const openshow::domain::Slide* findSlide(const openshow::domain::Id& showId,
                                             const openshow::domain::Id& slideId) const;
    // The background fit of cursor's slide ("" when there is no slide).
    QString backgroundFit(const Cursor& cursor) const;

    // --- the selected box (R17: one resolution, one fallback) ---
    // The box cursor selects: the one named by boxId, or, when that box is
    // gone or nothing was ever picked, the bottom of the stack — the box a
    // pre-6.17a slide had alone, and the box the canvas puts its handles
    // on. nullptr only when there is no slide or the slide has no boxes.
    // The controller's box-scoped edits resolve through this same call,
    // which is what keeps the highlighted box and the edited box identical.
    const openshow::domain::SlideItem* selectedBox(const Cursor& cursor) const;
    // The selected box's position in draw order, or -1.
    int selectedBoxIndex(const Cursor& cursor) const;
    // What the inspector reads off the selected box.
    QString boxText(const openshow::domain::SlideItem* box) const;
    QString boxKind(const openshow::domain::SlideItem* box) const;
    // id/name/sourcePath when the box is a picture with a registry entry;
    // empty otherwise, which is the inspector's "this picture has lost its
    // file" case, never the naming of a stranger.
    QVariantMap boxAsset(const openshow::domain::SlideItem* box) const;
    // The one '\n'-joined text a std::vector<std::string> of lines reads
    // as. The controller compares its debounced draft against this, so the
    // inspector's text and that comparison cannot disagree.
    static QString linesAsText(const std::vector<std::string>& lines);

    // --- styles ---
    QVariantList styles() const;
    // The style cursor's slide actually renders with, resolved through the
    // song and the default, plus the gradient the flat colours hide.
    QVariantMap currentStyle(const Cursor& cursor) const;
    // True when cursor's slide names a style of its own rather than
    // inheriting.
    bool styleIsOverride(const Cursor& cursor) const;

    // --- playlists, media, search ---
    QVariantList playlists() const;
    // cursor's playlist with each entry's show resolved (name and slide
    // count, or "?" and 0 for a row pointing at nothing).
    QVariantList playlistEntries(const openshow::domain::Id& playlistId) const;
    QVariantList assets() const;
    // The drawer's search: the reference's normalise-and-every-word-matches
    // filter over show name and all box lines, projected to the drawer's
    // rows (id, name, slideCount).
    QVariantList searchShows(const QString& query) const;

private:
    // The slide the cursor's index points at, or nullptr. Kept inside the
    // two reads that need the item vector so their bounds rule is one line.
    const openshow::domain::Slide* cursorSlide(const Cursor& cursor) const;

    const openshow::domain::Documents& fDocuments;
};

}  // namespace openshow::ui
