// Focused regressions for the shared controls' actual QML geometry and input.
#include <QGuiApplication>
#include <QClipboard>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlApplicationEngine>
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
#include <QUrlQuery>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "BibleImport.h"
#include "CatalogController.h"
#include "DocumentController.h"
#include "ServiceCatalogBridge.h"
#include "SongImport.h"
#include "InspectorAdapter.h"
#include "ScriptureImport.h"
#include "WorkerController.h"
#include "AppSettings.h"
#include "WorkspacePanels.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstring>
#include <functional>
#include <filesystem>
#include <memory>

namespace {
// Deterministic provider markup through the real Bible model; tests never use
// the network. Long verses also exercise measured pagination in capture mode.
class VerseReply final : public QNetworkReply {
public:
    explicit VerseReply(const QNetworkRequest& request, bool longPassage, bool replacement) {
        setRequest(request);
        fBody = R"(<div class="passage-text"><div class="passage-content">
<div class="version-NIV result-text-style-normal text-html">
<p><span class="text John-1-1">In the beginning was the Word.</span></p>
<p><span class="text John-1-2">This longer fixture verse contains enough words to wrap over
several lines inside the compact verse list. Every word must remain visible and
the selection control must use the viewport width rather than its natural text width.</span></p>
</div></div></div><div class="publisher-info-bottom"><p>Synthetic test passage.</p></div>)";
        if (longPassage) {
            fBody = "<div class=\"passage-text\"><div class=\"passage-content\">"
                    "<div class=\"version-NIV result-text-style-normal text-html\">";
            for (int i = 1; i <= 6; ++i) {
                fBody += "<p><span class=\"text John-1-" + QByteArray::number(i) + "\">";
                fBody += "Synthetic reading " + QByteArray::number(i) + ": ";
                fBody += "These words demonstrate a longer passage in the selected Bible theme. "
                         "They wrap across the authored reading box, with enough content to "
                         "generate several slides and make the preview collection meaningful.";
                fBody += "</span></p>";
            }
            fBody += "</div></div></div><div class=\"publisher-info-bottom\">"
                     "<p>OpenShowQt synthetic acceptance passage. No provider content used.</p></div>";
        }
        if (replacement) {
            fBody = R"(<div class="passage-text"><div class="passage-content">
<div class="version-NIV result-text-style-normal text-html">
<p><span class="text John-1-3">A replacement passage.</span></p>
</div></div></div><div class="publisher-info-bottom"><p>Synthetic test passage.</p></div>)";
        }
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
    bool longPassage = false;
    bool replacement = false;
    QUrl lastRequest;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override {
        ++requests;
        lastRequest = request.url();
        return new VerseReply(request, longPassage, replacement);
    }
};
QList<QQuickItem*> namedItems(QQuickItem* root, const QString& name) {
    QList<QQuickItem*> result;
    if (root->objectName() == name) result.append(root);
    for (auto* child : root->childItems()) result.append(namedItems(child, name));
    return result;
}

bool waitFor(const std::function<bool()>& predicate, int budget = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < budget) QTest::qWait(20);
    return predicate();
}

bool variedPixels(const QImage& image) {
    if (image.isNull()) return false;
    const auto corner = image.pixel(0, 0);
    int ink = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixel(x, y) != corner && ++ink > 100) return true;
    return false;
}

