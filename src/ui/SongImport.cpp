#include "SongImport.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QStringList>
#include <QVariantMap>

#include "CatalogController.h"
#include "DocumentController.h"
#include "WorkerController.h"

namespace openshow::ui {

namespace {

QString fromStd(const std::string& text) { return QString::fromStdString(text); }

}  // namespace

SongImport::SongImport(CatalogController* catalog, DocumentController* documents,
                       WorkerController* worker, QObject* parent)
    : QObject(parent), fCatalog(catalog), fDocuments(documents),
      fPlanner(documents, worker, this) {
    connect(&fPlanner, &SongPlanner::finished, this,
            [this](bool ok, const QString& error) {
                if (!ok) {
                    fSlides.clear();
                    ++fPlanGeneration;
                    fSelectedIndex = -1;
                    setStatus(error);
                    emit slidesChanged();
                    emit stateChanged();
                    return;
                }
                fSlides.clear();
                const auto& planned = fPlanner.slides();
                for (const openshow::importer::SongSlide& slide : planned) {
                    QVariantMap row;
                    QStringList lines;
                    for (const std::string& line : slide.lines) {
                        lines.push_back(fromStd(line));
                    }
                    row.insert(QStringLiteral("lines"), lines);
                    row.insert(QStringLiteral("label"), fromStd(slide.label));
                    const std::string group =
                        slide.stanzaIndex < fStanzas.size()
                            ? fStanzas[slide.stanzaIndex].label
                            : slide.label;
                    row.insert(QStringLiteral("group"), fromStd(group));
                    row.insert(QStringLiteral("blank"), slide.lines.empty());
                    fSlides.append(row);
                }
                ++fPlanGeneration;
                fSelectedIndex = fSlides.isEmpty() ? -1 : 0;
                setStatus(QStringLiteral("%1 slide%2 measured")
                              .arg(fSlides.size())
                              .arg(fSlides.size() == 1 ? QString() : QStringLiteral("s")));
                emit slidesChanged();
                emit stateChanged();
            });
}

void SongImport::setTitle(const QString& title) {
    if (fTitle == title) {
        return;
    }
    fTitle = title;
    emit draftChanged();
    emit stateChanged();
}

void SongImport::setCcli(const QString& ccli) {
    if (fCcli == ccli) {
        return;
    }
    fCcli = ccli;
    emit draftChanged();
}

void SongImport::setAuthors(const QString& authors) {
    if (fAuthors == authors) {
        return;
    }
    fAuthors = authors;
    emit draftChanged();
}

void SongImport::setCopyright(const QString& copyright) {
    if (fCopyright == copyright) {
        return;
    }
    fCopyright = copyright;
    emit draftChanged();
}

bool SongImport::canAdd() const {
    return fHasDraft && fPlanner.hasPlan() && !fPlanner.measuring() &&
           !fSlides.isEmpty() && !fTitle.trimmed().isEmpty();
}

void SongImport::setPreviewActive(bool active) {
    if (fPreviewActive == active) {
        return;
    }
    fPreviewActive = active;
    emit stateChanged();
}

void SongImport::pasteFromClipboard() {
    const QString text =
        QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    if (text.trimmed().isEmpty()) {
        setStatus(QStringLiteral(
            "the clipboard has no text — copy the song's words first"));
        return;
    }
    // Pasting again REPLACES the draft, including a failed paste: a stale
    // plan must never remain Add-able under a refused replacement.
    clearDraft();
    const openshow::importer::LyricsDraft draft = openshow::importer::parseLyrics(
        text.toStdString());
    if (!draft.ok) {
        setStatus(fromStd(draft.error));
        emit draftChanged();
        emit stateChanged();
        return;
    }
    fStanzas = draft.stanzas;
    fTitle = fromStd(draft.title);
    fCcli = fromStd(draft.metadata.ccli);
    fAuthors = fromStd(draft.metadata.authors);
    fCopyright = fromStd(draft.metadata.copyright);
    fHasDraft = true;
    fSlides.clear();
    ++fPlanGeneration;
    fSelectedIndex = -1;
    setStatus(draft.hasMetadata
                  ? QStringLiteral("%1 stanzas read; CCLI %2 and attribution found")
                        .arg(static_cast<qint64>(fStanzas.size()))
                        .arg(fCcli.isEmpty() ? QStringLiteral("(none)")
                                             : fCcli)
                  : QStringLiteral("%1 stanzas read; no CCLI/attribution footer found")
                        .arg(static_cast<qint64>(fStanzas.size())));
    emit draftChanged();
    emit slidesChanged();
    emit stateChanged();
    regenerate();
}

void SongImport::regenerate() {
    if (!fHasDraft || fStanzas.empty()) {
        return;
    }
    openshow::importer::SongPaginationInput input;
    songInputFor(fDocuments, fStanzas, &input);
    const std::string fingerprint =
        openshow::importer::songPlanFingerprint(input);
    if (fPlanner.hasPlan() && fromStd(fingerprint) == fLastFingerprint) {
        return;
    }
    fLastFingerprint = fromStd(fingerprint);
    fPlanner.start(fStanzas, &input);
    emit stateChanged();
}

void SongImport::previewSlide(int index) {
    if (index < 0 || index >= fSlides.size()) {
        return;
    }
    fSelectedIndex = index;
    emit stateChanged();
}

QString SongImport::previewKey(int index) const {
    return QStringLiteral("song:%1:%2").arg(fPlanGeneration).arg(index);
}

QString SongImport::previewSceneJson(int index) const {
    if (fDocuments == nullptr || index < 0 ||
        index >= static_cast<int>(fPlanner.slides().size())) {
        return QString();
    }
    QStringList lines;
    for (const std::string& line : fPlanner.slides()[index].lines) {
        lines.push_back(fromStd(line));
    }
    return fDocuments->songSlideSceneJson(lines);
}

bool SongImport::addToLibrary() {
    if (fCatalog == nullptr) {
        setStatus(QStringLiteral("the Library is not available"));
        return false;
    }
    if (!fHasDraft) {
        setStatus(QStringLiteral("paste a song before adding it"));
        return false;
    }
    if (fTitle.trimmed().isEmpty()) {
        setStatus(QStringLiteral("add to Library: type the song's title first"));
        return false;
    }
    if (fPlanner.measuring()) {
        setStatus(QStringLiteral("the words are still being measured — Add wakes "
                                 "when they are ready"));
        return false;
    }
    if (!fPlanner.hasPlan() || fSlides.isEmpty()) {
        setStatus(QStringLiteral("add to Library: no measured slides are ready"));
        return false;
    }
    QVariantList stanzas;
    for (const openshow::catalog::Stanza& stanza : fStanzas) {
        QVariantMap row;
        row.insert(QStringLiteral("label"), fromStd(stanza.label));
        QStringList lines;
        for (const std::string& line : stanza.lines) {
            lines.push_back(fromStd(line));
        }
        row.insert(QStringLiteral("lines"), lines);
        stanzas.append(row);
    }
    const QString id = fCatalog->addSong(fTitle, stanzas, fCcli, fAuthors,
                                         fCopyright);
    if (id.isEmpty()) {
        setStatus(QStringLiteral("add to Library: ") + fCatalog->status());
        return false;
    }
    setStatus(QStringLiteral("Added \u201c%1\u201d to Library \u2192 Songs")
                  .arg(fTitle.trimmed()));
    emit committed(id);
    clearDraft();
    return true;
}

void SongImport::cancel() {
    clearDraft();
    setStatus(QString());
}

void SongImport::clearDraft() {
    fPlanner.cancel();
    fStanzas.clear();
    fTitle.clear();
    fCcli.clear();
    fAuthors.clear();
    fCopyright.clear();
    fHasDraft = false;
    fLastFingerprint.clear();
    fSlides.clear();
    ++fPlanGeneration;
    fSelectedIndex = -1;
    emit draftChanged();
    emit slidesChanged();
    emit stateChanged();
}

void SongImport::setStatus(const QString& status) {
    if (fStatus == status) {
        return;
    }
    fStatus = status;
    emit statusChanged();
}

}  // namespace openshow::ui
