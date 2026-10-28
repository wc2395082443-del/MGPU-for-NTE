"""M343 observational contracts, not proof of full driver coverage."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
trace = (root/"src/queue_dependency_trace.cpp").read_text(encoding="utf-8")
gpu = (root/"src/gpu1_context.cpp").read_text(encoding="utf-8")
measure = (root/"src/frame_measurement.cpp").read_text(encoding="utf-8")
work = (root/"src/game_work_trace.cpp").read_text(encoding="utf-8")
collector = (root/"tools/collect_m343_causality.ps1").read_text(encoding="utf-8")
checks = {
    "fixed 24MiB event capacity and bounded object inventory": "capacity = 262144, object_capacity = 4096" in trace,
    "no added GPU execute/signal/wait/barrier/timestamp commands": all(token not in trace for token in (
        "->ExecuteCommandLists(", "->Wait(", "->Signal(", "->ResourceBarrier(", "->EndQuery(", "->GetClockCalibration(")),
    "no added CPU GPU-wait or worker sleep": all(token not in trace for token in ("WaitForSingleObject(", "WaitForMultipleObjects(", "Sleep(")),
    "original queue and fence ABI slots preserved": all(token in trace for token in (
        "originals[10]", "originals[Wait ? 15 : 14]", "originals[8]", "originals[9]")),
    "hook originals published before pointer exchange": trace.index("target.originals[slot].store") < trace.index("InterlockedExchangePointer"),
    "all original API calls forwarded, results retained": all(token in trace for token in (
        "original(queue, count, lists)", "original(queue, fence, value)", "original(fence)", "original(fence, value, event)", "original(fence, value)")),
    "raw queue/fence private identities have lifetimes, not pointer-only joins": "GetPrivateData(identity_guid" in trace and "SetPrivateData(identity_guid" in trace,
    "shared color/depth/full frame fences explicitly aliased": all(token in gpu for token in (
        "alias(s.gfence, s.nfence)", "alias(s.gcolor_ready, s.ncolor_ready)", "alias(s.gdepth_ready, s.ndepth_ready)")),
    "native generation linked before actual submission": "queue_trace::job(queue, static_cast<ID3D12GraphicsCommandList *>(lists[index]), entry.session, entry.id)" in work,
    "recording one-shot and time bounded": "!context.attempted && session != 0" in trace and "5 * context.file_header.frequency" in trace,
    "overflow cannot overwrite older evidence": "if (index >= capacity) { ++context.overflow; return; }" in trace,
    "export waits for recorder quiescence without blocking": "context.writers.load(std::memory_order_acquire) != 0) return" in trace,
    "bounded export outside recording and game locks": "if (!context.attempted || active()" in trace and "UINT64(1024 * 1024)" in trace
        and gpu.index("mgpu::measurement::poll();", gpu.index("void stream_poll()")) < gpu.index("std::unique_lock<std::mutex> lk(s.cs)", gpu.index("void stream_poll()")),
    "partial export never advertised as complete": "CREATE_NEW" in trace and "MoveFileExW(context.partial, context.final, 0)" in trace,
    "separate trace avoids shared hot-path log floods": "mgpu::lq::" not in trace and "queue_trace_export=" in measure,
    "collector bounded and preserves unknown scheduler status": "Seconds=3" in collector and "kernel_execution_validated=$false" in collector and "all_gpu_events_visible=$false" in collector,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
