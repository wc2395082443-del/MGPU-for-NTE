"""Read-only telemetry wiring, not evidence of scanout or input latency."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
addon = (root / "src/dllmain.cpp").read_text(encoding="utf-8")
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
host = (root / "experimental/official_fg/official_host.cpp").read_text(encoding="utf-8")
bridge = (root / "experimental/official_fg/official_bridge.cpp").read_text(encoding="utf-8")
channel = (root / "experimental/official_fg/official_channel.hpp").read_text(encoding="utf-8")
metrics = (root / "experimental/official_fg/official_metrics.hpp").read_text(encoding="utf-8")
window = (root / "experimental/official_fg/output_window.hpp").read_text(encoding="utf-8")
ui = addon.split("static void draw_mgpu_overlay", 1)[1]
checks = {
    "output rates precede gated panel stats and scrolling children": ui.index('5080 OFFICIAL OUTPUT') < ui.index('if (mgpu::t119::panel_stats())') < ui.index('const float avail_w'),
    "SDK output not labeled measured screen FPS": 'SDK output: %.1f frames/s' in ui and 'Screen FPS: not measured' in ui,
    "source age correctly ends at Present": 'Source age to Present:' in ui,
    "last foreground sample labeled and expired": 'Last foreground sample:' in ui and 'live_visible ? 2500 : 30000' in gpu,
    "new IPC size requires new channel version": 'channel_version = 3' in channel and 'channel.byte_size == sizeof(channel)' in host,
    "metric reads do not change user settings": 'read_metrics(channel.current_metrics' in bridge and 'ui_ini_write' not in metrics,
    "SDK rate uses observed deltas rather than configured multiplier": 'double(output.sdk_delta) / double(output.real_delta)' in metrics,
    "telemetry snapshot coherent via atomic words": 'InterlockedExchange64(&block.values[index]' in metrics and '!= before' in metrics,
    "visibility requires actual foreground game root": 'GetForegroundWindow() == GetAncestor(parent, GA_ROOT)' in host,
    "hidden windows never overwrite last visible counters": 'if (sample.visible && sample.valid) publish_metrics(channel.last_visible_metrics, sample);' in host,
    "source age collected after actual official present": 'metrics.source_age(output.present(packet, channel, next == 1, frame))' in host,
    "telemetry does not introduce sleep or queue controls": all(token not in metrics for token in ['Sleep(', 'WaitForSingleObject', 'SetMaximumFrameLatency', 'Present(']),
    "window messages live on a dedicated thread": 'thread_ = std::thread([this] { run(); });' in window and 'MsgWaitForMultipleObjects' in window,
    "UI thread never runs GPU or Streamline work": all(token not in window for token in ['Present(', 'slReflex', 'ExecuteCommandLists', 'Signal(']),
    "no forced focus/capture or synthetic mouse input": all(token not in window for token in ['SetFocus(', 'SetCapture(', 'ClipCursor(', 'SendInput(', 'AttachThreadInput(']),
    "F7 view mode reaches output visibility owner": 'g_output_view_mode.store(static_cast<unsigned>(mode));' in gpu and 'choose_visible_routes(' in gpu,
    "Home and peek have separate persistent visibility state": 'g_output_overlay_closed.store(on);' in gpu and 'g_output_peek_hidden.store(hidden);' in gpu,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
