/*
 * The one apply transaction: theme, optional reflow, optional remembered
 * Library look (task PT.6).
 *
 * Two scopes meet here, and they stay separate stores:
 *
 *   - the SERVICE edit is one DocumentController history entry — the chosen
 *     look and, for Apply and reflow, the replacement slides. One service
 *     undo restores the old theme, slide IDs and words.
 *   - the LIBRARY preference is one CatalogController history entry. Undoing
 *     it restores the remembered look without rebuilding any service.
 *
 * Order is deliberate: prevalidate the target, the snapshot and (for reflow)
 * the prepared plan; then write the Library preference durably; only then
 * publish the single service edit. A refused catalog write or a stale plan
 * publishes neither. The two files are still two stores — a crash between
 * them may keep the new future-use preference beside the older recoverable
 * service, and this header does not claim otherwise; each side is internally
 * valid.
 *
 * Apply only needs no measuring worker. Apply and reflow takes a plan that
 * ReflowPlanner already measured.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace openshow::ui {

class CatalogController;
class DocumentController;

class ThemeApplySession final : public QObject {
    Q_OBJECT
public:
    ThemeApplySession(DocumentController* documents, CatalogController* catalog,
                      QObject* parent = nullptr);

    // Apply only: one service edit. When `rememberLook` is asked and the item
    // is linked to `catalogItemId`, the snapshot is remembered first.
    Q_INVOKABLE bool applyOnly(const QString& showId, const QString& snapshotJson,
                               const QString& catalogItemId = QString(),
                               bool rememberLook = false);

    // Apply and reflow: the plan's words replace the text run in the SAME
    // service edit as the look. A stale plan (words, cue or slide count
    // moved) publishes neither side.
    Q_INVOKABLE bool applyReflow(const QString& showId, const QString& snapshotJson,
                                 const QVariantMap& plan,
                                 const QString& catalogItemId = QString(),
                                 bool rememberLook = false);

    QString lastError() const { return fError; }

signals:
    void applied(bool ok, const QString& error);

private:
    bool validateLook(const QString& snapshotJson);
    bool rememberLook(const QString& catalogItemId, const QString& snapshotJson);
    // Best-effort synchronous rollback when the service edit refuses after
    // the durable Library write. Not crash atomicity: a process death
    // between the two stores is the documented non-claim.
    void rollbackLook(const QString& catalogItemId, const QString& before);
    void finish(bool ok, const QString& error);

    DocumentController* fDocuments = nullptr;
    CatalogController* fCatalog = nullptr;
    QString fError;
};

}  // namespace openshow::ui
