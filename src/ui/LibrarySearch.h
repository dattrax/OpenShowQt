/*
 * The library drawer's search (M4 task 4.2).
 *
 * The reference normalises both sides before matching (`formatSearch`:
 * lowercase, strip [.,/#!?$%^&*;:{}=-_'"´`~()], strip accents, collapse
 * whitespace) and then keeps a show only when EVERY query word matches
 * at the start of a word in the title or the content, so adding words
 * always narrows the results — the spec's example: "here" must never
 * match "There's" (utils/search.ts, behaviour verified live). Matches
 * carry a 0–100 confidence and the list is sorted by it (stable, ties
 * in stored order).
 *
 * This reproduces that contract with the reference's simpler score tiers
 * — exact or leading title 100, every word in the title 90, words split
 * between title and content 65, content-only 50. The reference's extra
 * refinements (quoted phrases, fuzzy typo tolerance, song-number and
 * CCLI exactness) lean on fields OpenShowQt does not carry yet and are
 * deliberately left out.
 *
 * Pure Qt Core so the fixtures pin the boundaries with no window.
 */
#pragma once

#include <QString>
#include <QStringList>

namespace openshow::ui {

// One searchable library item: a show's name plus its content (all
// slide lines), which the reference also searches.
struct LibraryRow {
    QString id;
    QString name;
    QString content;
};

namespace librarysearch {

// formatSearch(): the reference's normalisation for matching text.
QString normalize(const QString& text);

// The rows matching query, in the reference's order (best match first,
// stable within a score). An empty or whitespace-only query returns the
// rows untouched, exactly as the drawer shows the full list then.
QList<LibraryRow> filter(const QList<LibraryRow>& rows, const QString& query);

// The drawer shows the score as a confidence bar; the fixtures use it
// directly for the tiers above. The query is normalised and tokenised
// here, so callers can score a row without any setup.
int score(const QString& name, const QString& content, const QString& query);

}  // namespace librarysearch
}  // namespace openshow::ui
