/*
 * CatalogController: the shared Library behind the workspace (tasks SL.2,
 * SL.5).
 *
 * The Qt read/write half of openshow::catalog. It owns the catalog, its
 * independent History and its durable package under the application's data
 * directory (never inside a service), and exposes rows and explicit
 * mutations to QML. It never touches the worker's live state and never the
 * current service's documents: those stay DocumentController's.
 *
 * Durability rule: an edit is applied to a COPY, written to disk, and only
 * then published to the model and its history. A refused write leaves the
 * rows, the undo stacks and the file exactly as they were, with the
 * failure named in status(). A successful mutation is immediately durable,
 * so a restart shows exactly what the operator last saw (the plan's
 * "restart and New/Load service leave catalog intact").
 *
 * Title filtering (SL.5) filters titles only, using normalized titles
 * cached at publish time: a keystroke never rebuilds lyrics or revisits
 * every record's words. Lyrics-only hits therefore cannot appear.
 *
 * Qt Core only, like DocumentController, so headless fixtures drive the
 * very object the QML binds to.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "openshow/catalog/catalog.h"
#include "openshow/catalog/history.h"

namespace openshow::ui {

class CatalogController : public QObject {
    Q_OBJECT
    // Every item row: [{id, category, title, kind, recordRevision,
    // contentRevision, ccli, authors, copyright, stanzaCount, lineCount}].
    // Creation order; both categories are in this one list so the Library
    // branches can be built without a second model.
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(int songCount READ songCount NOTIFY itemsChanged)
    Q_PROPERTY(int presentationCount READ presentationCount NOTIFY itemsChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    // The last durable-write/load note worth showing, "" when quiet.
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    // Where the catalog lives, for Settings/diagnostics.
    Q_PROPERTY(QString directory READ directory CONSTANT)
    // The one undo name the Library offers ("Undo Library edit"), and the
    // matching redo, so the host can label buttons without re-deriving
    // which store will move.
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY stateChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY stateChanged)

public:
    // baseDirectory defaults to the per-user application data location;
    // tests pass a scratch directory. A missing catalog begins empty.
    explicit CatalogController(QObject* parent = nullptr,
                               std::filesystem::path baseDirectory = {});

    QVariantList items() const { return fItems; }
    int songCount() const;
    int presentationCount() const;
    bool canUndo() const { return fHistory->canUndo(); }
    bool canRedo() const { return fHistory->canRedo(); }
    QString status() const { return fStatus; }
    QString directory() const;
    QString undoLabel() const;
    QString redoLabel() const;

    // The catalog model itself, for the service bridge and the import
    // session (both C++ neighbours). Never exposed to QML as a pointer.
    const openshow::catalog::Catalog& catalog() const { return fCatalog; }
    const openshow::catalog::CatalogItem* findItem(const QString& id) const;

    // --- rows and filtering ---

    // The rows whose TITLES match every word of the query (case/accent/
    // whitespace normalized at word starts, the LibrarySearch rule). A
    // blank query returns the whole catalog. Lyrics are never searched.
    Q_INVOKABLE QVariantList searchTitles(const QString& query) const;
    // One item's canonical stanzas as [{label, lines}] — the bridge and the
    // import session read these; the UI never edits slides from here.
    Q_INVOKABLE QVariantList stanzasOf(const QString& id) const;
    Q_INVOKABLE QVariantMap itemOf(const QString& id) const;

    // --- mutations (each one durable before it is published) ---

    // Adds a Songs item. `stanzas` is [{label, lines}] where lines is a
    // list of strings. Returns the new id, or "" with status naming why.
    Q_INVOKABLE QString addSong(const QString& title, const QVariantList& stanzas,
                                const QString& ccli, const QString& authors,
                                const QString& copyright);
    // Replaces a song's stanzas and metadata (a corrected lyric sheet).
    Q_INVOKABLE bool updateSong(const QString& id, const QVariantList& stanzas,
                                const QString& ccli, const QString& authors,
                                const QString& copyright);
    Q_INVOKABLE bool renameItem(const QString& id, const QString& title);
    Q_INVOKABLE bool setItemMetadata(const QString& id, const QString& ccli,
                                     const QString& authors, const QString& copyright);
    // Category moves preserve content; the drag/drop leaf and the keyboard
    // alternative both land here.
    Q_INVOKABLE bool moveItemToCategory(const QString& id, const QString& category);
    Q_INVOKABLE bool removeItem(const QString& id);
    Q_INVOKABLE bool removeItems(const QStringList& ids, bool permanent = false);

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

signals:
    void itemsChanged();
    void stateChanged();
    // One item changed durably. The service bridge listens and re-prepares
    // any linked running-order show, which is how an edit in the Library
    // reaches the service (operator amendment: rows are references).
    void itemChanged(const QString& id);
    // One item was deleted. Linked service shows detach (keep playing with
    // their copied attribution) rather than pointing at a dead record.
    void itemRemoved(const QString& id);
    void itemsPermanentlyRemoved(const QStringList& ids);

private:
    // Applies `edit` to a copy, writes it, then publishes model + history.
    // This is the ONE durable-write door; every mutation above runs it.
    bool commit(const std::function<bool(openshow::catalog::Catalog&, std::string&)>& edit,
                const QString& failurePrefix);

    void rebuildRows();
    void setStatus(const QString& status);
    static std::vector<openshow::catalog::Stanza> stanzasFromVariant(
        const QVariantList& rows, std::string* error);
    static QVariantList stanzasToVariant(const std::vector<openshow::catalog::Stanza>& stanzas);

    openshow::catalog::Catalog fCatalog;
    std::unique_ptr<openshow::catalog::History> fHistory;
    std::filesystem::path fDirectory;
    QVariantList fItems;
    // Cached normalized titles, parallel to the catalog's item order. One
    // normalize per item per publish, never per keystroke.
    QStringList fNormalizedTitles;
    std::vector<QString> fIdsInOrder;
    QString fStatus;
    // True when a catalog.json existed but could not be read. Mutations then
    // refuse rather than overwrite a file the operator may still want.
    bool fWriteBlocked = false;
};

}  // namespace openshow::ui
