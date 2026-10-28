"""Integration guards; these do not establish visual quality or driver behavior."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
capture = (root / "src/aligned_input.cpp").read_text(encoding="utf-8")
tags = (root / "src/sl_tags.cpp").read_text(encoding="utf-8")
events = (root / "src/dllmain.cpp").read_text(encoding="utf-8")
identity = (root / "src/d3d12_device_identity.hpp").read_text(encoding="utf-8")
submit = capture.split("void STDMETHODCALLTYPE execute(", 1)[1].split("bool allocate_plane", 1)[0]
producer = gpu.split("void stream_on_finish_effects(", 1)[1].split("void stream_mvec_copy(", 1)[0]
checks = {
    "device matching permits native/private-data identity but not LUID alone":
        "context.game_device.matches(device, adapter)" in capture
        and "GetPrivateData(marker_guid, &bytes, &marker)" in identity
        and "marker == reinterpret_cast<std::uintptr_t>(this)" in identity
        and "adapter.LowPart != adapter_.LowPart" in identity,
    "texture rejection exposes the native descriptor":
        "texture_rejection(tag, description, component == 1)" in capture
        and '"tag extent does not cover base mip"' in (root / "src/aligned_texture_policy.hpp").read_text(),
    "original execute is always forwarded before any completion notification":
        submit.index("hook.original(queue, count, lists)") < submit.index("after_submit(queue")
        and "return;" not in submit,
    "untracked conversion queues do not acquire the tracked-submission lock":
        "std::defer_lock" in submit
        and "if (serial_count != 0)\n    {\n        submission_lock.lock();" in submit,
    "hook target has stable lifetime":
        "GET_MODULE_HANDLE_EX_FLAG_PIN" in capture,
    "command reset and actual submission are observed":
        "addon_event::reset_command_list>(on_aligned_reset)" in events
        and "addon_event::execute_command_list>(on_aligned_execute)" in events,
    "unframed tags never use a global last frame":
        "record->identity.viewport != engine.viewport" in capture
        and "no cross-list guess" in capture,
    "all inputs must have actual submission tickets and share constants identity":
        "frame.state.schedulable()" in capture and "constants.key == frame.state.key" in capture,
    "completion cannot reorder recorded engine frames":
        "newer_frame(context.frames[selected].state.key.frame, frame.state.key.frame)" in capture
        and capture.index("selected = index;") < capture.index("frame.state.schedulable()"),
    "depth is scheduled behind capture and downstream copy waits for normalization":
        "context.queues[depth_ticket.queue].fence, depth_ticket.value" in capture
        and "dependencies[1] = {context.depth_converter.fence(), frame.normalization.value}" in capture
        and "context.depth_converter.complete(frame.normalization) && (frame.state.reclaimable(done)" in capture
        and "result.planes[1] = frame.normalized_depth;" in capture,
    "tag-time owned snapshots cannot enter deferred/shared capture paths":
        "target.deferred" not in capture and "shared_plane(" not in capture
        and "set_shared_capture_target(" not in gpu
        and "command->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr)" in capture,
    "two legacy early arrival signals are disabled":
        "if (mgpu::aligned::enabled()) return;" in gpu.split("void stream_on_present(", 1)[1].split("\n}", 1)[0]
        and "if (!aligned_mode && s.produced > 0)" in producer,
    "all aligned copies precede publication and bypass legacy texture copies":
        producer.index("mgpu::aligned::submit_transfer(") < producer.index("mgpu::aligned::commit_transfer(")
        and "s.produced_a.store(fi, std::memory_order_release);\n        return;" in producer,
    "no ring overwrite before the consumer completes":
        "s.produced >= s.consumed + stream_state::RING" in producer,
    "layout is validated before recording outgoing transfer":
        producer.index("mgpu::aligned::matches(") < producer.index("mgpu::aligned::submit_transfer("),
    "serial aligned fallback remains bounded while gated async follows submitted horizon":
        "? (std::min)(completed, s.consumed + 1ull)" in gpu,
    "temporal history advances only after GPU completion":
        gpu.index("s.aligned_history_frame = static_cast<unsigned>(s.slot_game_frame[slot])")
        > gpu.index("neural submission did not complete; stream held"),
    "same-frame normalized motion is converted to motion-subrect pixels, not color pixels":
        "guides.motion_subrect_scale(s.mvec_w, s.mvec_h)" in gpu
        and 'Set("DLSSNR.MVecScaleX", motion_scale[0])' in gpu
        and 'Set("DLSSNR.MVecScaleY", motion_scale[1])' in gpu
        and "guides.motion_scale_x * float(s.width)" not in gpu
        and "guides.motion_scale_y * float(s.height)" not in gpu,
    "constants hook preserves the original call and accepts successful values only":
        "const int result = g_real_constants(values, frame, viewport);" in tags
        and "if (result == 0)" in tags,
    "no partially submitted stream teardown":
        "mgpu::aligned::enabled() && mgpu::aligned::outstanding()" in gpu,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
