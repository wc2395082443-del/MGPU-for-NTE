#include "../src/admission_audit.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::admission;
    trace capture;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool passed, const char *name) {
        ++checks;
        if (!passed) { ++failures; std::printf("FAIL %s\n", name); }
    };
    check(capture.note(1, 1, 1000) == nullptr, "no trace without trigger");
    check(!capture.begin(0, 1000) && capture.begin(7, 1000), "explicit nonzero token required");
    auto *frame = capture.note(100, 1, 1000);
    if (!frame) return 2;
    frame->seen |= 1;
    frame->reasons |= no_capture_slot;
    ++frame->held_calls;
    auto *repeated = capture.note(100, 1, 1001);
    check(repeated == frame && capture.count() == 1, "multiple component calls share one unique source identity");
    repeated->seen |= 6;
    repeated->recorded |= 7;
    repeated->constants_seen = repeated->constants_valid_seen = true;
    repeated->acquired = repeated->delivered = true;
    repeated->seal = 9;
    check(frame->seen == 7 && frame->recorded == 7 && frame->delivered, "a transient full slot does not become a falsely dropped frame");
    check(capture.note(100 + trace::capacity, 1, 1002) == nullptr && capture.overflow() == 1, "hash collision reported without corrupting original identity");
    check(capture.note(100, 2, 1002) == nullptr && capture.overflow() == 2, "different viewport cannot borrow source row");
    check(capture.note(0, 1, 1002) == nullptr, "missing frame not fabricated");
    check(capture.note(101, 1, 20999) != nullptr && !capture.active(21000), "trace expires at twenty seconds");
    check(capture.note(102, 1, 21001) == nullptr, "late source events cannot extend measurement");
    check(!capture.begin(8, 21001), "unexported evidence cannot be overwritten");
    row output;
    unsigned delivered = 0, exported = 0;
    while (capture.pop(output)) {
        ++exported;
        if (output.delivered) ++delivered;
    }
    check(exported == 2 && delivered == 1, "flushing preserves dropped and delivered identities separately");
    check(!capture.begin(7, 22000) && capture.begin(8, 22000), "only changed trigger rearms after flush");
    for (unsigned index = 1; index <= trace::capacity; ++index)
        check(capture.note(index, 1, 22001) != nullptr, "bounded table accepts unique source identities");
    check(capture.count() == trace::capacity && capture.note(4097, 1, 22002) == nullptr, "table memory remains bounded");
    check(!capture.active(1), "clock regression stops tracing");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
