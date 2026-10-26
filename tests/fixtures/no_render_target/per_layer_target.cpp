// Test data for render_target_guard_negative: a layer allocating its own
// render target inside the composition pass. This file exists to be flagged
// by CheckNoRenderLayer.cmake's second scan, proving that scan still catches
// it. It is never compiled, and it deliberately contains no saveLayer call so
// the two negative tests fail for their own separate reasons.
#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

sk_sp<SkSurface> perLayerTarget(SkCanvas* canvas, const SkImageInfo& info) {
    return canvas->getSurface()->makeSurface(info);
}
