// Second negative form for skia_public_api_guard_negative (task 6.20b):
// the angle-bracket spelling resolves too, because the Skia root itself is
// an exported include directory — the modules' public headers are reached
// as <modules/<m>/include/...> — so the guard must flag this form as it
// flags the quoted one in violation.cpp. It is never compiled.
#include <src/core/SkYUVMath.h>
