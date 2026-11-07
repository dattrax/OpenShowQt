// Negative fixture for CheckNoNdi.cmake. This file MUST trip the guard; it is
// never compiled. The real application reaches NDI only through the separate
// tools/ndi-helper process.
#include <Processing.NDI.Lib.h>

void ndi_symbol_fixture() {
    NDIlib_initialize();
}
