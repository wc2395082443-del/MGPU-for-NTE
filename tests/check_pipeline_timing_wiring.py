"""Timing is observational and never changes readiness or synchronization policy."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
capture = (root / "src/aligned_input.cpp").read_text(encoding="utf-8")
depth = (root / "src/depth_normalizer.cpp").read_text(encoding="utf-8")
timer = (root / "src/gpu_timestamp_span.hpp").read_text(encoding="utf-8")
checks = {
    "timestamp reads reject wrong serials and unfinished GPU work":
        "!gpu_completed || !recorded_ || serial != serial_" in timer,
    "instrument does not wait, signal, execute, or change resource states":
        all(operation not in timer for operation in
            ["->Wait(", "->Signal(", "ExecuteCommandLists", "ResourceBarrier", "WaitForSingleObject"]),
    "failed timestamp allocation does not fail capture":
        "void begin(" in timer and "void submitted(" in timer,
    "normalizer timestamps include transitions and dispatch":
        depth.index("job.timing.begin(") < depth.index("job.command->ResourceBarrier(2, barriers)")
        < depth.index("job.command->Dispatch(") < depth.index("job.timing.end("),
    "normalizer calibration occurs after existing execute and signal":
        depth.index("queue_->ExecuteCommandLists") < depth.index("queue_->Signal")
        < depth.index("job.timing.submitted("),
    "statistics are collected only after outgoing retirement":
        capture.index("if (frame.timing_collected || !frame.state.reclaimable(done)) return;")
        < capture.index("std::array<timing::span, 3> captures")
        and "frame.timing_collected = false;" in capture,
    "measurements do not reinterpret missing values as zero":
        "metric.count, metric.invalid" in capture,
    "timestamp support does not gate fence-based scheduling":
        "frame.state.schedulable()" in capture
        and "frame.normalization.serial != frame.state.serial" in capture,
    "capture clocks are cached per queue, not calibrated per plane":
        "submitted(queue, submitted_at, &timeline.clock)" in capture
        and "ticks - sampled_at_ >= cpu_frequency()" in timer,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
