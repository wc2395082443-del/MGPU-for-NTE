"""Concurrency/ownership guards, not evidence of runtime latency or scanout."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
worker = (root / "src/worker.cpp").read_text(encoding="utf-8")
consumer = (root / "src/async_consumer.hpp").read_text(encoding="utf-8")
feedback = (root / "src/presentation_feedback.hpp").read_text(encoding="utf-8")
schedule = (root / "src/neural_pipeline.hpp").read_text(encoding="utf-8")
publish = gpu.split("bool stream_secondary_fg_publish(", 1)[1].split("void stream_secondary_fg_presented", 1)[0]
poll = gpu.split("void stream_poll()", 1)[1]
shutdown = gpu.split("void stream_shutdown()", 1)[1].split("void ", 1)[0]
checks = {
    "game Streamline session is not rebound or initialized":
        "slSetD3DDevice(" not in gpu and "slInit(" not in gpu,
    "official route does not create a second pacing owner on the fallback chain":
        'constexpr bool own_pacing = false;' in gpu
        and "scd.Flags = swapchain_flags;" in gpu
        and "if (mgpu::official_bridge::selected()) return true;" in gpu,
    "frame-latency manipulation requires proof of application ownership":
        feedback.index("!explicitly_owned") < feedback.index("GetDesc1")
        < feedback.index("SetMaximumFrameLatency(1)"),
    "resize preserves waitable creation flags and refreshes pacing":
        "ResizeBuffers(2, buf_w, buf_h, want_fmt, S.swapchain_flags)" in gpu
        and "presentation_feedback->initialize(sc, true)" in gpu,
    "capacity is acquired before frame selection":
        worker.index("present_capacity_gate(4)") < worker.index("stream_present_gate(4)")
        < worker.index("present_frame(cr"),
    "failed capacity waits cannot busy-spin the presentation thread":
        "Sleep(timeout_ms != 0 ? timeout_ms : 1u);" in gpu.split("bool present_capacity_gate(", 1)[1].split("bool present_frame(", 1)[0],
    "unused capacity credit survives a tick with no ready frame":
        "if (acquired_) return WAIT_OBJECT_0" in feedback,
    "worker service starts only for stable compatible local or official FG":
        "(stream.fg_ready || mgpu::official_bridge::selected()) && stream.async_neural && !stream.finished" in gpu
        and "stream_parallel_compatible(stream)" in gpu,
    "ownership flag precedes new thread creation":
        worker.index("stream_consumer_active(true)") < worker.index("consumer.start("),
    "main worker never polls concurrently with a started consumer":
        "if (!consumer.started()) mgpu::gpu1::stream_poll();" in worker,
    "consumer never publishes or discards the display owner's resources":
        "if (t_consumer_poll ||" in publish,
    "late generated tail completes on its real NR anchor":
        "s.fg_pair.finish_with_real" in publish,
    "only ready GPU-completed outputs can be superseded":
        "s.pipeline.discard_oldest_ready()" in publish
        and "if (ready_count_ < 2) return false" in schedule,
    "generated output is suppressed across a skipped display anchor":
        "output.generated_valid && display_anchors_match" in publish
        and "consecutive_frame(job.engine_frame" in publish,
    "present pointer ownership remains protected across frame copy":
        "s.fg_interpolated[stamp.snapshot][generated_index]" in gpu
        and "s.snapshots.wait_for_read(s.nq, pipeline_slot)" in poll,
    "shutdown defers GPU release while consumer thread is active":
        shutdown.index("g_consumer_shutdown.store(true") < shutdown.index("g_consumer_active.load")
        < shutdown.index("stream.finished = true") < shutdown.index("stream_release()"),
    "worker joins consumer before ordered GPU teardown":
        worker.index("if (!consumer.stop())", worker.index("present_frame(cr")) < worker.index("mgpu::gpu1::stream_shutdown();"),
    "consumer-only poll refuses incompatible reconfiguration":
        "if (t_consumer_poll && (!s.async_neural || (!s.fg_ready && !mgpu::official_bridge::selected()) || s.finished" in poll,
    "consumer cannot execute normal release-summary path":
        "if (!t_consumer_poll && s.finished" in poll,
    "source wake event is not consumed by both threads":
        "!g_consumer_active.load(std::memory_order_acquire) && mgpu::aligned::enabled()" in gpu,
    "timer is private, cancelled after thread exits, without global resolution changes":
        "CREATE_WAITABLE_TIMER_HIGH_RESOLUTION" in consumer and "CancelWaitableTimer" in consumer
        and "timeBeginPeriod" not in consumer,
    "failed join retains state and module code rather than freeing live callbacks":
        "GET_MODULE_HANDLE_EX_FLAG_PIN" in consumer
        and consumer.index("!= WAIT_OBJECT_0) return false") < consumer.index("destroy(state_)"),
    "display feedback stores generated index and handles unavailable statistics":
        "generated_index" in feedback and "if (SUCCEEDED(status)" in feedback
        and "unsupported statistics remain unknown" in gpu,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
