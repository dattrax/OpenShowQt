// Focused regressions for the shared controls' actual QML geometry and input.
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QTemporaryDir>
#include <QTimer>

#include "BibleImport.h"
#include "DocumentController.h"
#include "ScriptureImport.h"
#include "WorkerController.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

namespace {
// Deterministic provider markup through the real Bible model; tests never use
// the network. The second verse wraps, exercising populated delegate geometry.
class VerseReply final : public QNetworkReply {
public:
    explicit VerseReply(const QNetworkRequest& request) {
        setRequest(request);
        fBody = R"(<div class="passage-text"><div class="passage-content">
<div class="version-NIV result-text-style-normal text-html">
<p><span class="text John-1-1">In the beginning was the Word.</span></p>
<p><span class="text John-1-2">This longer fixture verse contains enough words to wrap over
several lines inside the compact verse list. Every word must remain visible and
the selection control must use the viewport width rather than its natural text width.</span></p>
</div></div></div><div class="publisher-info-bottom"><p>Synthetic test passage.</p></div>)";
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
            setFinished(true);
            emit finished();
        });
    }
    void abort() override {}
protected:
    qint64 readData(char* data, qint64 maximum) override {
        const auto count = qMin(maximum, fBody.size() - fOffset);
        if (count > 0) std::memcpy(data, fBody.constData() + fOffset, count);
        fOffset += count;
        return count;
    }
private:
    QByteArray fBody;
    qint64 fOffset = 0;
};
class VerseNetwork final : public QNetworkAccessManager {
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        return new VerseReply(request);
    }
};
QList<QQuickItem*> namedItems(QQuickItem* root, const QString& name) {
    QList<QQuickItem*> result;
    if (root->objectName() == name) result.append(root);
    for (auto* child : root->childItems()) result.append(namedItems(child, name));
    return result;
}
}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");
    QQmlEngine engine;
    QStringList warnings;
    QObject::connect(&engine, &QQmlEngine::warnings,
                     [&warnings](const QList<QQmlError>& errors) {
                         for (const auto& error : errors) warnings.append(error.toString());
                     });
    bool failed = false;
    auto check = [&failed](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); failed = true; }
    };

    QQmlComponent component(&engine);
    component.loadFromModule("OpenShow", "SlidePreviewCard");
    std::unique_ptr<QObject> object(component.create());
    auto* card = qobject_cast<QQuickItem*>(object.get());
    check(card != nullptr, "the real shared preview card loads");
    if (!card) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 1;
    }
    QQuickWindow window;
    window.resize(400, 300);
    card->setParentItem(window.contentItem());
    card->setProperty("caption", "John 1:1–4 — the selected generated slide");
    window.show();
    for (const int cellWidth : {120, 150, 190, 210, 240, 300}) {
        card->setWidth(cellWidth - 12);
        card->setHeight((cellWidth - 12) * 9.0 / 16 + 36);
        QTest::qWait(10);
        auto* surface = card->property("surface").value<QQuickItem*>();
        auto* caption = card->findChild<QQuickItem*>("slidePreviewCaption");
        check(surface && caption, "the card exposes its picture and caption parts");
        if (!surface || !caption) continue;
        const QRectF bounds(0, 0, card->width(), card->height());
        const QRectF picture(surface->x(), surface->y(), surface->width(), surface->height());
        const QRectF label(caption->x(), caption->y(), caption->width(), caption->height());
        check(std::abs(surface->width() / surface->height() - 16.0 / 9) < 0.001,
              "the card picture is landscape 16:9 at every thumbnail size");
        check(bounds.contains(picture) && bounds.contains(label),
              "picture and caption stay within their card instead of covering later rows");
        check(label.height() >= 20 && label.top() >= picture.bottom(),
              "the caption retains a readable strip below the picture");
    }

    // The actual Bible page with populated provider data, not just its empty
    // shells. No worker is needed to select verses; pagination tests cover it.
    card->setVisible(false);
    QTemporaryDir scratch;
    openshow::ui::DocumentController docs(nullptr, scratch.path().toStdString());
    openshow::ui::WorkerController worker;
    VerseNetwork network;
    BibleImport bible;
    bible.setNetworkAccessManager(&network);
    openshow::ui::ScriptureImport scripture(&bible, &docs, &worker);
    bible.setReference("John 1:1-2");
    bible.lookup();
    for (int i = 0; bible.busy() && i < 100; ++i) QTest::qWait(10);
    check(bible.verses().size() == 2, "the scripted lookup populates real verse selections");
    QQmlContext pageContext(engine.rootContext());
    pageContext.setContextProperty("docs", &docs);
    pageContext.setContextProperty("worker", &worker);
    pageContext.setContextProperty("bible", &bible);
    pageContext.setContextProperty("scripture", &scripture);
    QQmlComponent pageComponent(&engine);
    pageComponent.loadFromModule("OpenShow", "BibleImportPage");
    std::unique_ptr<QObject> pageObject(pageComponent.create(&pageContext));
    auto* page = qobject_cast<QQuickItem*>(pageObject.get());
    check(page != nullptr, "the populated Bible import page loads");
    if (page) {
        window.resize(900, 700);
        page->setWidth(900);
        page->setHeight(700);
        page->setParentItem(window.contentItem());
        QTest::qWait(80);
        auto* list = page->findChild<QQuickItem*>("bibleVerseList");
        const auto verses = namedItems(page, "bibleVerseCheck");
        check(list && verses.size() == 2, "both verse controls are instantiated in the list");
        for (auto* verse : verses) {
            check(list && std::abs(verse->width() - list->width()) < 1,
                  "populated verse controls use the list viewport width");
        }
        if (verses.size() == 2) {
            check(verses[1]->height() > verses[0]->height(),
                  "the long verse wraps to more lines than the short verse");
            const bool wasSelected = bible.verses()[0].toMap().value("selected").toBool();
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                              verses[0]->mapToScene(QPointF(12, verses[0]->height() / 2)).toPoint());
            check(bible.verses()[0].toMap().value("selected").toBool() != wasSelected,
                  "the visible verse checkbox changes the real source selection");
        }
    }
    for (const auto& warning : warnings) {
        std::fprintf(stderr, "QML: %s\n", qPrintable(warning));
        failed = true;
    }
    return failed ? 1 : 0;
}
