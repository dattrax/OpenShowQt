/*
 * Task SI.3 acceptance: the clipboard import PAGE against a real worker.
 *
 * Loads SongImportPage out of the OpenShow module with the real adapters,
 * pastes the synthetic fixture, waits for the stanza-aware plan, proves the
 * cards asked for worker pictures (off air), and clicks the page's own Add to
 * Library — one durable record, then the page closes. No provider, no service
 * addition and no audience present. Needs a display for the worker's output
 * window: desktop preset, label display.
 */

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QTemporaryDir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "CatalogController.h"
#include "DocumentController.h"
#include "SongImport.h"
#include "WorkerController.h"

namespace {

int gFailed = 0;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        ++gFailed;
    }
    return condition;
}

template <typename Predicate>
bool waitFor(Predicate predicate, int ms = 30000) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        if (predicate()) {
            return true;
        }
        QTest::qWait(20);
    }
    return predicate();
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

QQuickItem* findNamed(QQuickItem* root, const char* name) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->objectName() == QLatin1String(name)) {
        return root;
    }
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        return 1;
    }
    const std::filesystem::path root = std::filesystem::path(scratch.path().toStdString());

    openshow::ui::WorkerController worker;
    worker.start();
    const auto measureReady = [&] {
        QVariantMap probe;
        probe.insert(QStringLiteral("key"), QStringLiteral("probe"));
        probe.insert(QStringLiteral("lines"), QVariantList{QStringLiteral("x")});
        probe.insert(QStringLiteral("fontSize"), 40.0);
        probe.insert(QStringLiteral("width"), 200.0);
        probe.insert(QStringLiteral("lineSpacing"), 1.25);
        return worker.measureText(QVariantList{probe}) != 0;
    };
    if (!check(waitFor(measureReady, 30000), "the real worker connected and measures")) {
        worker.shutdownNow();
        return 1;
    }

    openshow::ui::CatalogController library(nullptr, root);
    openshow::ui::DocumentController docs(nullptr, root / "service");
    openshow::ui::SongImport songImport(&library, &docs, &worker);

    const std::string text =
        readFile(OPENSHOW_SOURCE_DIR "/docs/fixtures/songs/morning-light.txt");
    QGuiApplication::clipboard()->setText(QString::fromUtf8(text.c_str()));

    QQmlEngine engine;
    QStringList warnings;
    QObject::connect(&engine, &QQmlEngine::warnings,
                     [&warnings](const QList<QQmlError>& errors) {
                         for (const QQmlError& error : errors) {
                             warnings.append(error.toString());
                         }
                     });
    QQmlContext* context = engine.rootContext();
    context->setContextProperty("worker", &worker);
    context->setContextProperty("docs", &docs);
    context->setContextProperty("library", &library);
    context->setContextProperty("songImport", &songImport);

    QQmlComponent component(&engine);
    component.loadFromModule("OpenShow", "SongImportPage");
    if (!check(!component.isError(), "the import page compiles")) {
        for (const QQmlError& error : component.errors()) {
            std::fprintf(stderr, "  %s\n", qPrintable(error.toString()));
        }
        worker.shutdownNow();
        return 1;
    }
    std::unique_ptr<QObject> pageObject(component.create(context));
    auto* page = qobject_cast<QQuickItem*>(pageObject.get());
    if (!check(page != nullptr, "the import page is created")) {
        worker.shutdownNow();
        return 1;
    }

    QQuickWindow window;
    window.resize(1280, 800);
    page->setParentItem(window.contentItem());
    page->setWidth(1280);
    page->setHeight(800);
    window.show();
    QTest::qWait(80);

    QMetaObject::invokeMethod(pageObject.get(), "askPlanThumbnails");
    // The page itself owns the Paste action (the button's onClicked path).
    auto* paste = findNamed(page, "songPasteButton");
    check(paste != nullptr && page->isVisible(),
          "the page opens with its Paste action visible");
    songImport.pasteFromClipboard();
    check(waitFor([&] {
              return !songImport.measuring() && !songImport.slides().isEmpty();
          }, 30000),
          "the real worker finished the stanza plan");
    QTest::qWait(120);

    auto* titleField = findNamed(page, "songTitleField");
    auto* ccliField = findNamed(page, "songCcliField");
    auto* cards = findNamed(page, "songSlideCollection");
    auto* addButton = findNamed(page, "songAddToLibraryButton");
    check(titleField && titleField->property("text").toString() == "Morning Light",
          "the title field shows the parsed title");
    check(ccliField && ccliField->property("text").toString() == "9000001",
          "the CCLI field shows the parsed number");
    check(cards && cards->property("count").toInt() >= 7,
          "a card exists for every generated slide");
    check(addButton && addButton->property("enabled").toBool(),
          "Add to Library is live after measuring");

    const qint64 framesBefore = worker.thumbnailFrames();
    QMetaObject::invokeMethod(pageObject.get(), "askPlanThumbnails");
    check(waitFor([&] { return worker.thumbnailFrames() > framesBefore; }, 15000),
          "the page's cards asked the worker for real pictures");
    check(!worker.wordsOnAir() && worker.sceneActive() == 0,
          "the import page previews only, never presents");

    if (addButton) {
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                          addButton->mapToScene(
                              QPointF(addButton->width() / 2,
                                      addButton->height() / 2)).toPoint());
    }
    check(waitFor([&] { return library.songCount() == 1; }, 5000),
          "clicking Add wrote exactly one Library record");
    check(library.items().first().toMap().value("ccli").toString() == "9000001",
          "the record carries the CCLI number");
    check(!worker.wordsOnAir() && worker.sceneActive() == 0,
          "Add to Library presented nothing");
    check(warnings.isEmpty(), "the page emitted no QML warnings");
    for (const QString& warning : warnings) {
        std::fprintf(stderr, "  QML: %s\n", qPrintable(warning));
    }

    worker.shutdownNow();
    if (gFailed) {
        std::fprintf(stderr, "song_import_page_tests: FAILED\n");
        return 1;
    }
    std::printf("song_import_page_tests: all checks passed\n");
    return 0;
}
