from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
pump = gpu.split("static void stream_pump_aligned_locked", 1)[1].split("static bool stream_parallel_compatible", 1)[0]
poll = gpu.split("void stream_poll()", 1)[1]

checks = {
    "M358 early official backpressure gate is removed only from acquisition":
        "mgpu::official_bridge::backpressured()" not in pump
        and "mgpu::official_bridge::backpressured()" in poll,
    "official source acquisition restores the pre-M358 bounded ring budget":
        "const unsigned input_budget = s.fg_ready ? mgpu::secondary_fg::source_lead_limit : stream_state::RING;" in pump
        and "static const unsigned RING = 6;" in gpu
        and "source_lead_limit = 2" in (root / "experimental/secondary_fg/source_lead.hpp").read_text(encoding="utf-8"),
    "bounded source lead remains before acquisition":
        pump.index("s.produced < s.consumed + input_budget") < pump.index("mgpu::aligned::acquire(frame)"),
    "backpressure gate does not add a GPU or CPU wait":
        all(token not in pump for token in ("WaitFor", "->Wait(", "Sleep(", "SetEventOnCompletion")),
    "poll still pumps before retirement without moving the existing fence ownership":
        poll.index("stream_pump_aligned_locked(s);") < poll.index("while (s.pipeline.pending())")
        and "mgpu::official_bridge::backpressured()" not in poll.split("stream_pump_aligned_locked(s);", 1)[0],
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
