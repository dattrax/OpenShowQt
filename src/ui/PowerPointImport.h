/*
 * PowerPointImport: the renderer sidecar's Qt half (milestone PX).
 *
 * Starts `pptx-render` on a `.pptx`, waits for it off the UI thread, and hands
 * back the pages the process wrote. The cache is keyed by the deck's SHA-256:
 * a repeated import of unchanged bytes reuses the first render. The process is
 * the isolation boundary — a malformed deck can only exit nonzero, never take
 * the operator's session with it.
 *
 * Qt Core only, no GUI: document_controller_tests drives this through the very
 * controller the QML binds to.
 */

#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QProcess;
class QTimer;

namespace openshow::ui {

class PowerPointImport : public QObject {
    Q_OBJECT
public:
    struct Page {
        int page = 0;          // 1-based source page
        QString file;          // file name inside cacheDir
        int width = 0;
        int height = 0;
        int skippedImages = 0;
    };

    struct Deck {
        QString source;        // the deck's file name
        QString cacheDir;      // directory holding the pages and manifest
        QVector<Page> pages;
        QStringList substitutions;  // "Calibri → Carlito" per requested family
        int hiddenPages = 0;
    };

    explicit PowerPointImport(QObject* parent = nullptr);
    ~PowerPointImport() override;

    bool busy() const { return fBusy; }
    // The executable this build uses: OPENSHOW_PPTX_RENDER, then beside the
    // application, then the development cargo output, then PATH.
    QString binaryPath() const;

    // Starts a render. A cache hit finishes on the next event-loop turn, so
    // callers always observe the same signal sequence. Returns false when the
    // deck cannot be read or the renderer is missing; those cases emit failed().
    bool start(const QString& deckPath);
    void cancel();

signals:
    void finished(const openshow::ui::PowerPointImport::Deck& deck);
    void failed(const QString& message);
    void busyChanged();

private:
    void setBusy(bool busy);
    void finishWithDeck(const Deck& deck);
    void failWith(const QString& message);
    bool parseManifest(const QString& directory, Deck* deck, QString* error) const;

    QProcess* fProcess = nullptr;
    QTimer* fTimeout = nullptr;
    bool fBusy = false;
    QString fCacheDir;
    QString fSource;
};

}  // namespace openshow::ui

Q_DECLARE_METATYPE(openshow::ui::PowerPointImport::Deck)