// Optional reproducible acceptance tour. The ordinary test remains display-free;
// this explicitly requested mode requires the real worker/display/local IPC.
bool captureControlSets(const QString& prefix) {
    bool ok = true;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::fprintf(stderr, "CAPTURE: %s\n", message); ok = false; }
        return condition;
    };
    QTemporaryDir scratch;
    const auto base = std::filesystem::path(scratch.path().toStdString());
    openshow::ui::DocumentController docs(nullptr, base / "service");
    openshow::ui::WorkspacePanels panels(nullptr, base / "workspace.ini");
    openshow::ui::AppSettings settings(nullptr, base / "app.ini");
    openshow::ui::WorkerController worker;
    VerseNetwork network;
    network.longPassage = true;
    BibleImport bible;
    bible.setNetworkAccessManager(&network);
    openshow::ui::ScriptureImport scripture(&bible, &docs, &worker);
    openshow::ui::CatalogController library(nullptr, base);
    openshow::ui::SongImport songImport(&library, &docs, &worker);
    openshow::ui::ServiceCatalogBridge songService(&library, &docs, &worker);
    docs.createSong("Opening song");
    const auto song = docs.currentSongId();
    for (int i = 1; i <= 3; ++i) {
        docs.addSlide();
        docs.setSlideText(QStringLiteral("Welcome to our service\nOpening song — verse %1").arg(i));
        docs.flushText();
    }
    docs.addShowToPlaylist(song);
    docs.createBlankTheme("Warm");
    QString theme;
    for (const auto& row : docs.themes())
        if (row.toMap().value("name").toString() == "Warm") theme = row.toMap().value("id").toString();
    check(docs.editTheme(theme, "Warm", "#ffd23f", "#101828", "", "DejaVu Serif",
                         64, "left", 1.25) && docs.useThemeAsCurrent(theme), "configure the capture theme");
    check(docs.setThemePlacement(theme, 160, 100, 1600, 880), "configure the song placement");
    check(docs.applyThemeContext(theme, "scripture", {{"fontSize", 48.0}, {"box", QVariantMap{
        {"x", 180.0}, {"y", 100.0}, {"width", 1200.0}, {"height", 650.0}}}}),
        "configure a distinct Bible view");
    if (panels.drawerHeight() > 40) panels.toggleDrawer();
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, [&](const QList<QQmlError>& errors) {
        for (const auto& error : errors) {
            // Same narrow StandardKey-alias exclusion as workspace_ui_tests;
            // every other warning, including delegate/binding errors, fails.
            if (error.toString().contains("Shortcut: Only binding to one of multiple key bindings"))
                continue;
            std::fprintf(stderr, "CAPTURE QML: %s\n", qPrintable(error.toString()));
            ok = false;
        }
    });
    auto* context = engine.rootContext();
    context->setContextProperty("applicationVersion", "acceptance");
    context->setContextProperty("workerOffer", "acceptance");
    context->setContextProperty("worker", &worker);
    context->setContextProperty("docs", &docs);
    context->setContextProperty("panels", &panels);
    context->setContextProperty("appSettings", &settings);
    context->setContextProperty("bible", &bible);
    context->setContextProperty("scripture", &scripture);
    context->setContextProperty("library", &library);
    context->setContextProperty("songImport", &songImport);
    context->setContextProperty("songService", &songService);
    context->setContextProperty("uiSize", QSize(1680, 1020));
    engine.loadFromModule("OpenShow", "Main");
    auto* window = engine.rootObjects().isEmpty() ? nullptr
        : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (!check(window != nullptr, "load the production workspace")) return false;
    worker.start();
    if (!check(waitFor([&] { return worker.outputLive() && worker.selectionSupported() &&
                                   worker.thumbnailSupported(); }), "the real worker is ready")) return false;
    worker.refreshFontFamilies();
    check(waitFor([&] { return worker.fontListState() == "ready"; }),
          "the renderer's installed font list is ready for the inspector captures");
    const auto lookup = [&] {
        window->setProperty("page", "bible");
        bible.setReference("John 1:1-6");
        bible.lookup();
        return waitFor([&] { return !bible.busy() && !scripture.measuring() &&
                                   scripture.slides().size() > 1; });
    };
    if (!check(lookup(), "populate and paginate a multi-slide Bible fixture")) return false;
    const auto oldPlanKey = scripture.previewKey(0);
    network.replacement = true;
    bible.setReference("John 1:3");
    check(!scripture.canAdd() && scripture.slides().isEmpty(),
          "changing the reference retires the old plan before another Add");
    bible.lookup();
    check(waitFor([&] { return !bible.busy() && !scripture.measuring()
        && scripture.slides().size() == 1; }), "the replacement passage is measured without a selection step");
    check(scripture.previewKey(0) != oldPlanKey
          && scripture.previewSceneJson(0).contains("A replacement passage.")
          && !scripture.previewActive(), "replacement publishes a fresh thumbnail plan without a Selection render");
    network.replacement = false;
    if (!check(lookup(), "restore the complete multi-slide fixture")) return false;
    const int bibleSlides = scripture.slides().size();
    check(scripture.addToSchedule(), "add one Bible show for grouped Show captures");
    QDir().mkpath(QFileInfo(prefix).absolutePath());
    QJsonArray manifest;
    auto save = [&](const QString& mode, const QSize& size) {
        QTest::qWait(80);
        const auto image = window->grabWindow();
        const auto path = prefix + "." + mode + "-" + QString::number(size.width()) + ".png";
        check(!image.isNull() && image.save(path), "save a real workspace frame");
        manifest.append(QJsonObject{{"page", mode}, {"logicalWidth", size.width()},
            {"logicalHeight", size.height()}, {"pixelWidth", image.width()}, {"pixelHeight", image.height()},
            {"bibleSlides", bibleSlides}, {"selectionFrames", worker.selectionFrames()},
            {"thumbnailFrames", worker.thumbnailFrames()}, {"path", path}});
        check(!worker.wordsOnAir() && worker.sceneActive() == 0,
              "all capture browsing remains off air with the audience unchanged");
    };
    const QList<QSize> sizes{{1680, 1020}, {1380, 800}, {1000, 680}};
    window->setProperty("page", "show");
    for (const auto size : sizes) {
        window->resize(size);
        QTest::qWait(100);
        check(waitFor([&] {
            const auto cards = namedItems(window->contentItem(), "operatorCueTile");
            auto* grid = window->findChild<QQuickItem*>("operatorSlideGrid");
            if (cards.isEmpty() || !grid) return false;
            const QRectF viewport(grid->mapToScene(QPointF()), grid->size());
            int visibleCards = 0;
            for (auto* card : cards) {
                // GridView retains delegates outside its clipped viewport;
                // those items are isVisible(), but intentionally ask no pixels.
                if (!card->isVisible()
                        || !viewport.intersects(QRectF(card->mapToScene(QPointF()), card->size()))) continue;
                ++visibleCards;
                auto* surface = card->property("surface").value<QQuickItem*>();
                if (!surface || !variedPixels(worker.thumbnail(surface->property("key").toString()))) return false;
            }
            return visibleCards > 0;
        }), "every visible Show card has real rendered words");
        save("show", size);
    }
    if (!check(lookup(), "repopulate the import for Bible captures")) return false;
    for (const auto size : sizes) {
        window->resize(size);
        QTest::qWait(100);
        check(waitFor([&] { return !worker.thumbnail(scripture.previewKey(0)).isNull() &&
            !worker.thumbnail(scripture.previewKey(scripture.slides().size() - 1)).isNull(); }),
            "Bible cards carry real current-plan pixels");
        auto* page = window->findChild<QQuickItem*>("bibleImportPage");
        auto* scroll = page ? page->property("contentItem").value<QQuickItem*>() : nullptr;
        if (scroll) scroll->setProperty("contentY", 0);
        save("bible", size);
        if (scroll) scroll->setProperty("contentY", qMax(0.0,
            scroll->property("contentHeight").toDouble() - scroll->height()));
        save("bible-actions", size);
    }
    window->setProperty("page", "edit");
    docs.selectSong(song);
    docs.selectSlide(0);
    for (const auto size : sizes) {
        window->resize(size);
        check(waitFor([&] { return worker.selectionCurrent() && variedPixels(worker.selectionImage()); }),
              "Edit has its own worker-rendered slide");
        auto* page = window->findChild<QQuickItem*>("editPage");
        if (page) { page->setProperty("contentX", 0); page->setProperty("contentY", 0); }
        save("edit", size);
        if (page) page->setProperty("contentX", qMax(0.0,
            page->property("contentWidth").toDouble() - page->width()));
        save("edit-inspector", size);
    }
    window->setProperty("page", "themes");
    auto* themes = window->findChild<QQuickItem*>("themeEditorPage");
    check(themes != nullptr, "locate the Themes host");
    if (themes) themes->setProperty("selectedId", theme);
    for (const auto size : sizes) {
        window->resize(size);
        for (const QString view : {QString(), QStringLiteral("scripture")}) {
            const bool changedView = themes && themes->property("selectedView").toString() != view;
            const auto beforeFrames = worker.selectionFrames();
            if (themes) QMetaObject::invokeMethod(themes, "selectView", Q_ARG(QVariant, view));
            QTest::qWait(150);  // allow the production 120-ms selection debounce
            check(waitFor([&] { return worker.selectionCurrent() && variedPixels(worker.selectionImage())
                && (!changedView || worker.selectionFrames() > beforeFrames); }),
                  "the selected theme view has actual worker pixels");
            auto* box = window->findChild<QQuickItem*>("themeEditorPlacementBox");
            const auto handles = namedItems(window->contentItem(), "themeEditorPlacementHandle");
            check(box && handles.size() == 4, "theme capture has four placement handles");
            if (box) for (auto* handle : handles) {
                const auto center = handle->mapToItem(box, QPointF(handle->width() / 2, handle->height() / 2));
                check((qAbs(center.x()) < 0.01 || qAbs(center.x() - box->width()) < 0.01)
                      && (qAbs(center.y()) < 0.01 || qAbs(center.y() - box->height()) < 0.01),
                      "handles track the rectangle through both views, window sizes and display scales");
            }
            if (!view.isEmpty()) {
                auto* browse = window->findChild<QQuickItem*>("themeEditorContextFontBrowse");
                auto* sizeField = window->findChild<QQuickItem*>("themeEditorContextSize");
                check(browse && browse->isVisible() && browse->isEnabled()
                      && sizeField && sizeField->property("text").toString() == "48",
                      "Bible has the worker font browser and its own size in the actual inspector");
            }
            save(view.isEmpty() ? "themes" : "themes-bible", size);
        }
    }
    auto* resetBible = window->findChild<QQuickItem*>("themeEditorContextReset");
    check(resetBible && resetBible->isEnabled(), "the Bible override can be reset through its visible action");
    if (resetBible) QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
        resetBible->mapToScene(QPointF(resetBible->width() / 2, resetBible->height() / 2)).toPoint());
    for (const auto size : sizes) {
        window->resize(size);
        QTest::qWait(150);
        check(waitFor([&] { return worker.selectionCurrent() && variedPixels(worker.selectionImage()); }),
              "the inherited Bible appearance is rendered after Reset Bible");
        auto* fields = window->findChild<QQuickItem*>("themeEditorContextFields");
        auto* sizeField = window->findChild<QQuickItem*>("themeEditorContextSize");
        auto* browse = window->findChild<QQuickItem*>("themeEditorContextFontBrowse");
        check(!docs.themeContext(theme, "scripture").value("set").toBool()
                  && fields && fields->isVisible() && sizeField && sizeField->isVisible()
                  && sizeField->property("text").toString() == "64"
                  && browse && browse->isEnabled()
                  && !window->findChild<QQuickItem*>("themeEditorBoxLabel"),
              "Bible controls stay visible and editable without a context, and no box label is painted");
        save("themes-bible-inherited", size);
    }
    // --- tasks SI.3/SI.4/SL.6: the import page on the REAL worker ---------
    {
        std::ifstream songFile(OPENSHOW_SOURCE_DIR
                               "/docs/fixtures/songs/morning-light.txt");
        std::ostringstream songText;
        songText << songFile.rdbuf();
        QGuiApplication::clipboard()->setText(QString::fromUtf8(songText.str().c_str()));
        songImport.pasteFromClipboard();
        check(waitFor([&] { return !songImport.measuring() &&
                                  !songImport.slides().isEmpty(); }, 30000),
              "the real worker measures the pasted song");
        check(songImport.canAdd(), "the measured plan wakes Add");
        check(songImport.slides().size() >= 7,
              "every section produced at least one slide");
        check(songImport.ccli() == "9000001" &&
                  songImport.authors().contains("Example Writer"),
              "the page carries the CCLI and author fields");
        window->setProperty("page", "songimport");
        QTest::qWait(150);
        auto* page = window->findChild<QQuickItem*>("songImportPage");
        auto* titleField = window->findChild<QQuickItem*>("songTitleField");
        auto* addButton = window->findChild<QQuickItem*>("songAddToLibraryButton");
        check(page && page->isVisible(), "the import page opens");
        check(titleField && titleField->property("text").toString() == "Morning Light",
              "the title field shows the parsed title");
        check(addButton && addButton->property("enabled").toBool(),
              "Add to Library is live after measuring");
        const qint64 framesBefore = worker.thumbnailFrames();
        QMetaObject::invokeMethod(page, "askPlanThumbnails");
        check(waitFor([&] { return worker.thumbnailFrames() > framesBefore; }, 15000),
              "the page's cards asked for real worker pictures");
        check(!worker.wordsOnAir() && worker.sceneActive() == 0,
              "the import page previews off air only");
        for (const auto size : sizes) {
            window->resize(size);
            QTest::qWait(150);
            save("song-import", size);
        }
        // Return to a comfortable size and scroll the page's own ScrollView
        // so the Add row is inside the viewport before the real click.
        window->resize(QSize(1680, 1020));
        QTest::qWait(150);
        if (page) {
            const qreal maxY = page->property("contentHeight").toReal()
                               - page->property("height").toReal();
            page->setProperty("contentY", qMax(0.0, maxY));
            QTest::qWait(120);
        }
        if (addButton) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              addButton->mapToScene(
                                  QPointF(addButton->width() / 2,
                                          addButton->height() / 2)).toPoint());
        }
        check(waitFor([&] { return library.songCount() == 1; }, 5000),
              "Add to Library wrote exactly one record");
        check(window->property("page").toString() == "edit",
              "Add returns to the editing page");
        const QString songId =
            library.songCount() > 0
                ? library.items().first().toMap().value("id").toString()
                : QString();
        check(!songId.isEmpty(), "the added song has its id");
        if (!songId.isEmpty()) {
            check(songService.addToService(songId),
                  "the song starts preparing for the service");
            check(waitFor([&] { return !songService.busy(); }, 30000),
                  "the service preparation finished");
            check(!docs.showForCatalogItem(songId).isEmpty(),
                  "the running order references the catalog song");
        }
        check(!worker.wordsOnAir() && worker.sceneActive() == 0,
              "the whole import journey presented nothing");
    }

    QFile output(prefix + ".manifest.json");
    check(output.open(QIODevice::WriteOnly), "open the capture manifest");
    output.write(QJsonDocument(manifest).toJson());
    worker.shutdownNow();
    return ok;
}
}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");
    // Capture mode owns one production engine/window, separate from the unit
    // component lifetimes and scratch models below.
    const auto capturePrefix = qEnvironmentVariable("OPENSHOW_CONTROL_CAPTURE");
    if (!capturePrefix.isEmpty()) return captureControlSets(capturePrefix) ? 0 : 1;
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

    // Section colours and captions belong to operator chrome; repeated
    // labels disappear while the thumbnail and coloured strip remain.
    auto* strip = card->findChild<QQuickItem*>("slidePreviewCaption");
    auto* captionText = card->findChild<QQuickItem*>("slidePreviewCaptionText");
    auto* cardSurface = card->property("surface").value<QQuickItem*>();
    cardSurface->setProperty("placeholder", "All lyric words remain visible");
    QStringList palette;
    for (const auto& label : {"Verse 1", "Chorus", "Bridge", "Pre-Chorus", "Post-Chorus", "Blank", "Ending"}) {
        card->setProperty("sectionLabel", label);
        const auto color = card->property("sectionColor").toString();
        check(!palette.contains(color), "major song section kinds have distinct muted colours");
        palette.append(color);
        check(card->property("displayCaption").toString() == label,
              "a song card labels its section instead of repeating the first lyric");
    }
    card->setProperty("sectionSuffix", "Synthetic song");
    check(card->property("displayCaption").toString() == "Ending – Synthetic song",
          "the section caption includes the song title without a numeric prefix");
    card->setProperty("sectionLabel", "CHORUS");
    check(card->property("sectionTitle").toString() == "Chorus", "section labels have consistent display casing");
    card->setProperty("showSectionLabel", false);
    check(captionText && !captionText->isVisible() && card->property("displayCaption").toString().isEmpty()
              && strip->isVisible() && cardSurface->property("placeholder").toString() == "All lyric words remain visible",
          "a continuation hides only its caption text, retaining its section colour and lyric picture");
    card->setProperty("live", true);
    check(strip->property("color").toString() == "#7c2c28", "the live marker still outranks section colours");
    card->setProperty("live", false);
    card->setProperty("showSectionLabel", true);
    card->setProperty("sectionLabel", "");
    card->setProperty("sectionSuffix", "");
    check(card->property("displayCaption") == card->property("caption"),
          "Bible and ordinary presentation captions retain their existing text");

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
        check(!page->findChild<QQuickItem*>("bibleVerseList")
                  && namedItems(page, "bibleVerseCheck").isEmpty()
                  && !page->findChild<QQuickItem*>("biblePreviewCanvas"),
              "the import page shows one preview collection without verse checkboxes or a second canvas");
        check(!scripture.previewActive(), "the thumbnail-only page does not own Selection");
        // 416 is the available centre at the required 1000px shell width.
        // Lookup also replaces a populated answer using the current field text.
        for (const int width : {416, 796, 1096}) {
            bible.reset();
            network.replacement = false;
            bible.setReference("John 1:1-2");
            auto* reference = page->findChild<QQuickItem*>("bibleReferenceField");
            if (reference) reference->setProperty("text", "John 1:1-2");
            page->setWidth(width);
            window.resize(width, 1000);
            QTest::qWait(30);
            auto* form = page->findChild<QQuickItem*>("bibleLookupForm");
            check(form && form->height() > 0, "the wrapping form reserves all of its rows");
            for (const QString name : {"bibleReferenceField", "bibleTranslationPicker",
                                        "bibleLookupButton"}) {
                auto* control = page->findChild<QQuickItem*>(name);
                check(control && control->isVisible(), "the empty-state form controls are visible");
                if (!control || !form) continue;
                const QRectF rect(control->mapToItem(form, QPointF()), control->size());
                check(QRectF(0, 0, form->width(), form->height()).contains(rect),
                      "reference, translation and Lookup stay inside the responsive form");
            }
            auto* lookup = page->findChild<QQuickItem*>("bibleLookupButton");
            const int before = network.requests;
            if (lookup) QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                lookup->mapToScene(QPointF(lookup->width() / 2, lookup->height() / 2)).toPoint());
            for (int i = 0; bible.busy() && i < 100; ++i) QTest::qWait(10);
            check(network.requests == before + 1 && bible.verses().size() == 2,
                  "Lookup is clickable at every supported centre width");
            network.replacement = true;
            if (reference) reference->setProperty("text", "John 1:3");
            QTest::qWait(20);
            if (lookup) QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                lookup->mapToScene(QPointF(lookup->width() / 2, lookup->height() / 2)).toPoint());
            for (int i = 0; bible.busy() && i < 100; ++i) QTest::qWait(10);
            check(network.requests == before + 2 && bible.reference() == "John 1:3"
                      && QUrlQuery(network.lastRequest).queryItemValue("search") == "John 1:3"
                      && bible.verses().size() == 1
                      && bible.verses()[0].toMap().value("text") == "A replacement passage."
                      && bible.selectedVerses().size() == 1,
                  "typing a new reference and clicking Lookup replaces the whole passage without selection");
        }
        page->setVisible(false);
        check(!scripture.previewActive(), "hiding the actual Bible page releases preview ownership");
        page->setVisible(true);
        check(!scripture.previewActive(), "showing the Bible page still leaves Selection unused");
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
