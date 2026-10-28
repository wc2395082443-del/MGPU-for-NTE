"""Dedicated COPY-queue integration guards, not a hardware throughput test."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
copy = (root / "src/producer_copy.hpp").read_text(encoding="utf-8")
capture = (root / "src/aligned_input.cpp").read_text(encoding="utf-8")
gpu = (root / "src/gpu1_context.cpp").read_text(encoding="utf-8")
producer = gpu.split("void stream_on_finish_effects(", 1)[1].split("void stream_mvec_copy(", 1)[0]
transfer = capture.split("HRESULT submit_transfer(", 1)[1].split("void capture_constants(", 1)[0]
aligned = producer.split("const HRESULT transfer = mgpu::aligned::submit_transfer(", 1)[1].split("\n        return;", 1)[0]
checks = {
    "dedicated COPY queue and COPY command allocators":
        "description.Type = D3D12_COMMAND_LIST_TYPE_COPY" in copy
        and "CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY" in copy
        and "CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY" in copy,
    "bounded allocator pool cannot recycle pending commands":
        "std::array<submission, 4>" in copy
        and copy.index("if (slot.value > completed)") < copy.index("slot.allocator->Reset()"),
    "all split batches precede full retirement and full arrival signals":
        "std::array<mgpu::producer_copy::range, 4> copies" in transfer
        and "copies[0] = {seal_upload, seal_source, seal_target, seal_bytes}" in transfer
        and copy.index("queue_->ExecuteCommandLists(1, color_commands)")
        < copy.index("queue_->Signal(color_arrival, arrival_value)")
        < copy.index("queue_->ExecuteCommandLists(1, depth_commands)")
        < copy.index("queue_->Signal(depth_arrival, arrival_value)")
        < copy.index("queue_->ExecuteCommandLists(1, commands)")
        < copy.index("queue_->Signal(fence_.Get()") < copy.index("queue_->Signal(arrival"),
    "split planes cannot overlap or alias their completion fences":
        "color_arrival == arrival || color_arrival == fence_.Get()" in copy
        and "depth_arrival == arrival || depth_arrival == fence_.Get()" in copy
        and "depth_arrival == color_arrival" in copy
        and "ranges[range_index].target_offset < ranges[plane_index].target_offset + ranges[plane_index].bytes" in copy,
    "color and remaining ranges retain their original byte counts and destinations":
        "slot.color_command->CopyBufferRegion(target, color.target_offset, color.source, color.source_offset, color.bytes)" in copy
        and "if (slot.split && range_index == 1) continue;" in copy
        and "if (slot.split_guides && range_index == 2) continue;" in copy
        and "slot.depth_command->CopyBufferRegion(target, depth.target_offset, depth.source, depth.source_offset, depth.bytes)" in copy
        and "slot.command->CopyBufferRegion(target, copy.target_offset, copy.source, copy.source_offset, copy.bytes)" in copy,
    "source slots retire on the private COPY completion fence":
        "context.outgoing.queue(), context.outgoing.fence()" in transfer
        and "frame.state.released = {context.outgoing_queue, retirement}" in transfer
        and transfer.index("context.outgoing.submit(") < transfer.index("frame.state.released ="),
    "same-frame constants and produced count are published only after submission":
        aligned.index("if (FAILED(transfer))") < aligned.index("s.aligned_constants[slot] = aligned_frame.constants")
        < aligned.index("mgpu::aligned::commit_transfer") < aligned.index("s.produced_a.store"),
    "aligned transfer leaves no bulk copy on the game command list":
        "gl->" not in aligned
        and producer.index("s.produced_a.store(fi, std::memory_order_release);\n        return;")
        < producer.index("gl->CopyBufferRegion(s.gxfer"),
    "failed in-flight work blocks resource teardown":
        "if (failed_) return true;" in copy
        and "if (context.outgoing.pending()) return true;" in capture
        and "if (FAILED(result)) { failed_ = true; return result; }" in copy,
    "timestamps are optional and read only after COPY completion":
        "support.CopyQueueTimestampQueriesSupported" in copy
        and "slot.value > completed || slot.measured" in copy
        and "D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP" in copy,
    "completed-job tuning values are logged without changing settings":
        "pending.settings.tone, pending.settings.structure, pending.settings.skin" in gpu,
    "guide-stage initialization failure retains the working color split":
        "if (!s.guide_copy_overlap)" in producer
        and "s.color_copy_overlap = false" not in producer.split("if (!s.guide_copy_overlap)", 1)[1].split("char depth_report", 1)[0],
    "shutdown releases speculative depth waits before COPY drain":
        gpu.index("(void)s.ndepth_ready->Signal((UINT64)s.cq_issued)")
        < gpu.index("s.cq_depth_lists[ci]->Release()"),
    "timestamp storage and decoding cover three nonoverlapping query pairs":
        "query_desc.Count = unsigned(slots_.size()) * 6" in copy
        and "const unsigned last = slot.split_guides ? 5 : slot.split ? 3 : 1" in copy
        and "cqh.Count = stream_state::PIPELINE_SLOTS * 6u" in gpu
        and "const unsigned last_mark = s.guide_copy_overlap ? 5u : s.color_copy_overlap ? 3u : 1u" in gpu,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
