/*
 * openshow-ui: the Qt 6 Quick operator frontend.
 *
 * Task 0.4 gives it a window and the version the shared library reports. The
 * workspace, worker supervision and previews arrive in later milestones.
 */

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>

#include "openshow/domain/version.h"
#include "openshow/protocol/protocol.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("openshow-ui");
    app.setApplicationVersion(QString::fromLatin1(openshow::domain::applicationVersion()));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("applicationVersion", app.applicationVersion());
    engine.rootContext()->setContextProperty(
        "workerOffer",
        QString::fromStdString(openshow::protocol::offerPayload()));

    engine.loadFromModule("OpenShow", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
