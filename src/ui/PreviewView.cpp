#include "PreviewView.h"

#include <QImage>
#include <QPainter>

#include <algorithm>

#include "WorkerController.h"

namespace openshow::ui {

PreviewView::PreviewView(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setFillColor(Qt::black);
}

void PreviewView::setController(WorkerController* controller) {
    if (controller == fController) {
        return;
    }
    if (fController) {
        disconnect(fController, &WorkerController::previewChanged, this, nullptr);
        disconnect(fController, &WorkerController::thumbnailReady, this, nullptr);
    }
    fController = controller;
    if (fController) {
        connect(fController, &WorkerController::previewChanged, this,
                [this] { update(); });
        // A list is a hundred of these items and one arriving picture (task
        // 6.16b), so the repaint is keyed: the controller says WHICH slide got
        // one and every item that is not that row stays as it was painted.
        connect(fController, &WorkerController::thumbnailReady, this,
                [this](const QString& key) {
                    if (fTarget == Target::Thumbnail && key == fKey) {
                        update();
                    }
                });
    }
    emit controllerChanged();
}

void PreviewView::paint(QPainter* painter) {
    painter->fillRect(boundingRect(), Qt::black);
    if (!fController) {
        return;
    }
    // The stream carries three pictures (task 6.16a, and the list's band in
    // 6.16b). Which one this item shows is the whole of its involvement in
    // that: it never assembles or merges them, and an empty frame paints
    // nothing rather than falling back to the audience's — a canvas that
    // quietly showed a different scene under the drag handles, or a slide row
    // wearing its neighbour's picture, is the failure these tasks are about.
    // QML keeps its placeholder for that case.
    QImage image;
    switch (fTarget) {
    case Target::Audience:
        image = fController->previewImage();
        break;
    case Target::Selection:
        image = fController->selectionImage();
        break;
    case Target::Thumbnail:
        image = fController->thumbnail(fKey);
        break;
    }
    if (image.isNull()) {
        return;
    }
    // Aspect-fit, centred — the frame already carries the output rule's
    // letterboxing, so this only fits the pane itself. (drawImage with a
    // QPoint paints the image at native size: first acceptance 2026-10-23
    // showed a 640-wide frame overflowing a 480-wide pane.)
    const QRectF bounds = boundingRect();
    const double scale =
        std::min(bounds.width() / image.width(), bounds.height() / image.height());
    const QSizeF size(image.width() * scale, image.height() * scale);
    const QRectF target((bounds.width() - size.width()) / 2.0,
                        (bounds.height() - size.height()) / 2.0, size.width(),
                        size.height());
    painter->drawImage(target, image);
}

}  // namespace openshow::ui
