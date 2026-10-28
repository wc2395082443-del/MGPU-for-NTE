"""Submission contracts, not proof of dual-adapter execution overlap or FPS."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
capture = (root / "src/aligned_input.cpp").read_text(encoding="utf-8")
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
copy = (root / "src/producer_copy.hpp").read_text(encoding="utf-8")
depth = (root / "src/depth_normalizer.cpp").read_text(encoding="utf-8")
acquire = capture.split("bool acquire(", 1)[1].split("lease::~lease", 1)[0]
checks = {
    "base acquisition uses submission; M342 adds color-only readiness, not whole-tuple completion":
        "frame.state.schedulable()" in acquire
        and "depth_completed < depth_ticket.value" not in acquire
        and "!frame.state.ready(done)" not in acquire
        and "if (context.ready_dispatch)" in acquire
        and "const auto ticket = frame.state.copied[0]" in acquire,
    "normalization waits on this frame's captured depth before execution":
        "context.queues[depth_ticket.queue].fence, depth_ticket.value" in acquire
        and depth.index("queue_->Wait(gate, gate_value)") < depth.index("queue_->ExecuteCommandLists"),
    "outgoing color waits for color capture and guides wait for depth and motion":
        "dependencies[index] = {context.queues[ticket.queue].fence, ticket.value}" in capture
        and "dependencies[1] = {context.depth_converter.fence(), frame.normalization.value}" in capture
        and "arrival_value, retirement, dependencies, color_arrival, depth_arrival)" in capture
        and copy.index("const auto &color_gate = dependencies[0]")
        < copy.index("queue_->Wait(color_gate.fence, color_gate.value)")
        < copy.index("queue_->ExecuteCommandLists(1, color_commands)")
        < copy.index("queue_->Signal(color_arrival, arrival_value)")
        < copy.index("queue_->Wait(depth_gate.fence, depth_gate.value)")
        < copy.index("queue_->ExecuteCommandLists(1, depth_commands)")
        < copy.index("queue_->Signal(depth_arrival, arrival_value)")
        < copy.index("for (unsigned gate_index = slot.split_guides ? 2 : slot.split ? 1 : 0;")
        < copy.index("queue_->Wait(gate.fence, gate.value)")
        < copy.index("queue_->ExecuteCommandLists(1, commands)"),
    "pre-arrival neural scheduling is restricted to a working gated copy path":
        "? stream_schedule_target(s, completed)" in gpu
        and "s.copy_queue && s.cq_gated && s.cq != nullptr && s.nfence != nullptr" in gpu
        and "return (std::min)(s.produced, s.max_frames)" in gpu,
    "no inline fallback can read an unarrived frame":
        "if (f > completed && !run_nr) break;" in gpu
        and "if (f > completed && (!use_cq || !s.cq_gated || s.nfence == nullptr)) break;" in gpu,
    "NR still waits for matching input copy completion":
        "h = s.nq->Wait(s.cqf, s.cqa_value[ii]);" in gpu,
    "unsafe capture paths and obsolete transfer exports removed":
        all(token not in capture for token in
            ("source_copy_queue", "shared_plane(", "reserve_transfer(", "void copy_to(")),
    "no new CPU completion waits in capture and transfer":
        "WaitForSingleObject" not in capture and "WaitForSingleObject" not in copy,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
