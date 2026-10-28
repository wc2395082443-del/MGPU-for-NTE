"""M342 scheduling and lifetime guards, not driver-overlap or FPS evidence."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
capture = (root / "src/aligned_input.cpp").read_text(encoding="utf-8")
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
acquire = capture.split("bool acquire(", 1)[1].split("lease::~lease", 1)[0]
ready = acquire.split("if (context.ready_dispatch)", 1)[1].split("frame.acquired_ms", 1)[0]
wake = capture.split("HANDLE dispatch_event()", 1)[1].split("bool acquire(", 1)[0]
gate = gpu.split("bool stream_present_gate(", 1)[1].split("const double d_lock", 1)[0]
callback = gpu.split("void stream_on_finish_effects(", 1)[1].split("static void stream_produce_locked(", 1)[0]
checks = {
    "original startup disabled; successful arm enables worker dispatch including FG":
        "bool ready_dispatch = false" in capture
        and callback.index("mgpu::aligned::set_ready_dispatch(false)") < callback.index("mgpu::aligned::acquire(aligned_frame)")
        and callback.index("s.armed = true") < callback.index("set_ready_dispatch(aligned_mode)"),
    "normalization remains scheduled before color readiness check":
        acquire.index("context.depth_converter.submit") < acquire.index("if (context.ready_dispatch)"),
    "readiness checks only color, never full guide or neural completion":
        "frame.state.copied[0]" in ready and "GetCompletedValue()" in ready
        and all(token not in ready for token in ("copied[1]", "copied[2]", "depth_converter.complete", "nfence")),
    "no CPU blocking or extra GPU queue wait in acquisition":
        all(token not in acquire for token in ("WaitFor", "->Wait(", "Sleep(")),
    "removed device is not a ready source":
        ready.index("finished == UINT64_MAX") < ready.index("finished < ticket.value")
        and "context.failed = true" in ready,
    "wake event is process-lifetime, auto-reset, with one registration per ticket":
        "if (context.source_event == nullptr)" in ready
        and "CreateEventW(nullptr, FALSE, FALSE, nullptr)" in ready
        and "context.wake_fence.Get() != fence || context.wake_value != ticket.value" in ready,
    "wake failure falls back to existing bounded polling, not unsafe acquisition":
        "context.wake_failed = true" in ready and "return false" in ready
        and "context.wake_failed" in wake,
    "stale or disabled wake cannot grant a frame":
        all(token in wake for token in ("!enabled()", "context.failed", "!context.ready_dispatch",
            "frame.state.serial != context.wake_serial", "!frame.state.recorded()",
            "ticket.value != context.wake_value", "context.wake_fence.Get()")),
    "no lease before source completion; oldest-frame order retained":
        acquire.index("finished < ticket.value") < acquire.index("frame.state.leased = true")
        and "newer_frame(context.frames[selected].state.key.frame, frame.state.key.frame)" in acquire,
    "source event wakes alongside neural event without replacing neural fence":
        "const HANDLE events[]{ev, source_ev}" in gate
        and "WaitForMultipleObjects(2, events, FALSE, timeout_ms)" in gate
        and "f = s.nf;" in gate and "want = s.pipeline.current().fence" in gate,
    "output still takes priority; event wait occurs after stream lock released":
        gate.index("s.pipeline.display_seal() : s.consumed) != s.presented") < gate.index("mgpu::aligned::dispatch_event()")
        < gate.index("EVERYTHING BELOW RUNS WITH s.cs RELEASED") < gate.index("WaitForMultipleObjects"),
    "full ring prevents source wake; pending FG presentation takes precedence":
        "s.produced < s.consumed + stream_state::RING" in gate
        and gate.index("else if (s.fg_pair.pending())") < gate.index("mgpu::aligned::dispatch_event()"),
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
