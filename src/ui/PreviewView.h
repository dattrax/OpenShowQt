/*
 * PreviewView: paints one target of the worker's shared-memory stream
 * (task 1.4). The bytes come from the worker over shared memory; Qt only
 * displays them — nothing in the UI composes slide content itself.
 *
 * Two targets share the stream (task 6.16a, review R16): `Audience` is the
 * panel's picture, the mirror the worker publishes as it presents;
 * `Selection` is the worker's render of a scene the operator has picked and
 * NOT put on air, which is what the edit canvas draws its handles over. The
 * distinction is the worker's, made in the frame's own metadata — this item
 * picks which image to paint and never assembles one.
 */

#pragma once

#include <QQuickPaintedItem>

#include "WorkerController.h"

namespace openshow::ui {

class PreviewView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(WorkerController* controller READ controller WRITE setController NOTIFY
                   controllerChanged)
    Q_PROPERTY(Target target READ target WRITE setTarget NOTIFY targetChanged)

public:
    // Which of the two pictures on the stream this item shows.
    enum class Target {
        Audience,  // what the panel is being shown (the default)
        Selection  // the slide under edit, rendered off the air
    };
    Q_ENUM(Target)

    explicit PreviewView(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    WorkerController* controller() const { return fController; }
    void setController(WorkerController* controller);

    Target target() const { return fTarget; }
    void setTarget(Target target) {
        if (target == fTarget) {
            return;
        }
        fTarget = target;
        emit targetChanged();
        update();  // the other target's bytes are already in hand
    }

signals:
    void controllerChanged();
    void targetChanged();

private:
    WorkerController* fController = nullptr;
    Target fTarget = Target::Audience;
};

}  // namespace openshow::ui
