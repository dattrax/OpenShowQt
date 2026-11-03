#include "BibleImport.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QVariantMap>

#include "openshow/importer/bookindex.h"

namespace {

// The passage page is a couple of hundred KB; anything many times that is
// not the page we asked for, and the plan's bounded-memory rule says stop
// rather than buffer it.
constexpr qint64 kMaxResponseBytes = 6 * 1024 * 1024;
constexpr int kTransferTimeoutMs = 20000;

// An honest desktop-shaped UA: this is one operator's click, not a spider.
const char kUserAgent[] =
    "Mozilla/5.0 (X11; Linux x86_64) OpenShowQt/0.1 passage lookup";

const char kNoticeSentence[] =
    "Online Bible lookup \u2014 internet connection required";

QUrl passageUrl(const QString& reference, const QString& version) {
    QUrl url(QStringLiteral("https://www.biblegateway.com/passage/"));
    QUrlQuery query;
    // The plan's encoded search and version parameters, nothing else.
    query.addQueryItem(QStringLiteral("search"), reference);
    query.addQueryItem(QStringLiteral("version"), version);
    url.setQuery(query);
    return url;
}

}  // namespace

BibleImport::BibleImport(QObject* parent) : QObject(parent) {
    fStatus = QString::fromUtf8(kNoticeSentence);
}

void BibleImport::setNetworkAccessManager(QNetworkAccessManager* manager) {
    if (manager == fManager) {
        return;
    }
    if (fOwnsManager) {
        delete fManager;
        fOwnsManager = false;
    }
    fManager = manager;
}

void BibleImport::setReference(const QString& reference) {
    if (fReference == reference) {
        return;
    }
    fReference = reference;
    emit stateChanged();
}

void BibleImport::setTranslation(const QString& version) {
    if (fTranslation == version) {
        return;
    }
    fTranslation = version;
    // The fetched verses belong to the translation they were fetched in.
    // Moving the menu makes that answer stale, so it goes — the operator
    // re-looks-up and never wears a mismatched pair.
    fVerses.clear();
    fAttribution.clear();
    emit stateChanged();
    emit passageChanged();
}

QVariantList BibleImport::verses() const {
    QVariantList list;
    for (const VerseRow& row : fVerses) {
        QVariantMap entry;
        entry.insert(QStringLiteral("reference"),
                     QString::fromStdString(row.verse.reference));
        entry.insert(QStringLiteral("text"), QString::fromStdString(row.verse.text));
        entry.insert(QStringLiteral("selected"), row.selected);
        list.append(entry);
    }
    return list;
}

QVariantList BibleImport::translations() const {
    QVariantList list;
    for (const openshow::importer::Translation& t : openshow::importer::translations()) {
        QVariantMap entry;
        entry.insert(QStringLiteral("version"), QString::fromUtf8(t.version));
        entry.insert(QStringLiteral("name"), QString::fromUtf8(t.name));
        entry.insert(QStringLiteral("attributionNote"), QString::fromUtf8(t.attributionNote));
        list.append(entry);
    }
    return list;
}

std::vector<openshow::importer::PassageVerse> BibleImport::selectedVerses() const {
    std::vector<openshow::importer::PassageVerse> kept;
    for (const VerseRow& row : fVerses) {
        if (row.selected) {
            kept.push_back(row.verse);
        }
    }
    return kept;
}

void BibleImport::lookup() {
    if (fReference.trimmed().isEmpty()) {
        fStatus = QStringLiteral("enter a reference first, for example \u201c1 Corinthians 1:1-6\u201d");
        emit stateChanged();
        return;
    }
    if (fManager == nullptr) {
        fOwnsManager = true;
        fManager = new QNetworkAccessManager(this);
        // A manager with no cache is the whole disk-silence story: nothing
        // fetched is stored, and nothing stored is ever fetched.
        fManager->setCache(nullptr);
    }
    if (fBusy) {
        // One in-flight lookup at a time: the newer request retires the
        // older, so the cap holds even under clicking.
        cancel();
    }
    // A new lookup discards the previous answer first (bounded memory).
    fSizeCapped = false;
    fVerses.clear();
    fAttribution.clear();
    emit passageChanged();

    ++fGeneration;
    fBusy = true;
    fStatus = QStringLiteral("looking up %1 \u2026").arg(fReference.trimmed());
    emit stateChanged();
    send(passageUrl(fReference.trimmed(), fTranslation));
}

