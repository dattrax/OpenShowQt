#include "LibrarySearch.h"

#include <algorithm>

namespace openshow::ui::librarysearch {

namespace {

// The reference's specialChars set (utils/search.ts): these characters
// simply disappear before matching, which also glues the words around
// them ("a-b" becomes "ab").
bool stripped(QChar c) {
    static const QString kChars =
        QStringLiteral(".,/#!?$%^&*;:{}=-_'\"\u00b4`~()");
    return kChars.contains(c);
}

QString despaced(const QString& normalized) {
    QString out;
    out.reserve(normalized.size());
    for (const QChar c : normalized) {
        if (!c.isSpace()) {
            out.push_back(c);
        }
    }
    return out;
}

// A needle counts only at the start of a word — the contract that keeps
// "here" out of "theres" once apostrophes have been stripped.
bool matchesAtWordStarts(const QString& haystack, const QString& needle) {
    int from = haystack.indexOf(needle);
    while (from >= 0) {
        if (from == 0 || haystack.at(from - 1) == QLatin1Char(' ')) {
            return true;
        }
        from = haystack.indexOf(needle, from + 1);
    }
    return false;
}

}  // namespace

QString normalize(const QString& text) {
    // formatSearch(): lowercase, strip specials, NFD + drop diacritics,
    // collapse whitespace runs to single spaces.
    QString lowered = text.toLower();

    QString strippedOut;
    strippedOut.reserve(lowered.size());
    for (const QChar c : lowered) {
        if (c.isSpace()) {
            strippedOut.push_back(QLatin1Char(' '));  // any whitespace becomes a space
        } else if (!stripped(c)) {
            strippedOut.push_back(c);
        }
    }

    QString out;
    out.reserve(strippedOut.size());
    bool pendingSpace = false;
    bool lastWasSpace = true;  // trims a leading run, like the token list would anyway
    for (const QChar c : strippedOut.normalized(QString::NormalizationForm_D)) {
        if (c.isSpace()) {
            pendingSpace = true;
            continue;
        }
        if (c.isMark()) {
            continue;  // the combining half of an accented letter after NFD
        }
        if (pendingSpace && !lastWasSpace) {
            out.push_back(QLatin1Char(' '));
        }
        pendingSpace = false;
        lastWasSpace = false;
        out.push_back(c);
    }
    return out;
}

int score(const QString& name, const QString& content, const QString& query) {
    const QStringList tokens =
        normalize(query).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        return 0;
    }
    const QString nName = normalize(name);
    if (nName.isEmpty()) {
        return 0;  // the reference refuses nameless shows outright
    }
    const QString nContent = normalize(content);

    bool allInTitle = true;
    bool anyInTitle = false;
    for (const QString& token : tokens) {
        const bool inTitle = matchesAtWordStarts(nName, token);
        if (!inTitle && !matchesAtWordStarts(nContent, token)) {
            return 0;  // every word must land, title or content
        }
        anyInTitle = anyInTitle || inTitle;
        allInTitle = allInTitle && inTitle;
    }

    const QString despacedQuery = despaced(tokens.join(QLatin1Char(' ')));
    const QString despacedName = despaced(nName);
    if (despacedName == despacedQuery || despacedName.startsWith(despacedQuery)) {
        return 100;  // exact or leading title
    }
    if (allInTitle) {
        return 90;  // every word at a title word-start
    }
    if (anyInTitle) {
        return 65;  // words split between title and content
    }
    return 50;  // content-only
}

QList<LibraryRow> filter(const QList<LibraryRow>& rows, const QString& query) {
    if (query.trimmed().isEmpty()) {
        return rows;  // the drawer shows the full stored list with no query
    }
    QList<LibraryRow> matched;
    QList<int> scores;
    matched.reserve(rows.size());
    scores.reserve(rows.size());
    for (const LibraryRow& row : rows) {
        const int rowScore = score(row.name, row.content, query);
        if (rowScore > 0) {
            matched.push_back(row);
            scores.push_back(rowScore);
        }
    }
    // Stable, so equal-confidence rows keep their stored order — the
    // reference sorts the same way.
    QList<std::pair<int, int>> order;  // (-score, index)
    order.reserve(matched.size());
    for (int i = 0; i < matched.size(); ++i) {
        order.append({-scores.at(i), i});
    }
    std::stable_sort(order.begin(), order.end());
    QList<LibraryRow> out;
    out.reserve(matched.size());
    for (const auto& [negativeScore, index] : order) {
        out.push_back(matched.at(index));
    }
    return out;
}

}  // namespace openshow::ui::librarysearch
