/*
 * Checks that the shared version string is well formed. Fails with a non-zero
 * exit status so ctest reports it.
 */

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

#include "openshow/domain/version.h"

namespace {

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
    return condition;
}

}  // namespace

int main() {
    const std::string_view version = openshow::domain::applicationVersion();
    bool ok = check(!version.empty(), "applicationVersion is not empty");
    ok = check(version.find('.') != std::string_view::npos,
               "applicationVersion contains a dot separator") && ok;
    ok = check(std::isdigit(static_cast<unsigned char>(version.front())) != 0,
               "applicationVersion starts with a digit") && ok;

    if (!ok) {
        return 1;
    }
    std::printf("ok: applicationVersion \"%s\"\n", std::string(version).c_str());
    return 0;
}
