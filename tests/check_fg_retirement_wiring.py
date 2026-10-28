"""M345 ownership and source-pacing guards; not a runtime latency claim."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
addon = (root / "src/dllmain.cpp").read_text(encoding="utf-8")
capture = (root / "src/aligned_input.cpp").read_text(encoding="utf-8")
gate = (root / "experimental/secondary_fg/source_gate.hpp").read_text(encoding="utf-8")
schedule = (root / "src/neural_pipeline.hpp").read_text(encoding="utf-8")
complete = gpu.split("static bool stream_secondary_fg_complete(", 1)[1].split("static void stream_pump_aligned_locked", 1)[0]
publish = gpu.split("bool stream_secondary_fg_publish(", 1)[1].split("void stream_secondary_fg_presented", 1)[0]
local_fg_publish = publish.split("if (t_consumer_poll ||", 1)[1]
callback = gpu.split("void stream_finish_present(", 1)[1].split("// ================= REFLEX", 1)[0]
finish = addon.split("static void on_finish_present(reshade::api::command_queue *queue", 1)[1].split("static void on_present", 1)[0]
poll = gpu.split("void stream_poll()", 1)[1]
checks = {
    "completed FG metadata is immutable in its owned slot":
        "s.fg_completed[output_slot] = output;" in complete and "s.pipeline.current().snapshot" in complete,
    "async completion never mutates the displayed validity":
        "if (!s.async_neural) s.fg_output_valid = output.generated_valid;" in complete,
    "completed GPU job is not held by the previous group": "s.fg_pair.pending()" not in complete,
    "publication is still gated on the old group's end":
        "s.fg_pair.pending()" in publish and "s.pipeline.ready_job()" in publish,
    "publication matches the completed seal before changing slots":
        local_fg_publish.index("output.seal != job.seal") < local_fg_publish.index("s.pipeline.publish_ready()"),
    "ready slots cannot be reallocated":
        "ready_jobs_" in schedule and "slot_busy_[next.snapshot]" in schedule,
    "M358 rollback restores ring prefetch while retaining legacy FG bounds":
        "const unsigned input_budget = s.fg_ready ? mgpu::secondary_fg::source_lead_limit : stream_state::RING;" in gpu
        and "static const unsigned RING = 6;" in gpu,
    "source pacing callback is after native Present only":
        "stream_finish_present" in finish and "addon_event::finish_present" in addon,
    "bridge and unrelated queues never enter the source wait":
        "cmd_queue_v != g_fg_game_queue.load" in callback,
    "source wait holds no stream or capture mutex":
        all(token not in callback for token in ("lock_guard", "unique_lock", "->Wait(")),
    "wait uses submitted color rather than unsubmitted future commands":
        "mgpu::aligned::submitted_color_frame()" in callback
        and capture.index("frame.state.copied[component] = ticket;") < capture.index("latest_color_submission.store"),
    "source progress uses GPU retirement while display remains telemetry":
        "g_fg_source_gate.presented" in gpu and "return completed();" in gate
        and "return shown != 0 ? shown : completed();" not in gate,
    "timeout and consecutive-expiry fail-open are explicit":
        "source_wait_limit_ms" in gate and "consecutive_stalls_ >= 3" in gate and "stalled_.store(true)" in gate,
    "partial progress cannot erase an expired wait":
        "if (progress() == before)" not in gate,
    "wait deadlines use monotonic high resolution":
        "std::chrono::steady_clock" in gate and "GetTickCount64" not in gate,
    "original callback thread and both progress channels are explicitly logged":
        all(token in callback for token in ("callback_tid=", "GetCurrentThreadId()", "gpu_before=", "gpu_after=", "presented_frame=", "begin_ns=", "end_ns=")),
    "shutdown cancels pacing before acquiring the stream mutex":
        gpu.index("g_fg_source_gate.disable();", gpu.index("void stream_shutdown()"))
        < gpu.index("std::lock_guard<std::mutex> lk(s.cs);", gpu.index("void stream_shutdown()")),
    "serial mode transition includes completed-but-unpresented ownership":
        "s.pipeline.pending() || s.pipeline.ready() || s.fg_pair.pending()" in poll,
    "NR and FG reset protections remain":
        "!mgpu::aligned::consecutive_frame" in poll and "neural_reset_pass_mask != 0u" in poll,
    "no output/input precision or factor settings changed":
        'ini_read_flag("SecondaryFG")' in gpu and 'ini_read_sr_int("FGMultiplier"' in gpu,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
