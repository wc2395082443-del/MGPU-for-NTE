#include "output_visibility.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::official_fg;
    unsigned checks = 0, failures = 0;
    const auto check = [&](visible_routes routes, bool helper, bool fallback, const char *name) {
        ++checks;
        if (routes.helper != helper || routes.fallback != fallback) { ++failures; std::printf("FAIL %s\n", name); }
    };
    check(choose_visible_routes(true, false, 0, true, true), true, false, "NR routes to official output");
    check(choose_visible_routes(true, false, 1, true, true), false, true, "F7 input view exposes existing same-frame input");
    check(choose_visible_routes(true, false, 2, true, true), false, true, "F7 split view exposes existing compositor");
    check(choose_visible_routes(true, false, 0, true, false), true, true, "NR remains visible while helper prepares");
    check(choose_visible_routes(true, false, 3, true, true), false, false, "invalid mode cannot cover game");
    for (unsigned mode = 0; mode < 3; ++mode) {
        check(choose_visible_routes(false, false, mode, true, true), false, false, "Home hides BOTH output owners");
        check(choose_visible_routes(true, true, mode, true, true), false, false, "F6 peek hides BOTH output owners");
        check(choose_visible_routes(true, false, mode, false, true), false, false, "no output before first NR image");
    }
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
