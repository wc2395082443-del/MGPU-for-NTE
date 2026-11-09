#include "../src/nr_audit_policy.hpp"
#include <cmath>
#include <cstdio>

int main()
{
    using namespace mgpu::nr_audit;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool passed, const char *name) {
        ++checks;
        if (!passed) { ++failures; std::printf("FAIL %s\n", name); }
    };
    budget capture;
    check(!capture.take(1000), "diagnostic remains disabled without explicit trigger");
    check(!capture.observe(0, 1000), "missing trigger cannot arm");
    check(capture.observe(1, 1000), "new trigger arms");
    for (unsigned frame = 0; frame < budget::max_frames; ++frame)
        check(capture.take(1000 + frame), "bounded frame admitted");
    check(!capture.take(2000) && capture.recorded() == 600, "frame cap cannot be exceeded");
    check(!capture.observe(1, 2000) && !capture.take(2000), "unchanged trigger cannot rearm");
    check(capture.observe(2, 3000) && capture.take(22999) && !capture.take(23000), "20 second limit expires even with spare frame budget");
    check(capture.observe(3, 30000) && !capture.take(29999), "clock regression stops diagnostic");
    check(std::abs(brightness(0, 8, false)) < 1e-12, "black code value");
    check(std::abs(brightness(0xffffffffu, 8, false) - 1.0) < 1e-12, "white ignores alpha");
    check(std::abs(brightness(0x000000ffu, 8, false) - 0.2126) < 1e-12, "RGBA red coefficient");
    check(std::abs(brightness(0x00ff0000u, 8, true) - 0.2126) < 1e-12, "BGRA red coefficient");
    check(std::abs(brightness(0x000000ffu, 8, true) - 0.0722) < 1e-12, "BGRA blue coefficient");
    check(std::abs(brightness(1023u, 10, false) - 0.2126) < 1e-12, "10-bit red coefficient");
    check(std::abs(brightness(0x3fffffffu, 10, false) - 1.0) < 1e-12, "10-bit white without alpha");
    check(brightness(0xc0000000u, 10, false) == 0, "10-bit alpha excluded");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
