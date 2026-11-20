#include "ServiceCatalogBridge.h"

#include <QStringList>
#include <QVariantMap>

#include "CatalogController.h"
#include "DocumentController.h"
#include "ThumbnailStore.h"
#include "WorkerController.h"

namespace openshow::ui {

ServiceCatalogBridge::ServiceCatalogBridge(CatalogController* catalog,
                                           DocumentController* documents,
                                           WorkerController* worker, ThumbnailStore* pictures,
                                           QObject* parent)
    : QObject(parent), fCatalog(catalog), fDocuments(documents), fPictures(pictures),
      fPlanner(documents, worker, this) {
    if (fPictures != nullptr && fCatalog != nullptr && fDocuments != nullptr) {
        // TC.9: a finished service render of a linked show becomes one more
        // remembered look of its Library item. The catalog's model and every
        // revision stay untouched; the block is written cache-only.
        connect(fPictures, &ThumbnailStore::recordStored, this,
                [this](const QString&, const QString& slideId,
                       const QString& fingerprint) {
                    rememberServiceLook(slideId, fingerprint);
                });
    }
    connect(&fPlanner, &SongPlanner::finished, this,
            [this](bool ok, const QString& error) {
                if (!ok) {
                    setStatus(error.isEmpty()
                                  ? QStringLiteral("the song could not be prepared")
                                  : error);
                    return;
                }
                publishPlan();
            });
    if (fCatalog != nullptr && fDocuments != nullptr) {
        // The reference's propagation: a durable Library edit re-prepares
        // every service show that names the item. A rename is applied
        // immediately; a content/metadata change re-measures and re-lays.
        connect(fCatalog, &CatalogController::itemChanged, this,
                [this](const QString& id) {
                    const openshow::catalog::CatalogItem* item =
                        fCatalog->findItem(id);
                    if (item == nullptr ||
                        fDocuments->showForCatalogItem(id).isEmpty()) {
                        return;  // nothing linked yet (e.g. the initial add)
                    }
                    fDocuments->renameLinkedCatalogShow(
                        id, QString::fromStdString(item->title));
                    // Review D6: an edit that lands while the planner is
                    // busy is REMEMBERED, not dropped — including an edit to
                    // the song being prepared right now, whose plan is now
                    // stale and must run again before it can be published.
                    if (fPlanner.measuring()) {
                        markDirty(id);
                        return;
                    }
                    refreshFromLibrary(id);
                });
        connect(fCatalog, &CatalogController::itemRemoved, this,
                [this](const QString& id) {
                    if (fPendingId == id && fPlanner.measuring()) {
                        fPlanner.cancel();
                        setStatus(QStringLiteral("Preparation cancelled: the Library song was deleted"));
                    }
                    fDirty.remove(id);
                    fAddIntents.remove(id);
                    if (fDocuments != nullptr) {
                        fDocuments->detachLinkedCatalogShow(id);
                    }
                });
        connect(fCatalog, &CatalogController::itemsPermanentlyRemoved,
                fDocuments, &DocumentController::forgetDeletedCatalogItems);
    }
}

bool ServiceCatalogBridge::addToService(const QString& catalogItemId, int index) {
    if (fCatalog == nullptr || fDocuments == nullptr) {
        setStatus(QStringLiteral("the Library or the service is not available"));
        return false;
    }
    if (fCatalog->findItem(catalogItemId) == nullptr) {
        setStatus(QStringLiteral("that Library item no longer exists"));
        return false;
    }
    if (fPlanner.measuring()) {
        setStatus(QStringLiteral(
            "a song is already being prepared — wait for it to finish"));
        return false;
    }
    start(catalogItemId, Mode::Add, index);
    return true;
}

bool ServiceCatalogBridge::refreshFromLibrary(const QString& catalogItemId) {
    if (fCatalog == nullptr || fDocuments == nullptr) {
        return false;
    }
    if (fCatalog->findItem(catalogItemId) == nullptr) {
        return false;
    }
    if (fDocuments->showForCatalogItem(catalogItemId).isEmpty()) {
        return false;
    }
    if (fPlanner.measuring()) {
        markDirty(catalogItemId);
        return false;
    }
    fDirty.remove(catalogItemId);  // this run captures its current contents
    fAddIntents.remove(catalogItemId);
    start(catalogItemId, Mode::Refresh, -1);
    return true;
}

void ServiceCatalogBridge::refreshAllLinked() {
    if (fCatalog == nullptr || fDocuments == nullptr) {
        return;
    }
    // Review D6: EVERY linked show is owed a rebuild, not just the first.
    // The set remembers them all; the drain runs one preparation at a time
    // and the next is taken when this one publishes.
    for (const QVariant& row : fCatalog->items()) {
        const QString id = row.toMap().value(QStringLiteral("id")).toString();
        if (!fDocuments->showForCatalogItem(id).isEmpty()) {
            markDirty(id);
        }
    }
    drainDirty();
}

bool ServiceCatalogBridge::renameInService(const QString& catalogItemId,
                                           const QString& title) {
    if (fDocuments == nullptr ||
        fDocuments->showForCatalogItem(catalogItemId).isEmpty()) {
        return false;
    }
    return fDocuments->renameLinkedCatalogShow(catalogItemId, title);
}

void ServiceCatalogBridge::start(const QString& catalogItemId, Mode mode, int index) {
    const openshow::catalog::CatalogItem* item = fCatalog->findItem(catalogItemId);
    if (item == nullptr) {
        setStatus(QStringLiteral("that Library item no longer exists"));
        return;
    }
    fPendingId = catalogItemId;
    fPendingTitle = QString::fromStdString(item->title);
    fPendingCcli = QString::fromStdString(item->metadata.ccli);
    fPendingAuthors = QString::fromStdString(item->metadata.authors);
    fPendingCopyright = QString::fromStdString(item->metadata.copyright);
    fPendingStanzas = item->stanzas;
    fMode = mode;
    fPendingIndex = index;
    // Task PT.3: a NEW preparation wears the Library's remembered look and
    // must MEASURE it, not the service current theme. A refresh keeps the
    // item's existing applied theme (PT.6), so it captures nothing here.
    fPendingThemeSnapshot =
        mode == Mode::Add ? fCatalog->itemThemeSnapshot(catalogItemId) : QString();
    // The plan this run will publish must belong to the catalog and the
    // service it was CAPTURED from (review D6).
    fPendingRecordRevision = item->recordRevision;
    fPendingContentRevision = item->contentRevision;
    fPendingShowId = fDocuments != nullptr ? fDocuments->showForCatalogItem(catalogItemId)
                                           : QString();
    setStatus(mode == Mode::Add
                  ? QStringLiteral("Preparing \u201c%1\u201d\u2026").arg(fPendingTitle)
                  : QStringLiteral("Updating \u201c%1\u201d\u2026").arg(fPendingTitle));
    if (!fPendingThemeSnapshot.isEmpty() && fDocuments != nullptr) {
        openshow::importer::SongPaginationInput captured;
        if (songInputForStyle(fPendingStanzas,
                              fDocuments->boxStyleForSnapshot(fPendingThemeSnapshot),
                              &captured)) {
            fPlanner.start(fPendingStanzas, &captured);
            return;
        }
    }
    fPlanner.start(fPendingStanzas);
}

void ServiceCatalogBridge::publishPlan() {
    if (fDocuments == nullptr) {
        setStatus(QStringLiteral("the service is not available"));
        return;
    }
    // Review D6: the plan is only as current as the catalog it was CAPTURED
    // from. Publishing rechecks the record and content revisions, and the
    // destination show; a plan that no longer describes them is dropped and
    // the id is owed another run instead. This is what makes an edit to the
    // song being prepared converge on its newest contents rather than being
    // lost when the completion compares ids.
    const openshow::catalog::CatalogItem* current = fCatalog->findItem(fPendingId);
    if (current == nullptr) {
        setStatus(QStringLiteral("Preparation cancelled: the Library song was deleted"));
        drainDirty();
        return;
    }
    const bool catalogMoved = current->recordRevision != fPendingRecordRevision ||
                              current->contentRevision != fPendingContentRevision;
    const bool destinationMoved =
        fMode == Mode::Refresh &&
        fDocuments->showForCatalogItem(fPendingId) != fPendingShowId;
    if (catalogMoved || destinationMoved) {
        // The measurement described something that has since changed (or a
        // destination that is no longer the linked show). Prepare again —
        // the drain picks the right mode for what is linked NOW.
        markDirty(fPendingId, /*asAdd=*/fMode == Mode::Add);
        setStatus(QStringLiteral("\u201c%1\u201d changed while it was being prepared; "
                                 "preparing it again")
                      .arg(fPendingTitle));
        drainDirty();
        return;
    }
    QVariantList slides;
    for (const openshow::importer::SongSlide& slide : fPlanner.slides()) {
        QVariantMap row;
        QStringList lines;
        for (const std::string& line : slide.lines) {
            lines.push_back(QString::fromStdString(line));
        }
        row.insert(QStringLiteral("lines"), lines);
        row.insert(QStringLiteral("label"), QString::fromStdString(slide.label));
        const std::string group =
            slide.stanzaIndex < fPendingStanzas.size()
                ? fPendingStanzas[slide.stanzaIndex].label
                : slide.label;
        row.insert(QStringLiteral("group"), QString::fromStdString(group));
        row.insert(QStringLiteral("blank"), slide.lines.empty());
        slides.append(row);
    }
    const QString showId = fDocuments->upsertCatalogSong(
        fPendingId, fPendingTitle, fPendingCcli, fPendingAuthors,
        fPendingCopyright, slides, fMode == Mode::Add, fPendingIndex,
        fPendingThemeSnapshot);
    if (showId.isEmpty()) {
        setStatus(QStringLiteral("the song could not be added: %1")
                      .arg(fDocuments->status()));
        // A refused write leaves the id owed: the next event retries it, and
        // an edit is not lost because a save failed once.
        markDirty(fPendingId, /*asAdd=*/fMode == Mode::Add);
        drainDirty();
        return;
    }
    // TC.9: a look this Library already has for the song's exact prepared
    // layout is copied under the new slide ids, so the first frame after an
    // Add can come from disk rather than the worker. A different theme,
    // reflow or lyric change produces different fingerprints and misses.
    reuseLibraryLooks(fPendingId, showId);
    setStatus(fMode == Mode::Add
                  ? QStringLiteral("\u201c%1\u201d added to the service")
                        .arg(fPendingTitle)
                  : QStringLiteral("\u201c%1\u201d updated from the Library")
                        .arg(fPendingTitle));
    // The next owed id, one per published plan: an edit that arrived while
    // this one was being measured is prepared now, against the catalog as it
    // is — never merged into the plan that just went out.
    drainDirty();
}

void ServiceCatalogBridge::markDirty(const QString& catalogItemId, bool asAdd) {
    if (catalogItemId.isEmpty()) {
        return;
    }
    fDirty.insert(catalogItemId);
    if (asAdd) {
        fAddIntents.insert(catalogItemId);
    }
}

void ServiceCatalogBridge::drainDirty() {
    if (fCatalog == nullptr || fDocuments == nullptr || fPlanner.measuring() ||
        fDirty.isEmpty()) {
        return;
    }
    const QString id = *fDirty.begin();
    fDirty.remove(id);
    const bool owedAdd = fAddIntents.remove(id);
    if (fCatalog->findItem(id) == nullptr) {
        return;  // deleted while it waited: nothing to prepare
    }
    // A linked show is refreshed; an Add whose plan was never published
    // (nothing linked yet) is prepared as the Add it was. Anything else
    // whose destination disappeared was a Refresh against a cue the operator
    // removed — dropped, never re-added.
    const bool linked = !fDocuments->showForCatalogItem(id).isEmpty();
    if (!linked && !owedAdd) {
        return;
    }
    start(id, linked ? Mode::Refresh : Mode::Add, linked ? -1 : fPendingIndex);
}

void ServiceCatalogBridge::reuseLibraryLooks(const QString& catalogItemId,
                                             const QString& showId) {
    if (fPictures == nullptr || fCatalog == nullptr || fDocuments == nullptr) {
        return;
    }
    const auto variants = fCatalog->thumbnailVariantsFor(catalogItemId);
    if (variants.empty()) {
        return;
    }
    const openshow::domain::Show* show =
        fDocuments->documents().findShow(showId.toStdString());
    if (show == nullptr) {
        return;
    }
    for (const openshow::domain::Slide& slide : show->slides) {
        const QString slideId = QString::fromStdString(slide.id);
        const QString fingerprint = fDocuments->thumbnailSourceForSlide(slideId)
                                        .value(QStringLiteral("fingerprint"))
                                        .toString();
        if (fingerprint.isEmpty()) {
            continue;
        }
        const openshow::domain::ThumbnailCacheEntry* match =
            openshow::domain::findThumbnailVariant(variants, fingerprint.toStdString());
        if (match == nullptr) {
            continue;
        }
        openshow::domain::ThumbnailCacheEntry entry = *match;
        entry.showId = showId.toStdString();
        entry.slideId = slide.id;
        fPictures->adoptEntry(entry);
    }
}

void ServiceCatalogBridge::rememberServiceLook(const QString& slideId,
                                               const QString& fingerprint) {
    if (fPictures == nullptr || fCatalog == nullptr || fDocuments == nullptr) {
        return;
    }
    const openshow::domain::ThumbnailCacheEntry* entry =
        fPictures->find(slideId, fingerprint);
    if (entry == nullptr) {
        return;
    }
    const openshow::domain::Documents& documents = fDocuments->documents();
    for (const openshow::domain::Id& showId : documents.showIds()) {
        const openshow::domain::Show* show = documents.findShow(showId);
        if (show == nullptr || show->catalogItemId.empty()) {
            continue;
        }
        for (const openshow::domain::Slide& slide : show->slides) {
            if (slide.id != slideId.toStdString()) {
                continue;
            }
            openshow::domain::ThumbnailCacheEntry variant = *entry;
            variant.showId.clear();
            variant.slideId.clear();
            fCatalog->rememberThumbnailVariant(
                QString::fromStdString(show->catalogItemId), variant);
            return;
        }
    }
}

void ServiceCatalogBridge::setStatus(const QString& status) {
    if (fStatus == status) {
        return;
    }
    fStatus = status;
    emit statusChanged();
}

}  // namespace openshow::ui
