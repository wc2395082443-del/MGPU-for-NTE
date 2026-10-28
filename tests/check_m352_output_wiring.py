"""M352 timing retained in M354; the user-requested OSD removal is explicit."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
directory = root / "experimental/official_fg"
host = (directory / "official_host.cpp").read_text(encoding="utf-8")
trace = (directory / "output_trace.hpp").read_text(encoding="utf-8")
cmake = (directory / "CMakeLists.txt").read_text(encoding="utf-8")
present = host.split("double present(", 1)[1].split("void drain()", 1)[0]
checks = {
    "source simulation markers still required": "missing real source simulation markers" in host and "source.simulation_ready(packet.color_frame)" in host,
    "retirement precedes allocator reuse": present.index("wait(slot.retired)") < present.index("slot.allocator->Reset()"),
    "official guide lifetime wait retained": "queue()->Wait(slot.official_fence.Get(), slot.official_value)" in present,
    "NR copied unchanged to official backbuffer": present.index("CopyResource(backbuffer.Get()") < present.index("->Present(0, 0)"),
    "own OSD allocation and rendering unreachable": all(token not in host for token in ["output_osd.hpp", "make_unique<output_osd>", "osd_->"]),
    "only real guide tags remain": "runtime_.tag(packet.color_frame, tags, 2)" in host and "kBufferTypeUIAlpha" not in host and "kBufferTypeHUDLessColor" not in host,
    "production helper no longer links GDI renderer": "target_link_libraries(mgpu_official_host PRIVATE mgpu_shared_frame_transport user32)" in cmake,
    "Home telemetry preserved": "publish_metrics(channel.current_metrics, sample)" in host,
    "no M353 alternative FPS process introduced": "display_monitor" not in host and "PresentMon.exe" not in host,
    "retired OSD trace columns are adjacent empty brackets": "trace.mark(output_stage::osd_begin);\n        trace.mark(output_stage::osd_end);" in present,
    "proxy timing immediately brackets actual call": "trace.mark(output_stage::tagged);\n        const HRESULT result = runtime_.swapchain()->Present(0, 0);\n        trace.mark(output_stage::present_returned);" in present,
    "source absence wait explicitly accumulated": "input_poll_wait_ticks += wait_end.QuadPart - wait_begin.QuadPart" in host,
    "trace capped without pacing on render thread": "capacity = 4096" in trace and "Sleep(" not in trace.split("bool record(", 1)[1],
    "trace I/O only in separate writer": "worker_ = std::thread" in trace and "fprintf" not in trace.split("bool record(", 1)[1],
    "frequency queried only once per trace": trace.count("QueryPerformanceFrequency(") == 1,
    "trace waits for explicit request before consuming rows": "WaitForSingleObject(trigger_, 0)" in trace and "capturing_ = true" in trace,
    "trace remains live-readable": '_wfsopen(path.c_str(), L"wb", _SH_DENYNO)' in trace,
    "extended timing IPC requires matched main and helper": "channel_version = 3" in (directory / "official_channel.hpp").read_text(encoding="utf-8"),
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
