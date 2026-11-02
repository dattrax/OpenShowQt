// The Bible importer's session (milestone BI, task BI.2).
//
// Everything the lookup owns lives here and dies here: one in-flight
// request at a time, the fetched bytes and the parsed passage held only in
// memory, discarded on a new lookup, a cancel or a close. Nothing in this
// class opens a file or installs a cache — the plan's rule is that fetched
// content reaches disk only inside the schedule, and reaching the schedule
// is Add (task BI.3), not the lookup.
#ifndef OPENSHOW_UI_BIBLEIMPORT_H
#define OPENSHOW_UI_BIBLEIMPORT_H

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <vector>

#include "openshow/importer/biblepassage.h"

// The importer's transient state machine: the notice, the in-flight
// request, the parsed verses with their selection, and the failure
// sentences. QML reads it; the importer writes it. A superseded reply
// cannot replace a newer passage — every request carries a sequence
// number and only the current one may speak.
class BibleImport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    // What the operator asked for: a reference ("1 Corinthians 1:1-6")
    // and a version parameter ("NIV"). Setting either clears a stale
    // answer's hold on the UI but not the answer itself until a new
    // lookup completes.
    Q_PROPERTY(QString reference READ reference WRITE setReference NOTIFY stateChanged)
    Q_PROPERTY(QString translation READ translation WRITE setTranslation NOTIFY stateChanged)
    // The answered passage. `attribution` is the translation's licence
    // text; each `verses` row is {reference, text, selected}.
    Q_PROPERTY(QString attribution READ attribution NOTIFY passageChanged)
    Q_PROPERTY(QVariantList verses READ verses NOTIFY passageChanged)
    // Rows for the translation menu: {version, name, attributionNote}.
    Q_PROPERTY(QVariantList translations READ translations CONSTANT)

public:
    explicit BibleImport(QObject* parent = nullptr);

    // A manager to drive requests through. Production passes none and
    // gets a plain QNetworkAccessManager (no cache, ever); tests inject a
    // scripted one. The importer does not take ownership.
    void setNetworkAccessManager(QNetworkAccessManager* manager);

    bool busy() const { return fBusy; }
    QString status() const { return fStatus; }
    QString reference() const { return fReference; }
    QString translation() const { return fTranslation; }
    QString attribution() const { return fAttribution; }
    QVariantList verses() const;
    QVariantList translations() const;

    // The verses the operator kept, in passage order (empty until a
    // passage answers and a selection exists).
    std::vector<openshow::importer::PassageVerse> selectedVerses() const;

    void setReference(const QString& reference);
    void setTranslation(const QString& version);

    Q_INVOKABLE void lookup();
    Q_INVOKABLE void cancel();
    // The plan's retry: the same reference and translation again, from a
    // clean state. Never an old answer replayed.
    Q_INVOKABLE void retry();
    // Discard everything transient — the close/cancel-everything entry.
    Q_INVOKABLE void reset();
    Q_INVOKABLE void setVerseSelected(int index, bool selected);
    // The whole fetched passage or none of it — the menu's quick step.
    Q_INVOKABLE void selectAllVerses(bool selected);

signals:
    void stateChanged();
    void passageChanged();

private:
    struct VerseRow {
        openshow::importer::PassageVerse verse;
        bool selected = true;
    };

    void send(const QUrl& url);
    void failWith(const QString& sentence);
    void finishWith(openshow::importer::PassageParse parse);

    QNetworkAccessManager* fManager = nullptr;
    bool fOwnsManager = false;
    QNetworkReply* fReply = nullptr;
    // Every request stamps the generation it belongs to; a reply from an
    // older generation is deleted in silence.
    quint64 fGeneration = 0;
    bool fBusy = false;
    // Set when the download cap retired a reply, so the finished handler
    // says WHY instead of leaving the status at "looking up".
    bool fSizeCapped = false;
    QString fStatus;
    QString fReference;
    QString fTranslation = QStringLiteral("NIV");
    QString fAttribution;
    std::vector<VerseRow> fVerses;
};

#endif  // OPENSHOW_UI_BIBLEIMPORT_H
