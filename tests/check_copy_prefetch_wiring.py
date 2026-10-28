"""Static regression guards for guide copies recorded before frame arrival."""

import argparse
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("source", nargs="?", type=Path,
                    default=Path(__file__).resolve().parents[1] / "src/gpu1_context.cpp")
args = parser.parse_args()
source = args.source.read_text(encoding="utf-8")
prefetch = source.split("static bool stream_cq_submit(", 1)[1].split("// Bridge thread", 1)[0]
compact_prefetch = " ".join(prefetch.split())
consumer = source.split("const bool depth_here =", 1)[1]
checks = {
    "future depth copy does not depend on an unpublished CPU validity flag":
        "depth_slot_valid" not in prefetch,
    "future motion copy does not depend on an unpublished CPU validity flag":
        "mvec_slot_valid" not in prefetch,
    "depth allocation and mode guards remain":
        "s.depth_mode != 0 && s.tex_depth[ii] != nullptr && s.depth_bytes != 0" in compact_prefetch,
    "motion allocation and mode guards remain":
        "s.mvec_mode == 3 && s.tex_mvec_r[ii] != nullptr && s.mvec_bytes2 != 0" in compact_prefetch,
    "color, depth and motion share the same frame slot":
        "slot_off + SEAL_STRIDE" in prefetch and "slot_off + s.depth_off" in prefetch
        and "slot_off + s.mvec_off" in prefetch and prefetch.count("->CopyTextureRegion(") == 3,
    "color and depth readiness gate their own copies while full arrival gates motion":
        prefetch.index("s.cq->Wait(s.ncolor_ready, UINT64(f))")
        < prefetch.index("s.cq->ExecuteCommandLists(1, color_commands)")
        < prefetch.index("s.cq->Wait(s.ndepth_ready, UINT64(f))")
        < prefetch.index("s.cq->ExecuteCommandLists(1, depth_commands)")
        < prefetch.index("s.cq->Wait(s.nfence, (UINT64)f)")
        < prefetch.index("s.cq->ExecuteCommandLists(1, cls)")
        < prefetch.index("const UINT64 copy_fence = ++s.cqf_value")
        < prefetch.index("s.cq->Signal(s.cqf, copy_fence)"),
    "all three allocators remain protected until full-copy completion":
        prefetch.index("copy_completed < s.cqa_value[ii]")
        < prefetch.index("s.cqa[ii]->Reset()")
        < prefetch.index("s.cq_color_allocators[ii]->Reset()")
        < prefetch.index("s.cq_depth_allocators[ii]->Reset()")
        and prefetch.index("s.pipeline.owns_input(ii)") < prefetch.index("s.cqa[ii]->Reset()"),
    "partial color submission cannot recycle its allocator or continue after a failed full wait":
        prefetch.index("s.cqa_value[ii] = UINT64_MAX")
        < prefetch.index("s.cq->ExecuteCommandLists(1, color_commands)")
        and "if (s.color_copy_overlap) { s.finished = true; g_present_read_fault.store(true); }" in prefetch,
    "partial submission failure at the depth gate faults instead of reusing inputs":
        "s.finished = true;" in prefetch.split("if (FAILED(s.cq->Wait(s.ndepth_ready, UINT64(f))))", 1)[1].split("ID3D12CommandList *depth_commands", 1)[0]
        and "g_present_read_fault.store(true);" in prefetch.split("if (FAILED(s.cq->Wait(s.ndepth_ready, UINT64(f))))", 1)[1].split("ID3D12CommandList *depth_commands", 1)[0],
    "depth uses its own command list with the original region":
        "auto *depth_list = s.guide_copy_overlap ? s.cq_depth_lists[ii] : s.cql[ii]" in prefetch
        and "depth_list->CopyTextureRegion(&ddu, 0, 0, 0, &dsu, nullptr)" in prefetch,
    "consumer still checks current-frame depth and motion validity":
        "s.depth_slot_valid[slot] != 0u" in consumer.split("const bool mvec_here =", 1)[0]
        and "s.mvec_packets.valid(slot, f)" in
        consumer.split("const bool mvec_here =", 1)[1].split("if (mvec_here)", 1)[0],
    "invalid motion is not bound to neural evaluation":
        's.nr_params->Set("DLSSNR.MVec", (ID3D12Resource *)nullptr);' in consumer,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
