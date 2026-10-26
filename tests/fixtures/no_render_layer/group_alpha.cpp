// Test data for render_layer_guard_negative: production code bringing a
// group-alpha layer back into the composition pass. This file exists to be
// flagged by CheckNoRenderLayer.cmake, proving the saveLayer scan still
// catches it. It is never compiled.
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

void fadeWords(SkCanvas* canvas, const SkPaint& fade) {
    canvas->saveLayer(SkRect::MakeWH(1920, 1080), &fade);
}
