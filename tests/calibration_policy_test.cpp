#include "../src/calibration_policy.hpp"

#include <cstdio>

int main()
{
    using namespace mgpu::calibration;
    unsigned failures = 0;
    const auto check = [&](bool value, const char *name)
    {
        std::printf("%s: %s\n", value ? "PASS" : "FAIL", name);
        if (!value) ++failures;
    };

    mgpu::aligned::guide_constants guides{};
    guides.valid = true;
    guides.key = {12008, 7};
    mgpu::motion_packet::packet motion{9752, 0x1000, 0, {12008, 7, true, true}};
    const auto authoritative = [&](bool aligned_mode, bool depth_valid)
    {
        return authoritative_guides_available(aligned_mode, depth_valid, motion, 9752, 12008, guides);
    };
    check(authoritative(true, true), "current sealed aligned tuple has authoritative guides");
    check(!authoritative(false, true), "legacy route retains calibration with matching metadata");
    check(!authoritative(true, false), "missing guides cannot establish authority");
    motion.seal = 9751;
    check(!authoritative(true, true), "recycled motion packet cannot establish authority");
    motion.seal = 9752;
    motion.source = 0;
    check(!authoritative(true, true), "absent motion cannot establish authority");
    motion.source = 0x1000;
    motion.engine.frame_known = false;
    check(!authoritative(true, true), "unknown source identity cannot establish authority");
    motion.engine.frame_known = true;
    motion.engine.viewport_known = false;
    check(!authoritative(true, true), "unknown viewport cannot establish authority");
    motion.engine.viewport_known = true;
    motion.engine.frame = 12009;
    check(!authoritative(true, true), "neighboring source frame cannot establish authority");
    motion.engine.frame = 12008;
    motion.engine.viewport = 8;
    check(!authoritative(true, true), "another viewport cannot establish authority");
    motion.engine.viewport = 7;
    guides.key.frame = 12007;
    check(!authoritative(true, true), "stale constants cannot establish authority");
    guides.key.frame = 12008;
    guides.valid = false;
    check(!authoritative(true, true), "invalid constants cannot establish authority");
    guides.valid = true;
    check(!authoritative_guides_available(true, true, motion, 9752, 12009, guides),
        "color source must also match the guide frame");
    guides.key.frame = 0;
    motion.engine.frame = 0;
    check(authoritative_guides_available(true, true, motion, 9752, 0, guides),
        "wrapped engine frame zero remains valid with a nonzero seal");

    for (const bool virtual_judge : {false, true})
        for (const bool learning : {false, true})
            for (const bool written : {false, true})
                check(!should_schedule(true, virtual_judge, 0, written, learning, 1050),
                    "authoritative guides bypass every legacy scheduling branch");
    check(should_schedule(false, false, 23, false, false, 2450), "ordinary final sample retained");
    check(!should_schedule(false, false, 24, false, false, 2550), "ordinary 24-sample budget enforced");
    check(should_schedule(false, false, 39, false, true, 4050), "learning final sample retained");
    check(!should_schedule(false, false, 40, false, true, 4150), "learning 40-sample budget enforced");
    check(!should_schedule(false, false, 0, true, false, 150), "persisted ordinary verdict remains closed");
    check(should_schedule(false, true, 23, false, false, 2450), "virtual warmup preserves enough votes");
    check(!should_schedule(false, true, 24, false, false, 2550), "unchanged scale cannot bypass warmup budget");
    check(should_schedule(false, true, 24, true, false, 3050), "virtual fallback retains periodic monitoring");
    check(!should_schedule(false, true, 40, false, true, 4150), "virtual learning budget also bounded");

    unsigned evaluated = 0, old_evaluated = 0, late_windows = 0;
    for (std::uint64_t frame = 150; frame < 20000; frame += 100)
    {
        if (should_schedule(false, true, evaluated, false, false, frame))
        {
            ++evaluated;
            if (frame >= 10000) ++late_windows;
        }
        const unsigned unchanged_scale_verdicts = 0;
        if ((unchanged_scale_verdicts == 0 || frame % 1000 < 100) || old_evaluated < 24)
            ++old_evaluated;
    }
    check(late_windows == 10 && old_evaluated > evaluated * 4,
        "long run reproduces old every-100-frame defect and limits new steady fallback to every 1000");
    return failures == 0 ? 0 : 1;
}
