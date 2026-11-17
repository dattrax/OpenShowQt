/*
 * ServiceCatalogBridge: prepare a shared-catalog song into the current
 * service (tasks SL.6/SL.8, operator amendment 2026-11-06).
 *
 * The running order REFERENCES a catalog song: adding one creates (or reuses)
 * ONE service show carrying the catalog id, lays that show out from the
 * catalog's canonical stanzas against the service's current Songs theme, and
 * adds a running-order entry. Adding the same song twice adds a second entry
 * over the same linked show, so an edit to the Library item updates every
 * occurrence at once.
 *
 * The catalog is the source of truth while the link stands. A rename or a
 * lyric/metadata correction in the Library re-prepares every linked show in
 * one service undo step; deleting the catalog item detaches the link and the
 * service copy stays playable — the DocumentController keeps its attribution.
 *
 * Qt Core only. Measurement is the shared SongPlanner; nothing here presents
 * or writes the slide document without a completed plan.
 */

#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "openshow/catalog/catalog.h"
#include "SongPlanner.h"

namespace openshow::ui {

class DocumentController;
class WorkerController;

class ServiceCatalogBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged)

public:
    // `catalog` is used only to look items up; the bridge never edits it.
    ServiceCatalogBridge(class CatalogController* catalog,
                         DocumentController* documents, WorkerController* worker,
                         QObject* parent = nullptr);

    QString status() const { return fStatus; }
    bool busy() const { return fPlanner.measuring(); }

    // Explicit Add from the Library pane (double-click, Add button or a
    // drop into the running order). `index` is the insertion gap; -1 appends.
    Q_INVOKABLE bool addToService(const QString& catalogItemId, int index = -1);
    // Re-prepare a linked show from its catalog item without adding another
    // cue (used when the library item changes).
    Q_INVOKABLE bool refreshFromLibrary(const QString& catalogItemId);
    // Rebuild every linked item (e.g. after a theme change), preserving each
    // projection's place and the single linked show per item.
    Q_INVOKABLE void refreshAllLinked();
    // Rename propagates immediately (no re-measure needed).
    Q_INVOKABLE bool renameInService(const QString& catalogItemId,
                                     const QString& title);

signals:
    void statusChanged();

private:
    enum class Mode { Add, Refresh };
    void start(const QString& catalogItemId, Mode mode, int index);
    void publishPlan();
    // Review D6: the ids whose linked service copy is owed a re-preparation.
    // A SET, deduplicated and bounded by the catalog's own size, so an edit
    // arriving while the planner is busy is remembered rather than
    // overwriting the previous one, and the same id twice is one entry.
    void markDirty(const QString& catalogItemId, bool asAdd = false);
    // Takes one owed id and starts its preparation (Add when nothing is
    // linked yet, Refresh when a show exists). Called after a plan is
    // published or refused, and by the explicit entry points.
    void drainDirty();
    void setStatus(const QString& status);

    class CatalogController* fCatalog = nullptr;
    DocumentController* fDocuments = nullptr;
    SongPlanner fPlanner;

    QString fPendingId;
    QString fPendingTitle;
    // Task PT.3: the remembered Library look captured for this Add, as the
    // snapshot codec's JSON. Empty for a refresh (a lyric edit keeps the
    // item's existing look) and for a song that remembers nothing.
    QString fPendingThemeSnapshot;
    QString fPendingCcli;
    QString fPendingAuthors;
    QString fPendingCopyright;
    std::vector<openshow::catalog::Stanza> fPendingStanzas;
    Mode fMode = Mode::Add;
    int fPendingIndex = -1;
    // Review D6: the catalog revisions this plan was CAPTURED at, and the
    // service show it was captured for. Publishing rechecks both — a stale
    // plan is not written over newer catalog content, and a plan for a
    // replaced destination is dropped rather than published into the
    // successor.
    openshow::catalog::Revision fPendingRecordRevision = 0;
    openshow::catalog::Revision fPendingContentRevision = 0;
    QString fPendingShowId;
    // The ids still owed a re-preparation (deduplicated, catalog-bounded),
    // and the subset that is owed an ADD (an initial add whose plan was never
    // published). A Refresh whose destination disappears is dropped: the
    // operator removed that cue, and a bridge must not put it back.
    QSet<QString> fDirty;
    QSet<QString> fAddIntents;
    QString fStatus;
};

}  // namespace openshow::ui
