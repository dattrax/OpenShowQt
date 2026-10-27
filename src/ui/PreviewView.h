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
    // An accessor pair that is not a Q_PROPERTY is invisible to QML: the C++
    // compiles, the delegate that assigns it does not, and the whole module
    // fails to load — which is how the first version of the slide list's rows
    // broke, found by running the binary rather than by reading it.
    Q_PROPERTY(QString key READ key WRITE setKey NOTIFY keyChanged)

public:
    // Which of the pictures on the stream this item shows.
    enum class Target {
        Audience,  // what the panel is being shown (the default)
        Selection, // the slide under edit, rendered off the air
        Thumbnail  // one slide of the list, rendered off the air and small
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

    // Which slide of the list this item is (task 6.16b): the key the batch was
    // asked with, which is a slide id. It is the only way a per-row item can
    // find its own picture among the frames of a band, and the only way a
    // delegate can be told that ITS row's picture arrived — the controller
    // announces a key, and every item that is not that row stays painted.
    QString key() const { return fKey; }
    void setKey(const QString& key) {
        if (key == fKey) {
            return;
        }
        fKey = key;
        emit keyChanged();
        update();  // the cache may already hold this slide
    }

signals:
    void controllerChanged();
    void targetChanged();
    void keyChanged();

private:
    WorkerController* fController = nullptr;
    Target fTarget = Target::Audience;
    QString fKey;
};

}  // namespace openshow::ui
