#include "CatalogController.h"

#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <utility>

#include "LibrarySearch.h"
#include "openshow/catalog/catalogstorage.h"

namespace openshow::ui {

namespace {

QString fromStd(const std::string& text) { return QString::fromStdString(text); }
std::string toStd(const QString& text) { return text.toStdString(); }

// Every query word must match at the start of a word in the (already
// normalized) title. The same rule LibrarySearch uses, applied to the
// cached titles so a keystroke does no normalization of its own.
bool matchesNormalized(const QString& haystack, const QStringList& tokens) {
    if (tokens.isEmpty()) {
        return true;
    }
    for (const QString& token : tokens) {
        bool found = false;
        int from = haystack.indexOf(token);
        while (from >= 0) {
            if (from == 0 || haystack.at(from - 1) == QLatin1Char(' ')) {
                found = true;
                break;
            }
            from = haystack.indexOf(token, from + 1);
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

QVariantMap itemRow(const openshow::catalog::CatalogItem& item) {
    QVariantMap row;
    row.insert(QStringLiteral("id"), fromStd(item.id));
    row.insert(QStringLiteral("category"),
               QString::fromLatin1(
                   openshow::catalog::categoryName(item.category)));
    row.insert(QStringLiteral("title"), fromStd(item.title));
    row.insert(QStringLiteral("kind"),
               QString::fromLatin1(
                   openshow::catalog::contentKindName(item.kind)));
    row.insert(QStringLiteral("recordRevision"),
               static_cast<qulonglong>(item.recordRevision));
    row.insert(QStringLiteral("contentRevision"),
               static_cast<qulonglong>(item.contentRevision));
    row.insert(QStringLiteral("ccli"), fromStd(item.metadata.ccli));
    row.insert(QStringLiteral("authors"), fromStd(item.metadata.authors));
    row.insert(QStringLiteral("copyright"), fromStd(item.metadata.copyright));
    row.insert(QStringLiteral("stanzaCount"),
               static_cast<int>(item.stanzas.size()));
    int lines = 0;
    for (const openshow::catalog::Stanza& stanza : item.stanzas) {
        lines += static_cast<int>(stanza.lines.size());
    }
    row.insert(QStringLiteral("lineCount"), lines);
    return row;
}

}  // namespace

CatalogController::CatalogController(QObject* parent, std::filesystem::path baseDirectory)
    : QObject(parent) {
    if (baseDirectory.empty()) {
        baseDirectory = std::filesystem::path(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                .toStdString());
    }
    fDirectory = baseDirectory / "catalog";
    fHistory = std::make_unique<openshow::catalog::History>(&fCatalog);

    std::error_code ec;
    if (std::filesystem::is_regular_file(fDirectory / "catalog.json", ec) && !ec) {
        std::string error;
        if (!openshow::catalog::loadCatalog(&fCatalog, fDirectory, &error)) {
            // Never pretend an unreadable Library is empty and then
            // overwrite it on the next click. Reads answer empty and every
            // mutation refuses, naming the file, until a person fixes it.
            fStatus = QStringLiteral("The Library could not be read: ") +
                      fromStd(error);
            fWriteBlocked = true;
        }
    }
    // The history starts empty even when a catalog was loaded (the stacks
    // themselves need not survive a restart; the durable records do).
    fHistory->clear();
    rebuildRows();
}

int CatalogController::songCount() const {
    return static_cast<int>(
        fCatalog.count(openshow::catalog::Category::Songs));
}

int CatalogController::presentationCount() const {
    return static_cast<int>(
        fCatalog.count(openshow::catalog::Category::Presentations));
}

QString CatalogController::directory() const {
    return QString::fromStdWString(fDirectory.wstring());
}

QString CatalogController::undoLabel() const {
    return QStringLiteral("Undo Library edit");
}

QString CatalogController::redoLabel() const {
    return QStringLiteral("Redo Library edit");
}

const openshow::catalog::CatalogItem* CatalogController::findItem(
    const QString& id) const {
    return fCatalog.find(toStd(id));
}

QVariantList CatalogController::searchTitles(const QString& query) const {
    QVariantList result;
    const QStringList tokens = librarysearch::normalize(query).split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < fItems.size(); ++i) {
        if (matchesNormalized(fNormalizedTitles.value(i), tokens)) {
            result.append(fItems.at(i));
        }
    }
    return result;
}

QVariantList CatalogController::stanzasOf(const QString& id) const {
    const openshow::catalog::CatalogItem* item = findItem(id);
    return item == nullptr ? QVariantList() : stanzasToVariant(item->stanzas);
}

QVariantMap CatalogController::itemOf(const QString& id) const {
    const openshow::catalog::CatalogItem* item = findItem(id);
    return item == nullptr ? QVariantMap() : itemRow(*item);
}

QString CatalogController::addSong(const QString& title, const QVariantList& stanzas,
                                   const QString& ccli, const QString& authors,
                                   const QString& copyright) {
    std::string conversionError;
    const std::vector<openshow::catalog::Stanza> rows =
        stanzasFromVariant(stanzas, &conversionError);
    if (!conversionError.empty()) {
        setStatus(fromStd(conversionError));
        return QString();
    }
    openshow::catalog::SongMetadata metadata;
    metadata.ccli = toStd(ccli);
    metadata.authors = toStd(authors);
    metadata.copyright = toStd(copyright);

    QString newId;
    const bool ok = commit(
        [&](openshow::catalog::Catalog& catalog, std::string& error) {
            newId = fromStd(catalog.createSong(toStd(title), rows, metadata, &error));
            return !newId.isEmpty();
        },
        QStringLiteral("Could not add the song to the Library"));
    return ok ? newId : QString();
}

bool CatalogController::updateSong(const QString& id, const QVariantList& stanzas,
                                   const QString& ccli, const QString& authors,
                                   const QString& copyright) {
    std::string conversionError;
    const std::vector<openshow::catalog::Stanza> rows =
        stanzasFromVariant(stanzas, &conversionError);
    if (!conversionError.empty()) {
        setStatus(fromStd(conversionError));
        return false;
    }
    openshow::catalog::SongMetadata metadata;
    metadata.ccli = toStd(ccli);
    metadata.authors = toStd(authors);
    metadata.copyright = toStd(copyright);
    return commit(
        [&](openshow::catalog::Catalog& catalog, std::string& error) {
            return catalog.updateSong(toStd(id), rows, metadata, &error);
        },
        QStringLiteral("Could not update the song"));
}

bool CatalogController::renameItem(const QString& id, const QString& title) {
    return commit(
        [&](openshow::catalog::Catalog& catalog, std::string& error) {
            return catalog.rename(toStd(id), toStd(title), &error);
        },
        QStringLiteral("Could not rename the Library item"));
}

bool CatalogController::setItemMetadata(const QString& id, const QString& ccli,
                                        const QString& authors,
                                        const QString& copyright) {
    openshow::catalog::SongMetadata metadata;
    metadata.ccli = toStd(ccli);
    metadata.authors = toStd(authors);
    metadata.copyright = toStd(copyright);
    return commit(
        [&](openshow::catalog::Catalog& catalog, std::string& error) {
            return catalog.setMetadata(toStd(id), metadata, &error);
        },
        QStringLiteral("Could not update the song's attribution"));
}

bool CatalogController::moveItemToCategory(const QString& id, const QString& category) {
    openshow::catalog::Category parsed;
    if (!openshow::catalog::categoryFromName(toStd(category), &parsed)) {
        setStatus(QStringLiteral("Unknown Library category \u201c") + category +
                  QStringLiteral("\u201d"));
        return false;
    }
    return commit(
        [&](openshow::catalog::Catalog& catalog, std::string& error) {
            return catalog.moveToCategory(toStd(id), parsed, &error);
        },
        QStringLiteral("Could not move the Library item"));
}

bool CatalogController::removeItem(const QString& id) {
    return commit(
        [&](openshow::catalog::Catalog& catalog, std::string& error) {
            return catalog.remove(toStd(id), &error);
        },
        QStringLiteral("Could not remove the Library item"));
}

void CatalogController::undo() {
    if (fWriteBlocked) {
        setStatus(QStringLiteral("The Library could not be read; undo is "
                                 "unavailable until it is fixed"));
        return;
    }
    std::string error;
    if (!fHistory->undo(&error)) {
        setStatus(fromStd(error));
        return;
    }
    if (!openshow::catalog::saveCatalog(fCatalog, fDirectory, &error)) {
        // The durable file still holds the post-undo state, so put the
        // in-memory state back to match it rather than diverge.
        std::string ignored;
        fHistory->redo(&ignored);
        setStatus(QStringLiteral("Could not save the undone Library: ") +
                  fromStd(error));
        return;
    }
    rebuildRows();
    setStatus(QString());
    emit itemsChanged();
    emit stateChanged();
}

void CatalogController::redo() {
    if (fWriteBlocked) {
        setStatus(QStringLiteral("The Library could not be read; redo is "
                                 "unavailable until it is fixed"));
        return;
    }
    std::string error;
    if (!fHistory->redo(&error)) {
        setStatus(fromStd(error));
        return;
    }
    if (!openshow::catalog::saveCatalog(fCatalog, fDirectory, &error)) {
        std::string ignored;
        fHistory->undo(&ignored);
        setStatus(QStringLiteral("Could not save the redone Library: ") +
                  fromStd(error));
        return;
    }
    rebuildRows();
    setStatus(QString());
    emit itemsChanged();
    emit stateChanged();
}

bool CatalogController::commit(
    const std::function<bool(openshow::catalog::Catalog&, std::string&)>& edit,
    const QString& failurePrefix) {
    if (fWriteBlocked) {
        setStatus(QStringLiteral("The Library could not be read: ") + fStatus +
                  QStringLiteral(". Fix or move ") + directory() +
                  QStringLiteral(" before editing."));
        return false;
    }
    openshow::catalog::Catalog trial = fCatalog;
    std::string error;
    if (!edit(trial, error)) {
        setStatus(fromStd(error));
        return false;
    }
    if (trial == fCatalog) {
        return true;  // a no-op verb: no write, no undo step
    }
    if (!openshow::catalog::saveCatalog(trial, fDirectory, &error)) {
        setStatus(failurePrefix + QStringLiteral(": ") + fromStd(error));
        return false;
    }
    openshow::catalog::Catalog before = fCatalog;
    fCatalog = std::move(trial);
    fHistory->recordApplied(std::move(before));
    rebuildRows();
    setStatus(QString());
    emit itemsChanged();
    emit stateChanged();
    return true;
}

void CatalogController::rebuildRows() {
    fItems.clear();
    fNormalizedTitles.clear();
    fIdsInOrder.clear();
    for (const openshow::catalog::Id& id : fCatalog.itemIds()) {
        const openshow::catalog::CatalogItem* item = fCatalog.find(id);
        if (item == nullptr) {
            continue;
        }
        fItems.append(itemRow(*item));
        fIdsInOrder.push_back(fromStd(id));
        fNormalizedTitles.append(librarysearch::normalize(fromStd(item->title)));
    }
}

void CatalogController::setStatus(const QString& status) {
    if (fStatus == status) {
        return;
    }
    fStatus = status;
    emit stateChanged();
}

std::vector<openshow::catalog::Stanza> CatalogController::stanzasFromVariant(
    const QVariantList& rows, std::string* error) {
    std::vector<openshow::catalog::Stanza> stanzas;
    for (const QVariant& row : rows) {
        const QVariantMap map = row.toMap();
        if (map.isEmpty()) {
            *error = "a stanza row is not an object";
            return {};
        }
        openshow::catalog::Stanza stanza;
        stanza.label = toStd(map.value(QStringLiteral("label")).toString());
        const QVariantList lines = map.value(QStringLiteral("lines")).toList();
        for (const QVariant& line : lines) {
            stanza.lines.push_back(toStd(line.toString()));
        }
        stanzas.push_back(std::move(stanza));
    }
    return stanzas;
}

QVariantList CatalogController::stanzasToVariant(
    const std::vector<openshow::catalog::Stanza>& stanzas) {
    QVariantList rows;
    for (const openshow::catalog::Stanza& stanza : stanzas) {
        QVariantMap row;
        row.insert(QStringLiteral("label"), fromStd(stanza.label));
        QStringList lines;
        for (const std::string& line : stanza.lines) {
            lines.append(fromStd(line));
        }
        row.insert(QStringLiteral("lines"), lines);
        rows.append(row);
    }
    return rows;
}

}  // namespace openshow::ui
