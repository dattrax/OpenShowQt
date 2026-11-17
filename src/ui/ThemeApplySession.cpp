#include "ThemeApplySession.h"

#include "CatalogController.h"
#include "DocumentController.h"

namespace openshow::ui {

ThemeApplySession::ThemeApplySession(DocumentController* documents,
                                     CatalogController* catalog, QObject* parent)
    : QObject(parent), fDocuments(documents), fCatalog(catalog) {}

void ThemeApplySession::finish(bool ok, const QString& error) {
    fError = error;
    emit applied(ok, error);
}

bool ThemeApplySession::validateLook(const QString& snapshotJson) {
    if (fDocuments == nullptr) {
        finish(false, QStringLiteral("there is no open service"));
        return false;
    }
    if (snapshotJson.isEmpty()) {
        finish(false, QStringLiteral("no look was chosen"));
        return false;
    }
    // The one codec, through the same mapping the plan is measured with: a
    // snapshot that cannot be parsed or has no usable box is refused before
    // anything durable happens.
    if (fDocuments->boxStyleForSnapshot(snapshotJson).isEmpty()) {
        finish(false, QStringLiteral("the chosen look is not usable"));
        return false;
    }
    return true;
}

bool ThemeApplySession::rememberLook(const QString& catalogItemId,
                                     const QString& snapshotJson) {
    if (catalogItemId.isEmpty()) {
        return true;  // no Library link; nothing to remember
    }
    if (fCatalog == nullptr) {
        finish(false, QStringLiteral(
                          "the Library is not open — the look cannot be remembered"));
        return false;
    }
    const QString before = fCatalog->itemThemeSnapshot(catalogItemId);
    if (before.isEmpty() && !fCatalog->items().isEmpty()) {
        // Distinguish "remembers nothing" from "no such item": the setter
        // would refuse either way, but this sentence is the operator's.
        bool found = false;
        for (const QVariant& row : fCatalog->items()) {
            if (row.toMap().value(QStringLiteral("id")).toString() == catalogItemId) {
                found = true;
                break;
            }
        }
        if (!found) {
            finish(false, QStringLiteral("that Library item no longer exists"));
            return false;
        }
    }
    if (!fCatalog->rememberItemTheme(catalogItemId, snapshotJson)) {
        finish(false, fCatalog->status().isEmpty()
                          ? QStringLiteral("the Library could not remember the look")
                          : fCatalog->status());
        return false;
    }
    return true;
}

void ThemeApplySession::rollbackLook(const QString& catalogItemId,
                                     const QString& before) {
    if (catalogItemId.isEmpty() || fCatalog == nullptr) {
        return;
    }
    if (fCatalog->itemThemeSnapshot(catalogItemId) == before) {
        return;  // the write was a no-op; undoing would take an older entry
    }
    fCatalog->undo();
}

bool ThemeApplySession::applyOnly(const QString& showId,
                                  const QString& snapshotJson,
                                  const QString& catalogItemId,
                                  bool rememberLookAsked) {
    if (!validateLook(snapshotJson)) {
        return false;
    }
    if (fDocuments->showInfo(showId).isEmpty()) {
        finish(false, QStringLiteral("that item no longer exists"));
        return false;
    }
    const QString before =
        (rememberLookAsked && fCatalog != nullptr)
            ? fCatalog->itemThemeSnapshot(catalogItemId)
            : QString();
    if (rememberLookAsked && !rememberLook(catalogItemId, snapshotJson)) {
        return false;
    }
    const QString theme = fDocuments->applyRememberedTheme(showId, snapshotJson);
    if (theme.isEmpty()) {
        rollbackLook(rememberLookAsked ? catalogItemId : QString(), before);
        finish(false, fDocuments->status().isEmpty()
                          ? QStringLiteral("the look could not be applied")
                          : fDocuments->status());
        return false;
    }
    finish(true, QString());
    return true;
}

bool ThemeApplySession::applyReflow(const QString& showId,
                                    const QString& snapshotJson,
                                    const QVariantMap& plan,
                                    const QString& catalogItemId,
                                    bool rememberLookAsked) {
    if (!validateLook(snapshotJson)) {
        return false;
    }
    const QString problem = fDocuments->reflowPlanProblem(showId, plan);
    if (!problem.isEmpty()) {
        finish(false, problem);
        return false;
    }
    const QString before =
        (rememberLookAsked && fCatalog != nullptr)
            ? fCatalog->itemThemeSnapshot(catalogItemId)
            : QString();
    if (rememberLookAsked && !rememberLook(catalogItemId, snapshotJson)) {
        return false;
    }
    if (!fDocuments->commitReflowPlan(showId, snapshotJson, plan)) {
        rollbackLook(rememberLookAsked ? catalogItemId : QString(), before);
        finish(false, fDocuments->status().isEmpty()
                          ? QStringLiteral("the reflow could not be published")
                          : fDocuments->status());
        return false;
    }
    finish(true, QString());
    return true;
}

}  // namespace openshow::ui
