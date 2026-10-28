"""Worker ownership and nonblocking delivery guards; not a runtime FPS test."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
worker = (root / "src/worker.cpp").read_text(encoding="utf-8")
callback = gpu.split("void stream_on_finish_effects(", 1)[1].split(
    "static void stream_produce_locked(", 1
)[0]
produce = gpu.split("static void stream_produce_locked(", 2)[2].split("void stream_mvec_copy(", 1)[0]
pump = gpu.split("static void stream_pump_aligned_locked(", 1)[1].split("static bool stream_parallel_compatible", 1)[0]
poll = gpu.split("void stream_poll()", 1)[1]
aligned_transfer = produce.split("const HRESULT transfer = mgpu::aligned::submit_transfer(", 1)[1].split(
    "if (t119_pd) { QueryPerformanceCounter", 1
)[0]
checks = {
    "armed aligned producer no longer waits for finish-effects":
        callback.index("if (s.armed) return;")
        < callback.index("mgpu::aligned::acquire(aligned_frame)"),
    "initial arming still occurs only after game adapter and queue validation":
        callback.index("ll.LowPart != want.LowPart") < callback.index("mgpu::aligned::observe_queue(gq)")
        < callback.index("if (!s.armed)"),
    "worker pump holds the stream mutex and precedes nonblocking neural retirement":
        poll.index("std::unique_lock<std::mutex> lk(s.cs)") < poll.index("stream_pump_aligned_locked(s);")
        < poll.index("while (s.pipeline.pending())") < poll.index("completion::waiting"),
    "stopped, unarmed and incompatible guide streams cannot dispatch":
        all(guard in pump for guard in ["!s.requested", "!s.armed", "s.finished", "s.summarised",
                                        "s.depth_mode == 0", "s.mvec_mode != 3"]),
    "secondary FG does not disable the worker-owned dispatch route": "s.fg_requested" not in pump,
    "ring backpressure is checked before acquiring a source":
        pump.index("s.produced < s.consumed + input_budget") < pump.index("mgpu::aligned::acquire(frame)"),
    "producer frame bound shuts down capture without requiring another game callback":
        pump.index("s.produced >= s.max_frames") < pump.index("mgpu::aligned::acquire(frame)")
        and "s.mvec_live.store(false" in pump,
    "worker dispatch uses owned buffers, never a retained game command list":
        "stream_produce_locked(s, nullptr, nullptr, nullptr," in pump
        and all(f"frame.planes[{index}].buffer" in pump for index in range(3)),
    "bounded worker batch drains at most three owned inputs per poll":
        pump.count("mgpu::aligned::acquire(frame)") == 1 and pump.count("stream_produce_locked(s,") == 1
        and "constexpr unsigned max_per_poll = 3" in pump
        and "while (dispatched < max_per_poll" in pump
        and "s.produced == before" in pump,
    "worker pump cannot block on a GPU wait or create a thread":
        all(token not in pump for token in ["WaitFor", "->Wait(", "Sleep(", "std::thread", "CreateThread"]),
    "worker keeps existing four-ms fence wait rather than adding a busy loop":
        "stream_present_gate(4)" in worker and worker.index("stream_poll();") < worker.index("stream_present_gate(4)"),
    "one shared producer handles layout, seal, submit, and publication":
        produce.count("mgpu::aligned::submit_transfer(") == 1
        and produce.index("mgpu::aligned::matches(") < produce.index("mgpu::aligned::submit_transfer("),
    "busy copy-queue retries do not prematurely close the aligned motion packet":
        aligned_transfer.index("if (FAILED(transfer))") < aligned_transfer.index("s.mvec_packets.recorded(")
        < aligned_transfer.index("s.mvec_packets.close(fi)") < aligned_transfer.index("mgpu::aligned::commit_transfer(")
        < aligned_transfer.index("s.produced_a.store("),
    "legacy game-list work is unreachable from an aligned dispatch":
        produce.index("s.produced_a.store(fi, std::memory_order_release);\n        return;")
        < produce.index("gl->CopyBufferRegion(s.gxfer"),
    "invalid legacy arguments fail closed before command-list use":
        "if (!aligned_mode && (gl == nullptr || gq == nullptr || src == nullptr)) return;" in produce,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