void BibleImport::send(const QUrl& url) {
    QNetworkRequest request{url};
    request.setRawHeader("Accept", "text/html");
    request.setRawHeader("User-Agent", kUserAgent);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(kTransferTimeoutMs);
    // QNAM's default redirect policy already follows same-or-safer
    // redirects inside the manager; the importer adds no chasing of its
    // own, so one ask stays one passage.
    fReply = fManager->get(request);
    const quint64 generation = fGeneration;
    connect(fReply, &QNetworkReply::downloadProgress, this,
            [this, generation](qint64 received, qint64 total) {
                if (generation != fGeneration || fReply == nullptr) {
                    return;
                }
                if (received > kMaxResponseBytes || (total > 0 && total > kMaxResponseBytes)) {
                    fSizeCapped = true;
                    fReply->abort();
                }
            });
    connect(fReply, &QNetworkReply::finished, this, [this, generation] {
        QNetworkReply* reply = fReply;
        if (generation != fGeneration) {
            // A retired request answers to nobody.
            reply->deleteLater();
            if (fReply == reply) {
                fReply = nullptr;
            }
            return;
        }
        fReply = nullptr;
        fBusy = false;
        const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError;
        const QString errorString = reply->errorString();
        const QByteArray body = reply->readAll();
        const QVariant statusVariant =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const QNetworkReply::NetworkError replyError = reply->error();
        reply->deleteLater();

        if (cancelled) {
            // cancel() says its own sentence; the size cap and a transfer
            // timeout each say theirs.
            if (fSizeCapped) {
                fSizeCapped = false;
                failWith(QStringLiteral("the answer was larger than the importer's limit; "
                                        "nothing was imported"));
            } else if (errorString.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)) {
                failWith(QStringLiteral("the lookup timed out \u2014 retry when ready"));
            }
            return;
        }
        if (replyError != QNetworkReply::NoError) {
            switch (replyError) {
                case QNetworkReply::HostNotFoundError:
                case QNetworkReply::ProxyNotFoundError:
                case QNetworkReply::UnknownNetworkError:
                    failWith(QString::fromUtf8(kNoticeSentence));
                    break;
                case QNetworkReply::SslHandshakeFailedError:
                    failWith(QStringLiteral("the secure connection failed \u2014 retry when ready"));
                    break;
                default:
                    failWith(QStringLiteral("the lookup failed: %1")
                                 .arg(errorString.isEmpty() ? QStringLiteral("network error")
                                                            : errorString));
                    break;
            }
            return;
        }
        const int status = statusVariant.toInt();
        if (status >= 400) {
            failWith(QStringLiteral("Bible Gateway answered %1 \u2014 retry, or try another "
                                    "reference")
                         .arg(status));
            return;
        }
        if (body.size() > kMaxResponseBytes) {
            failWith(QStringLiteral("the answer was larger than the importer's limit; "
                                    "nothing was imported"));
            return;
        }
        // The parse is one pass over a bounded body; it belongs here, on
        // the importer's own thread, before anything can supersede it.
        finishWith(openshow::importer::parsePassagePage(
            std::string_view{body.constData(), static_cast<std::size_t>(body.size())},
            fTranslation.toStdString()));
    });
}

void BibleImport::cancel() {
    if (fReply != nullptr) {
        // The generation retires first: the reply's finished() will find
        // it stale and delete in silence.
        ++fGeneration;
        fReply->abort();
        fReply = nullptr;
    }
    if (fBusy) {
        fBusy = false;
        fStatus = QStringLiteral("lookup cancelled");
        emit stateChanged();
    }
}

void BibleImport::retry() {
    lookup();
}

void BibleImport::reset() {
    cancel();
    fVerses.clear();
    fAttribution.clear();
    fStatus = QString::fromUtf8(kNoticeSentence);
    emit stateChanged();
    emit passageChanged();
}

void BibleImport::setVerseSelected(int index, bool selected) {
    if (index < 0 || static_cast<std::size_t>(index) >= fVerses.size() ||
        fVerses[static_cast<std::size_t>(index)].selected == selected) {
        return;
    }
    fVerses[static_cast<std::size_t>(index)].selected = selected;
    emit passageChanged();
}

void BibleImport::selectAllVerses(bool selected) {
    for (VerseRow& row : fVerses) {
        row.selected = selected;
    }
    emit passageChanged();
}

QVariantList BibleImport::suggestBooks(const QString& typed) const {
    // The built-in table answers, in canon order; the lookup is untouched
    // by anything here. Conversion only — the matching lives in
    // openshow::importer where it is testable with no Qt at all.
    QVariantList rows;
    for (const openshow::importer::BookSuggestion& suggestion :
         openshow::importer::suggestBooks(typed.toStdString())) {
        QVariantMap row;
        row.insert(QStringLiteral("name"), QString::fromStdString(suggestion.name));
        row.insert(QStringLiteral("completion"),
                   QString::fromStdString(suggestion.completion));
        rows.push_back(row);
    }
    return rows;
}

void BibleImport::failWith(const QString& sentence) {
    fStatus = sentence;
    emit stateChanged();
}

void BibleImport::finishWith(openshow::importer::PassageParse parse) {
    if (!parse.ok) {
        failWith(QString::fromStdString(parse.error));
        return;
    }
    fAttribution = QString::fromStdString(parse.passage.attribution);
    fVerses.clear();
    for (openshow::importer::PassageVerse& verse : parse.passage.verses) {
        fVerses.push_back({std::move(verse), true});
    }
    fStatus = QStringLiteral("%1 verses \u2014 choose and preview before adding")
                  .arg(fVerses.size());
    emit stateChanged();
    emit passageChanged();
}
