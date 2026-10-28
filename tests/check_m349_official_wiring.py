"""Source guards only; these do not establish official interpolation or latency."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
tags = (root / "src/sl_tags.cpp").read_text(encoding="utf-8")
host = (root / "experimental/official_fg/official_host.cpp").read_text(encoding="utf-8")
bridge = (root / "experimental/official_fg/official_bridge.cpp").read_text(encoding="utf-8")
runtime = (root / "experimental/official_fg/official_runtime.cpp").read_text(encoding="utf-8")
window = (root / "experimental/official_fg/output_window.hpp").read_text(encoding="utf-8")
poll = gpu.split("void stream_poll()", 1)[1]
publish = gpu.split("bool stream_secondary_fg_publish", 1)[1].split("void stream_secondary_fg_presented", 1)[0]
create = gpu.split("bool stream_secondary_fg_create", 1)[1].split("void stream_secondary_fg_camera", 1)[0]
checks = {
    "game-side official runtime is not initialized/rebound": "slInit(" not in gpu and "slSetD3DDevice(" not in gpu,
    "legacy FG disabled on official route": create.index("official_bridge::selected()") < create.index("s.fg_stage.create"),
    "NR output exported before guide state restoration": poll.index("official_bridge::record(") < poll.index("s.snapshots.record("),
    "export follows final-pass exact-frame validation": "s.fg_camera_seal[slot] == f && depth_here && mvec_here" in poll,
    "publication is after neural submission and signal": poll.index("s.nq->ExecuteCommandLists") < poll.index("s.nq->Signal(s.nf") < poll.index("official_bridge::submitted(s.nq)"),
    "failed submission is retained or cancelled according to execution": "submission_failed(official_submitted)" in poll,
    "consumer accepts the official route without old fg_ready": "(!s.fg_ready && !mgpu::official_bridge::selected())" in poll,
    "official consumer retires instead of taking display ownership": "(s.fg_ready || mgpu::official_bridge::selected())\n            ? s.pipeline.retire" in poll,
    "fallback display changes only on presentation owner": "!t_consumer_poll && mgpu::official_bridge::selected()" in publish,
    "fallback publication requires ready GPU-retired output": "!s.fg_ready && s.async_neural && s.pipeline.ready()" in publish and "? s.pipeline.retire(completed_fence)" in poll,
    "source originals are preserved": "const auto result = original(frame);" in tags and "const auto result = original(marker, frame);" in tags,
    "game Present markers never substitute official Present": tags.count("identity.frame_known && mgpu::official_fg::source_marker_supported(marker)") == 2,
    "source callback code is pinned before pointers escape": "GET_MODULE_HANDLE_EX_FLAG_PIN" in tags,
    "missing real source simulation markers stop rather than fabricate": "missing real source simulation markers" in host and "source.simulation_ready(packet.color_frame)" in host,
    "source sleep acknowledgement bounded": "GetTickCount64() + 100" in bridge and "sleep_completed" in bridge,
    "IPC handle inheritance is explicitly restricted": "PROC_THREAD_ATTRIBUTE_HANDLE_LIST" in bridge,
    "helper tied to parent lifetime": "JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE" in bridge,
    "no new visible top-level window": "WS_CHILD | WS_DISABLED" in window and "WS_EX_NOPARENTNOTIFY" in window,
    "child remains hidden until a real frame exists": "read_atomic(channel.real_presents) > 0" in host,
    "official buffer index precedes GetBuffer": host.index("GetCurrentBackBufferIndex()") < host.index("->GetBuffer("),
    "input release precedes official evaluation only after private copy": host.index("transport_.record_read") < host.index("ExecuteCommandLists") < host.index("transport_.signal_released"),
    "private FG input reuse waits official completion": "queue()->Wait(slot.official_fence.Get(), slot.official_value)" in host,
    "official Present is bracketed with exact-frame markers": host.index("ePresentStart") < host.index("->Present(0, 0)") < host.index("ePresentEnd"),
    "unsupported FG status fails explicitly": "fg.status == sl::DLSSGStatus::eOk" in host,
    "HUD-less input is not fabricated from final color": "kBufferTypeHUDLessColor" not in host,
    "official library signature validation preserved": "verifyEmbeddedSignature" in runtime,
    "sixfold capability checked, not clamped": "refusing silent downgrade" in runtime,
    "official factory is used instead of composition passthrough": "factory_proxy_->CreateSwapChainForHwnd" in runtime and "CreateSwapChainForComposition" not in runtime,
    "no manual generated-frame pacing in helper": "presentation_pair" not in host and "generated_index" not in host,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
