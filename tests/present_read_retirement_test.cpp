#include "../src/present_read_retirement.hpp"
#include <cstdio>

int main()
{
    unsigned failures = 0;
    const auto check = [&failures](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    using mgpu::present_read::retirement;
    using mgpu::present_read::status;
    retirement reads;
    check(reads.check(0) == status::retired, "no submitted read needs no wait");
    reads.submitted(7, true);
    check(reads.check(6) == status::pending, "NR-only read blocks overwrite while GPU copy is delayed");
    check(reads.check(6) == status::pending, "Present return does not retire a GPU read");
    check(reads.check(6) == status::pending, "failed or occluded Present does not cancel its submitted copy");
    check(reads.check(7) == status::retired, "completion at the read fence permits reuse");
    reads.submitted(8, true);
    check(reads.check(7) == status::pending, "repeated presentation extends the output lifetime");
    reads.submitted(5, true);
    check(reads.value() == 8 && reads.check(7) == status::pending, "older values cannot regress retirement");
    check(reads.check(8) == status::retired, "teardown can proceed after the last read");
    check(reads.check(~std::uint64_t{0}) == status::unsafe, "device removal is not successful retirement");
    retirement failed_signal;
    failed_signal.submitted(9, false);
    check(failed_signal.check(100) == status::unsafe, "unfenced submission forbids reuse and release");
    failed_signal.submitted(10, true);
    check(failed_signal.check(100) == status::unsafe, "later signals cannot hide an unfenced submission");
    retirement invalid_signal;
    invalid_signal.submitted(0, true);
    check(invalid_signal.check(0) == status::unsafe, "zero signal value is not proof of completion");
    return failures == 0 ? 0 : 1;
}
