#include "../src/present_backpressure.hpp"
#include <cstdio>
int main()
{
    using namespace mgpu::present_backpressure;
    unsigned failures = 0;
    const auto check = [&](bool value, const char *name) { std::printf("%s: %s\n", value ? "PASS" : "FAIL", name); if (!value) ++failures; };
    check(!choose(false, true, 3, 10, 1).wait_for_nr, "serial path never adds backpressure");
    check(!choose(true, false, 3, 10, 1).wait_for_nr, "no pending NR does not wait");
    check(choose(true, true, 1, 10, 1).wait_for_nr, "one in-flight frame waits when producer leads");
    check(!choose(true, true, 2, 2, 1).wait_for_nr, "no producer lead stays on base gate");
    const auto pressured = choose(true, true, 2, 5, 2);
    check(pressured.wait_for_nr && pressured.timeout_ms == 25, "producer lead uses bounded NR wait");
    const auto custom = choose(true, true, 4, 9, 3, 7, 31);
    check(custom.wait_for_nr && custom.timeout_ms == 31, "bounded timeout is explicit");
    return failures ? 1 : 0;
}
