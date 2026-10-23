/*
 * PreviewView: paints the WorkerController's last copied preview frame
 * (task 1.4). The bytes come from the worker over shared memory; Qt only
 * displays them — nothing in the UI composes slide content itself.
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

public:
    explicit PreviewView(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    WorkerController* controller() const { return fController; }
    void setController(WorkerController* controller);

signals:
    void controllerChanged();

private:
    WorkerController* fController = nullptr;
};

}  // namespace openshow::ui
