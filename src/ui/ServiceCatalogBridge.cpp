#include "ServiceCatalogBridge.h"

#include <QStringList>
#include <QVariantMap>

#include "CatalogController.h"
#include "DocumentController.h"
#include "WorkerController.h"

namespace openshow::ui {

ServiceCatalogBridge::ServiceCatalogBridge(CatalogController* catalog,
                                           DocumentController* documents,
                                           WorkerController* worker, QObject* parent)
    : QObject(parent), fCatalog(catalog), fDocuments(documents),
      fPlanner(documents, worker, this) {
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
                    if (fPlanner.measuring()) {
                        fDeferredRefresh = id;
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
                    if (fDeferredRefresh == id) fDeferredRefresh.clear();
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
        fDeferredRefresh = catalogItemId;
        return false;
    }
    start(catalogItemId, Mode::Refresh, -1);
    return true;
}

void ServiceCatalogBridge::refreshAllLinked() {
    if (fCatalog == nullptr || fDocuments == nullptr) {
        return;
    }
    for (const QVariant& row : fCatalog->items()) {
        const QString id = row.toMap().value(QStringLiteral("id")).toString();
        if (!fDocuments->showForCatalogItem(id).isEmpty()) {
            refreshFromLibrary(id);
            return;  // one at a time; the deferred slot would be overwritten
        }
    }
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
    setStatus(mode == Mode::Add
                  ? QStringLiteral("Preparing \u201c%1\u201d\u2026").arg(fPendingTitle)
                  : QStringLiteral("Updating \u201c%1\u201d\u2026").arg(fPendingTitle));
    fPlanner.start(fPendingStanzas);
}

void ServiceCatalogBridge::publishPlan() {
    if (fDocuments == nullptr) {
        setStatus(QStringLiteral("the service is not available"));
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
        fPendingCopyright, slides, fMode == Mode::Add, fPendingIndex);
    if (showId.isEmpty()) {
        setStatus(QStringLiteral("the song could not be added: %1")
                      .arg(fDocuments->status()));
        return;
    }
    setStatus(fMode == Mode::Add
                  ? QStringLiteral("\u201c%1\u201d added to the service")
                        .arg(fPendingTitle)
                  : QStringLiteral("\u201c%1\u201d updated from the Library")
                        .arg(fPendingTitle));
    if (!fDeferredRefresh.isEmpty() && fDeferredRefresh != fPendingId) {
        const QString next = fDeferredRefresh;
        fDeferredRefresh.clear();
        refreshFromLibrary(next);
    } else {
        fDeferredRefresh.clear();
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
