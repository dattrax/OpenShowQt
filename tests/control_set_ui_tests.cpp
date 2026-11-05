// Focused regressions for the shared controls' actual QML geometry and input.
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>

#include <cmath>
#include <cstdio>
#include <memory>

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
    for (const auto& warning : warnings) {
        std::fprintf(stderr, "QML: %s\n", qPrintable(warning));
        failed = true;
    }
    return failed ? 1 : 0;
}
