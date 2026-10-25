// Test data for skia_public_api_guard_negative: this file exists to be
// flagged by CheckNoSkiaPrivate.cmake, proving the guard still catches a
// private include. It is never compiled.
#include "src/core/SkYUVMath.h"
