"""Integration guards; software evidence only, never an FG quality claim."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
stage = (root / "experimental/secondary_fg/dlss_fg_stage.cpp").read_text(encoding="utf-8")
contract = (root / "experimental/secondary_fg/frame_contract.hpp").read_text(encoding="utf-8")
camera = (root / "experimental/secondary_fg/aligned_camera.hpp").read_text(encoding="utf-8")
parallel = gpu.split("static bool stream_parallel_compatible", 1)[1].split("static void stream_prefetch", 1)[0]
poll = gpu.split("void stream_poll()", 1)[1]
retire = poll.split("while (s.pipeline.pending())", 1)[1].split("if (s.async_neural && !stream_parallel_compatible", 1)[0]
present = gpu.split("ID3D12Resource *stream_present_source(UINT &w", 2)[2].split("bool stream_nr_live()", 1)[0]
aligned_camera = gpu.split("if (aligned_mode && seal.mvec_valid != 0u && depth_ok)", 1)[1].split("else if (!aligned_mode", 1)[0]
checks = {
    "FG preserves compatible async NR": "!s.fg_requested" not in parallel,
    "aligned camera uses the captured constants, not the global latest snapshot":
        "aligned_frame.constants, aligned_frame.key" in aligned_camera and "read_camera" not in aligned_camera,
    "camera identity compares both engine frame and viewport":
        "source.key.frame != expected.frame" in camera and "source.key.viewport != expected.viewport" in camera,
    "FG source identity is independent from camera identity":
        "frame.contract.frame_id = source_frame;" in poll and "frame.contract.camera_frame_id = camera.frame_id;" in poll
        and "s.slot_game_frame[slot] : camera.frame_id" in poll,
    "FG reset includes NR strength and appearance history reset": "neural_reset_pass_mask != 0u" in poll,
    "generated images are per-slot, never a shared output set":
        "fg_interpolated[mgpu::neural_pipeline::schedule::SLOTS]" in gpu
        and "s.fg_interpolated[pipeline_slot][output]" in poll,
    "presentation selects the completed display slot and matching seal":
        "s.fg_interpolated[stamp.snapshot][generated_index]" in present
        and "s.fg_pair.frame_id() == stamp.frame" in present,
    "new recording cannot invalidate the currently displayed generated set":
        "if (s.fg_requested && run_nr) s.fg_output_valid = false" not in poll,
    "retirement requires GPU completion without waiting for display":
        "s.fg_pair.pending()) break;" not in retire
        and retire.index("completion::ready") < retire.index("stream_secondary_fg_complete(s, pending.seal)")
        < retire.index("s.pipeline.retire(completed_fence)"),
    "one pending FG evaluation protects shared NGX parameters and readback":
        "if (s.fg_ready && (s.fg_pending || s.pipeline.pending())) break;" in poll,
    "unsupported FG leaves normal NR capacity and avoids generated allocations":
        "if (s.fg_ready && !stream_secondary_fg_complete(s, pending.seal))" in retire
        and gpu.index("s.fg_stage.check_capability(s.nr_params") < gpu.index("for (auto &outputs : s.fg_interpolated)", gpu.index("bool stream_secondary_fg_create")),
    "async evaluation does not globally wait on the previous display fence":
        "if (!s.async_neural && s.fg_present_fence != nullptr" in poll,
    "GPU display fence gates ALL writes to a reused output slot":
        poll.index("s.snapshots.wait_for_read(s.nq, pipeline_slot)") < poll.index("s.nq->ExecuteCommandLists(1, ls)"),
    "presentation read fences cover generated outputs via the same snapshot stamp":
        gpu.index("stream_snapshot_presented(stamp.snapshot, fence, fence_value)") < gpu.index("hr = sc->Present(pi_interval, 0)"),
    "runtime capability uses unsigned getter and rejects unsupported requests before create":
        stage.index("probe_parameter_get_uint(capabilities") < stage.index("supports_multiplier(multiplier")
        < stage.index("api_.create(commands"),
    "all output formats and UAV flags are validated":
        "frame.interpolated[output].resource->GetDesc().Format != config_.format" in stage,
    "late presentations cannot recursively shift deadlines":
        "(now_ns != 0 ? now_ns : next_due_ns_) +" not in contract,
    "late generated frames are discarded, not shown as an overdue burst":
        "now_ns - next_due_ns_ >= frame_interval_ns_" in contract and "++dropped_generated_" in contract,
    "NR fallback is retained when generation is not valid":
        "s.snapshots.input(stamp.snapshot) : s.snapshots.output(stamp.snapshot)" in present,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
