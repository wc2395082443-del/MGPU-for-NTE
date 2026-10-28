#include "probe_policy.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::official_fg;
    unsigned checked = 0;
    unsigned failures = 0;
    const auto check = [&](bool condition, const char *name) {
        ++checked;
        if (!condition) { ++failures; std::printf("FAIL %s\n", name); }
    };
    check(parse_mode(L"") == probe_mode::invalid, "no implicit GPU initialization");
    check(parse_mode(L"--initialize") == probe_mode::invalid, "unknown mode refused");
    check(parse_mode(L"--verify") == probe_mode::verify, "verification is separate");
    check(parse_mode(L"--initialize-5080") == probe_mode::initialize, "explicit GPU mode");
    check(parse_mode(L"--hidden-swapchain-5080") == probe_mode::hidden_swapchain, "explicit chain mode");
    check(matches_adapter(0x10de, false, L"NVIDIA GeForce RTX 5080"), "5080 selected");
    check(!matches_adapter(0x10de, false, L"NVIDIA GeForce RTX 4080 SUPER"), "4080 refused");
    check(!matches_adapter(0x10de, true, L"NVIDIA GeForce RTX 5080"), "software refused");
    check(!matches_adapter(0x1002, false, L"NVIDIA GeForce RTX 5080"), "wrong vendor refused");
    check(!supports_multiplier(6, 4), "fivefold is not sixfold");
    check(supports_multiplier(6, 5), "sixfold requires five generated");
    check(supports_multiplier(2, 1), "twofold requires one generated");
    check(!supports_multiplier(6, 0), "unknown capability refused");
    check(!supports_multiplier(1, 5), "no silent disable");
    check(!supports_multiplier(7, 6), "unsupported request refused");
    std::printf("%u checks, %u failures\n", checked, failures);
    return failures ? 1 : 0;
}
