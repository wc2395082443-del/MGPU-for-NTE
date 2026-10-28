"""Static guards complement CPU/WARP tests; none establishes visual quality."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
snapshots = (root / "src/neural_snapshots.hpp").read_text(encoding="utf-8")
capture = (root / "src/aligned_input.cpp").read_text(encoding="utf-8")
poll = gpu.split("void stream_poll()", 1)[1]
retire = poll.split("while (s.pipeline.pending())", 1)[1].split("if (s.fg_requested)", 1)[0]
waiting = retire.split("if (status == mgpu::neural_pipeline::completion::waiting)", 1)[1].split(
    "if (status != mgpu::neural_pipeline::completion::ready)", 1
)[0]
source = gpu.split("ID3D12Resource *stream_present_source(UINT &w", 2)[2].split("bool stream_nr_live()", 1)[0]
checks = {
    "prefetch only waits for already submitted payloads and reserves their identity":
        "next_seal > s.produced" in gpu and "s.pipeline.reserve(input_slot, next_seal)" in gpu,
    "prefetch executes before the CPU arrival check":
        poll.index("stream_prefetch_aligned_locked(s);")
        < poll.index("const unsigned long long completed ="),
    "mode changes wait for prefetched copies before reusing serial inputs":
        poll.index("for (const auto pending_copy : s.cqa_value)")
        < poll.index("s.async_neural = false;"),
    "pending completion is checked before new command-list recording":
        poll.index("s.pipeline.check(completed_fence)") < poll.index("s.na->Reset()"),
    "history and display advance only after successful completion":
        retire.index("status != mgpu::neural_pipeline::completion::ready")
        < retire.index("s.aligned_history_frame = pending.engine_frame")
        < retire.index("s.pipeline.publish(completed_fence)"),
    "a pending async job does not stop later submissions":
        waiting.rstrip().endswith("break;\n        }") and "return;" in waiting,
    "async submission continues without the CPU GPU-wait path":
        poll.index("if (SUCCEEDED(h) && submit_async)")
        < poll.index("const HRESULT event_hr = s.nf->SetEventOnCompletion"),
    "manual neural pause does not submit an empty neural job":
        "const bool submit_async = s.async_neural && run_nr;" in poll,
    "in-flight ownership exists before execute or signal can fail":
        poll.index("if (!s.pipeline.begin(next)) h = E_FAIL;")
        < poll.index("s.nq->ExecuteCommandLists(1, ls)")
        < poll.index("if (SUCCEEDED(h) && submit_async)\n            {\n                // Keep the producer cursor moving"),
    "input copy queue wait failures prevent neural submission":
        "h = s.nq->Wait(s.cqf, s.cqa_value[ii]);" in poll,
    "display reads independent paired snapshots, not in-flight NR output":
        "s.snapshots.input(stamp.snapshot) : s.snapshots.output(stamp.snapshot)" in source
        and "s.snapshots.input(s.pipeline.display_slot())" in source,
    "presentation signals are recorded before Present":
        gpu.index("stream_snapshot_presented(stamp.snapshot, fence, fence_value)")
        < gpu.index("hr = sc->Present(pi_interval, 0)"),
    "snapshot overwrite is guarded by GPU-side display retirement":
        "s.snapshots.wait_for_read(s.nq, pipeline_slot)" in poll
        and "queue->Wait(entry.read_fence.Get(), entry.read_value)" in snapshots,
    "three neural jobs can be in flight without reusing a snapshot slot":
        "static constexpr unsigned SLOTS = 3" in (root / "src/neural_pipeline.hpp").read_text(encoding="utf-8")
        and "snapshots=%u ordered=1 inflight=%u" in gpu
        and "PIPELINE_SLOTS" in gpu,
    "gate waits on the pending neural fence rather than repeatedly waking on arrival":
        "f = s.nf;\n                want = s.pipeline.current().fence;" in gpu,
    "oldest recorded frame is selected before testing submission tickets or constants":
        capture.index("selected = index;") < capture.index("if (!frame.state.schedulable() || !constants.valid"),
    "bounded same-frame difference readback follows snapshot creation":
        "if (f % 120ull == 0ull)" in gpu
        and gpu.index("s.snapshots.record(s.nl") < gpu.index("job_readback->difference.record(s.nl"),
    "teardown retains resources with unretired neural work":
        "retaining resources while neural submission is unretired" in gpu
        and "if (!s.snapshots.reads_complete())" in gpu
        and "s.pipeline.all_complete(completed)" in gpu,
    "copy input, direct allocator, output and FP16 descriptor use one owned slot":
        "ii = submit_async ? pipeline_slot" in poll
        and "s.async_na[pipeline_slot]" in poll
        and "s.tex_out_ring[pipeline_slot]" in poll
        and "mgpu::nr16::encode(s.nl, ii, src" in poll
        and "s.pipeline.owns_input(ii)" in gpu,
    "parallel copies are never speculatively written into unreserved inputs":
        "want = submit_async ? f :" in poll
        and "const bool pf_on = !submit_async" in poll
        and "const unsigned copy_index = submit_async ? pipeline_slot" in poll,
    "copy completion has a monotonic submission counter separate from frame identity":
        "copy_fence = ++s.cqf_value" in gpu and "s.cq_frame[ii] = f;" in gpu
        and "use_cq = (s.cq_frame[ii] == f);" in poll,
    "timestamps, evaluation roles, liveness and difference belong to the completing slot":
        "s.pipeline_readbacks[s.pipeline.current().snapshot]" in gpu
        and "s.pipeline_readbacks[pending.snapshot].difference.collect" in retire
        and all(f"job_readback->{name}" in poll for name in ["timestamps.Get()", "evaluations.Get()", "sample.Get()", "evaluation_roles.begin()"]),
    "legacy diagnostic readbacks wait for their writer without disabling probes":
        "if (probe_done < s.probe_read_fence) break;" in poll
        and "if (SUCCEEDED(h) && probe_recorded) s.probe_read_fence = s.nf_value;" in poll,
    "mode switches drain queued inference and presentation before aliasing serial buffers":
        poll.index("if (s.pipeline.pending() || s.pipeline.ready() || s.fg_pair.pending() || !s.snapshots.reads_complete()) return;")
        < poll.index("s.async_neural = false;") < poll.index("s.na = s.primary_na;"),
    "ring skipping and serial bulk work cannot retire pending input":
        "if (!s.async_neural && !s.pipeline.pending() && s.rw_on" in poll
        and "while (!s.pipeline.pending() && s.consumed < completed" in poll
        and "if (!submit_async && s.pipeline.pending()) break;" in poll,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
