/*
 * Task 4.2 fixtures: LibrarySearch — the drawer's search contract, pinned
 * headless. The reference normalises both sides (lowercase, strip the
 * special-characters set and accents, collapse whitespace) and keeps a
 * show only when EVERY query word matches at a word start in the title
 * or the content, so adding words narrows and "here" never matches
 * "There's" (docs/reference.md §4, utils/search.ts).
 */

#include <cstdio>

#include <QString>

#include "LibrarySearch.h"

namespace {

bool gFailed = false;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        gFailed = true;
    }
    return condition;
}

using openshow::ui::LibraryRow;
namespace search = openshow::ui::librarysearch;

QList<LibraryRow> rows() {
    return {
        {QStringLiteral("1"), QStringLiteral("Amazing Grace"),
         QStringLiteral("Amazing grace how sweet the sound")},
        {QStringLiteral("2"), QStringLiteral("There Is a God"),
         QStringLiteral("Here I am with arms raised high")},
        {QStringLiteral("3"), QStringLiteral("Café Gräce"),
         QStringLiteral("Searching me, searching me, O Lord")},
        {QStringLiteral("4"), QStringLiteral("Here's Grace"), QStringLiteral("")},
    };
}

QStringList ids(const QList<LibraryRow>& list) {
    QStringList out;
    for (const LibraryRow& row : list) {
        out << row.id;
    }
    return out;
}

void exerciseNormalize() {
    check(search::normalize(QStringLiteral("  A   B  ")) == QStringLiteral("a b"),
          "whitespace collapses and edges trim");
    check(search::normalize(QStringLiteral("Gräce")) == QStringLiteral("grace"),
          "accents fall off after NFD");
    check(search::normalize(QStringLiteral("love, it's here")) == QStringLiteral("love its here"),
          "the special set vanishes (apostrophe too)");
    check(search::normalize(QStringLiteral("a-b")) == QStringLiteral("ab"),
          "hyphens glue the words around them");
}

void exerciseScoringTiers() {
    // 100: exact / leading title.
    check(search::score(QStringLiteral("Amazing Grace"), QStringLiteral("x"),
                        QStringLiteral("amazing grace")) == 100,
          "exact title scores 100");
    check(search::score(QStringLiteral("Amazing Grace"), QStringLiteral("x"),
                        QStringLiteral("amazing")) == 100,
          "a query the title starts with scores 100");
    // 90: every word at a title word start, not leading as a whole.
    check(search::score(QStringLiteral("Grace Upon Amazing Sinners"), QStringLiteral("x"),
                        QStringLiteral("amazing grace")) == 90,
          "all words in the title score 90");
    // 65: words split between title and content.
    check(search::score(QStringLiteral("Searching Me"),
                        QStringLiteral("amazing love"), QStringLiteral("amazing searching")) == 65,
          "split words score 65");
    // 50: content-only.
    check(search::score(QStringLiteral("O Lord"), QStringLiteral("when searching me"),
                        QStringLiteral("searching")) == 50,
          "content-only scores 50");
    // Refusals.
    check(search::score(QStringLiteral("Amazing Grace"), QStringLiteral("x"),
                        QStringLiteral("amazing disco")) == 0,
          "a word that never lands rejects the row");
    check(search::score(QStringLiteral("There Is a God"),
                        QStringLiteral("Here I am with arms raised high"),
                        QStringLiteral("ere")) == 0,
          "mid-word starts never match ('ere' is not a word start anywhere)");
    // The reference's own example, title side: "here" vs "There's" (the
    // apostrophe strip glues it into "theres", still no word start).
    check(search::score(QStringLiteral("There's"), QStringLiteral(""), QStringLiteral("here")) == 0,
          "'here' never matches \"There's\"");
    check(search::score(QStringLiteral("Here's"), QStringLiteral(""), QStringLiteral("here")) == 100,
          "\"Here's\" itself matches 'here'");
}

void exerciseFilter() {
    // No query: the full stored list, in order.
    check(ids(search::filter(rows(), QStringLiteral("   "))) == QStringList({"1", "2", "3", "4"}),
          "a blank query returns everything in stored order");

    // Case and accents normalise across the match; all three hits are
    // word-start title matches (90), so the ties keep stored order.
    check(ids(search::filter(rows(), QStringLiteral("GRACE"))) == QStringList({"1", "3", "4"}),
          "grace finds both plain titles and the accented one, in stored order");

    // Title matches rank above content-only ones, ties keep stored order.
    const QStringList found = ids(search::filter(rows(), QStringLiteral("searching me")));
    check(found == QStringList({"3"}), "content-only matches still get found");

    // Every word must land, and matches combine across title and content:
    // "grace" is row 1's title word and row 3's (accented) title word.
    const QStringList both = ids(search::filter(rows(), QStringLiteral("grace sweet")));
    check(both == QStringList({"1"}), "two words narrow to the row holding both");

    // A query nothing answers comes back empty.
    check(search::filter(rows(), QStringLiteral("disco fever")).isEmpty(),
          "no match, no rows");
}

}  // namespace

int main() {
    exerciseNormalize();
    exerciseScoringTiers();
    exerciseFilter();
    if (gFailed) {
        return 1;
    }
    std::printf("library_search_tests: the drawer search obeys the reference\n");
    return 0;
}
