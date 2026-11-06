/*
 * The clipboard song import session (tasks SI.1/SI.3/SI.4).
 *
 * Reads the NORMAL clipboard only when the operator presses Paste, parses it,
 * measures the stanza-aware slide plan through the current Songs theme, and
 * offers exactly one destination: Add to Library. It never touches the
 * running order or the audience — adding a song to tonight's service is a
 * separate, explicit action on the Library pane.
 *
 * Draft lifecycle: draft → measuring → ready/error → committed/cancelled.
 * A second Paste replaces the draft. Cancel, repaste, page browsing and a
 * failed measurement write nothing. Add requires a title and a measured plan;
 * a successful Add creates ONE catalog record (one durable history entry) and
 * retires the draft so a double-click cannot add twice.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <string>
#include <vector>

#include "openshow/catalog/catalog.h"
#include "openshow/importer/lyrics.h"
#include "SongPlanner.h"

namespace openshow::ui {

class CatalogController;
class DocumentController;

class SongImport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY draftChanged)
    Q_PROPERTY(QString ccli READ ccli WRITE setCcli NOTIFY draftChanged)
    Q_PROPERTY(QString authors READ authors WRITE setAuthors NOTIFY draftChanged)
    Q_PROPERTY(QString copyright READ copyright WRITE setCopyright NOTIFY draftChanged)
    // True once a paste produced a draft that can be measured.
    Q_PROPERTY(bool hasDraft READ hasDraft NOTIFY draftChanged)
    // The clipboard could not be read / the paste was refused; also holds
    // the last success note. One honest sentence.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool measuring READ measuring NOTIFY stateChanged)
    Q_PROPERTY(bool canAdd READ canAdd NOTIFY stateChanged)
    // Generated slides: {lines: [QString], label: QString, group: QString}.
    Q_PROPERTY(QVariantList slides READ slides NOTIFY slidesChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY stateChanged)
    // Transient plan identity: thumbnail keys carry it, so a repaste cannot
    // leave a new card wearing the old plan's pixels.
    Q_PROPERTY(qint64 planGeneration READ planGeneration NOTIFY stateChanged)
    // True while this page owns the thumbnail lane; the page sets false when
    // hidden so Show/Themes/Bible cannot race it.
    Q_PROPERTY(bool previewActive READ previewActive WRITE setPreviewActive
                   NOTIFY stateChanged)

public:
    SongImport(CatalogController* catalog, DocumentController* documents,
               WorkerController* worker, QObject* parent = nullptr);

    QString title() const { return fTitle; }
    void setTitle(const QString& title);
    QString ccli() const { return fCcli; }
    void setCcli(const QString& ccli);
    QString authors() const { return fAuthors; }
    void setAuthors(const QString& authors);
    QString copyright() const { return fCopyright; }
    void setCopyright(const QString& copyright);
    bool hasDraft() const { return fHasDraft; }
    QString status() const { return fStatus; }
    bool measuring() const { return fPlanner.measuring(); }
    bool canAdd() const;
    QVariantList slides() const { return fSlides; }
    int selectedIndex() const { return fSelectedIndex; }
    qint64 planGeneration() const { return fPlanGeneration; }
    bool previewActive() const { return fPreviewActive; }
    void setPreviewActive(bool active);

    // Read the normal clipboard once and replace the draft.
    Q_INVOKABLE void pasteFromClipboard();
    // Rebuild the measured plan from the held text and the CURRENT Songs
    // theme. A no-op when the full fingerprint (words + box + typography)
    // has not changed, so a navigation/autosave signal costs nothing.
    Q_INVOKABLE void regenerate();
    Q_INVOKABLE void previewSlide(int index);
    Q_INVOKABLE QString previewKey(int index) const;
    Q_INVOKABLE QString previewSceneJson(int index) const;
    // The page's one destination: Library → Songs. One durable catalog
    // record; no service addition and no second destination.
    Q_INVOKABLE bool addToLibrary();
    Q_INVOKABLE void cancel();

signals:
    void draftChanged();
    void statusChanged();
    void stateChanged();
    void slidesChanged();
    // A successful Add, so the host can return to the Library and select it.
    void committed(const QString& catalogItemId);

private:
    void clearDraft();
    void publish();
    void setStatus(const QString& status);

    CatalogController* fCatalog = nullptr;
    DocumentController* fDocuments = nullptr;
    SongPlanner fPlanner;

    std::vector<openshow::catalog::Stanza> fStanzas;
    QString fTitle;
    QString fCcli;
    QString fAuthors;
    QString fCopyright;
    QString fStatus;
    bool fHasDraft = false;
    bool fPreviewActive = false;
    int fSelectedIndex = -1;
    qint64 fPlanGeneration = 0;
    QVariantList fSlides;
    QString fLastFingerprint;
};

}  // namespace openshow::ui
