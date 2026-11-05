// Focused regressions for the shared controls' actual QML geometry and input.
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlExpression>
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
#include "InspectorAdapter.h"
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
public:
    int requests = 0;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        ++requests;
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
            auto* indicator = verse->property("indicator").value<QQuickItem*>();
            auto* label = verse->property("contentItem").value<QQuickItem*>();
            check(indicator && label && label->x() + label->property("leftPadding").toDouble()
                      >= indicator->x() + indicator->width(),
                  "the checkbox indicator stays left of wrapped verse text instead of covering it");
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
        // 416 is the available centre at the required 1000px shell width.
        // Exercise both the populated form and its empty/retry state.
        for (const int width : {416, 796, 1096}) {
            bible.reset();
            page->setWidth(width);
            window.resize(width, 1000);
            QTest::qWait(30);
            auto* form = page->findChild<QQuickItem*>("bibleLookupForm");
            check(form && form->height() > 0, "the wrapping form reserves all of its rows");
            for (const QString name : {"bibleReferenceField", "bibleTranslationPicker",
                                        "bibleLookupButton", "bibleRetryButton"}) {
                auto* control = page->findChild<QQuickItem*>(name);
                check(control && control->isVisible(), "the empty-state form controls are visible");
                if (!control || !form) continue;
                const QRectF rect(control->mapToItem(form, QPointF()), control->size());
                check(QRectF(0, 0, form->width(), form->height()).contains(rect),
                      "reference, translation, Lookup and Retry stay inside the responsive form");
            }
            auto* lookup = page->findChild<QQuickItem*>("bibleLookupButton");
            const int before = network.requests;
            if (lookup) QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                lookup->mapToScene(QPointF(lookup->width() / 2, lookup->height() / 2)).toPoint());
            for (int i = 0; bible.busy() && i < 100; ++i) QTest::qWait(10);
            check(network.requests == before + 1 && bible.verses().size() == 2,
                  "Lookup is clickable at every supported centre width");
            bible.reset();
            QTest::qWait(20);
            auto* retry = page->findChild<QQuickItem*>("bibleRetryButton");
            if (retry) QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                retry->mapToScene(QPointF(retry->width() / 2, retry->height() / 2)).toPoint());
            for (int i = 0; bible.busy() && i < 100; ++i) QTest::qWait(10);
            check(network.requests == before + 2 && bible.verses().size() == 2,
                  "Retry is clickable at every supported centre width");
        }
        page->setVisible(false);
        check(!scripture.previewActive(), "hiding the actual Bible page releases preview ownership");
        page->setVisible(true);
        check(scripture.previewActive(), "showing the Bible page reacquires preview ownership");
        page->setVisible(false);
    }

    docs.createSong("Inspector controls");
    docs.addSlide();
    docs.createBlankTheme("Inspector fixture");
    QString themeId;
    for (const auto& row : docs.themes()) {
        if (row.toMap().value("name").toString() == "Inspector fixture")
            themeId = row.toMap().value("id").toString();
    }
    openshow::ui::InspectorAdapter adapter;
    adapter.setDocument(&docs);
    for (const QString kind : {QStringLiteral("theme"), QStringLiteral("slide")}) {
        adapter.setTarget({{"kind", kind}, {"id", themeId}});
        QQmlComponent styleComponent(&engine);
        styleComponent.loadFromModule("OpenShow", "TextStyleControls");
        std::unique_ptr<QObject> styleObject(styleComponent.createWithInitialProperties(
            {{"adapter", QVariant::fromValue(&adapter)},
             {"worker", QVariant::fromValue(&worker)}}));
        auto* style = qobject_cast<QQuickItem*>(styleObject.get());
        check(style != nullptr, "the shared typography section loads for both write targets");
        if (!style) continue;
        style->setParentItem(window.contentItem());
        window.resize(400, 1000);
        for (const int width : {232, 282}) {
            style->setWidth(width);
            QTest::qWait(30);
            auto* field = style->findChild<QQuickItem*>("minSizeField");
            auto* reset = style->findChild<QQuickItem*>("minSizeInherit");
            check(field && reset, "minimum-size input and reset exist");
            if (!field || !reset) continue;
            const QRectF rail(0, 0, width, style->height());
            const QRectF fieldRect(field->mapToItem(style, QPointF()), field->size());
            const QRectF resetRect(reset->mapToItem(style, QPointF()), reset->size());
            check(field->width() >= 100 && rail.contains(fieldRect) && rail.contains(resetRect),
                  "minimum-size input and reset fit the actual Themes/Edit rail width");
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                              field->mapToScene(QPointF(20, field->height() / 2)).toPoint());
            QTest::keyClick(&window, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClick(&window, Qt::Key_2);
            QTest::keyClick(&window, Qt::Key_4);
            QTest::keyClick(&window, Qt::Key_Tab);
            check(adapter.fieldState("minFontSize").value("value").toDouble() == 24,
                  "typing in the visible minimum-size field commits to its host target");
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                              reset->mapToScene(QPointF(reset->width() / 2,
                                                        reset->height() / 2)).toPoint());
            check(!adapter.fieldState("minFontSize").value("isSet").toBool(),
                  "the visible minimum-size Inherit button removes the own value");
        }
    }

    QQmlComponent collectionComponent(&engine);
    collectionComponent.setData(R"(
import QtQuick
import OpenShow 1.0
SlidePreviewCollection {
    cellWidth: 210
    height: 460
    delegate: Item {
        required property int index
        required property var modelData
        objectName: "geometryCell"
        width: GridView.view.cellWidth
        height: GridView.view.cellHeight
    }
})", QUrl("qrc:/qt/qml/OpenShow/CollectionGeometryTest.qml"));
    std::unique_ptr<QObject> collectionObject(collectionComponent.create());
    auto* collection = qobject_cast<QQuickItem*>(collectionObject.get());
    check(collection != nullptr, "the actual collection loads with fixed-width cells");
    if (collection) {
        QVariantList source;
        for (int i = 0; i < 100; ++i) source.append(QVariantMap{{"entryId", "one"}, {"id", i}});
        collection->setProperty("source", source);
        collection->setParentItem(window.contentItem());
        for (const int width : {419, 420, 439, 440, 629, 630, 650, 880}) {
            collection->setWidth(width);
            collection->setProperty("contentY", 0);
            QTest::qWait(30);
            check(collection->property("columns").toInt() == width / 210,
                  "mapped columns follow physical cell width at both 210/220 boundaries");
            for (const int offset : {0, 80, 250}) {
                collection->setProperty("contentY", offset);
                QTest::qWait(20);
                for (auto* cell : namedItems(collection, "geometryCell")) {
                    const int index = cell->property("index").toInt();
                    QQmlExpression mapped(engine.rootContext(), collection,
                        QStringLiteral("cueRect(%1)").arg(index));
                    // A QML object-valued result is a QJSValue; read its four
                    // members explicitly rather than duplicating the mapping.
                    const auto value = mapped.evaluate().value<QJSValue>();
                    check(std::abs(value.property("x").toNumber() - cell->x()) < 1 &&
                              std::abs(value.property("y").toNumber() - cell->y()) < 1,
                          "mapped cue coordinates agree with actual delegate placement");
                    const double scrollY = collection->property("contentY").toDouble();
                    if (cell->y() + cell->height() > scrollY &&
                        cell->y() < scrollY + collection->height()) {
                        QQmlExpression band(engine.rootContext(), collection,
                            QStringLiteral("visibleIndexes().indexOf(%1) >= 0").arg(index));
                        check(band.evaluate().toBool(),
                              "every physically visible card belongs to the requested band");
                    }
                }
            }
        }
        collection->setVisible(false);
    }
    QQmlComponent barComponent(&engine);
    barComponent.loadFromModule("OpenShow", "TopBar");
    std::unique_ptr<QObject> barObject(barComponent.create(&pageContext));
    auto* bar = qobject_cast<QQuickItem*>(barObject.get());
    check(bar != nullptr, "the actual header loads with dirty document/output status");
    if (bar) {
        bar->setParentItem(window.contentItem());
        for (const QString mode : {QStringLiteral("show"), QStringLiteral("themes")}) {
            bar->setProperty("page", mode);
            for (const int width : {1000, 1380, 1680}) {
                bar->setWidth(width);
                QTest::qWait(20);
                auto* nav = bar->findChild<QQuickItem*>("toolbarNavigation");
                auto* state = bar->findChild<QQuickItem*>("toolbarOutputState");
                check(nav && state && nav->x() + nav->width() + 8 <= state->x(),
                      "output/unsaved status cannot overlap navigation, including Themes at 1000px");
                check(state && state->x() + state->width() <= bar->width(),
                      "status, emergency Clear and Settings remain inside the header");
            }
        }
    }
    for (const auto& warning : warnings) {
        std::fprintf(stderr, "QML: %s\n", qPrintable(warning));
        failed = true;
    }
    return failed ? 1 : 0;
}
