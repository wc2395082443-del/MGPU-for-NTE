from pathlib import Path

root=Path(__file__).resolve().parents[1]
trace=(root/"src/game_work_trace.cpp").read_text()
aligned=(root/"src/aligned_input.cpp").read_text()
dll=(root/"src/dllmain.cpp").read_text()
native_reset=trace[trace.index("HRESULT STDMETHODCALLTYPE native_reset"):trace.index("HRESULT STDMETHODCALLTYPE native_close")]
checks={
    "no GPU or CPU waits in tracer": all(token not in trace for token in ("WaitForSingleObject", "SetEventOnCompletion", "->Wait(", "Sleep(")),
    "game device identity marker": "GetPrivateData(trace_device_guid" in trace,
    "bounded jobs": "constexpr unsigned max_jobs = 8192" in trace,
    "bounded markers": "constexpr unsigned max_marks = 64" in trace and "constexpr unsigned query_stride = native_work::timestamp_limit" in trace,
    "bounded sampling duration": "elapsed >= 2*timing::cpu_frequency()" in trace and "elapsed < timing::cpu_frequency()*11/4" in trace,
    "mapped readback behind completion fence": "entry.status.retire(completed[entry.queue] >= entry.fence_value)" in trace,
    "cached calibration": "timeline.cache.sample" in trace,
    "no query reuse": "const auto slot = context.cursor++" in trace and "context.cursor = 0" not in trace,
    "reset observer does not inject GPU commands": "EndQuery" not in trace[trace.index("void reset("):trace.index("void marker(")],
    "close before native close": "close_command_list>(on_work_close)" in dll,
    "duplicate submission invalidates": "command_replayed_queries_invalid" in trace and "duplicate ||" in trace,
    "overflow explicitly logged": "overflow_jobs=" in trace and "overflow=%u" in trace,
    "unknown frame retained": "frame_known=%u" in trace,
    "compact data preserves target dimensions": '"%s%u:%u:%u:%llx"' in trace,
    "bounded log drain": "drained < 8" in trace and "kind=segment" not in trace,
    "wrapper native command identity": "GetPrivateData(trace_command_guid" in trace and "SetPrivateData(trace_command_guid" in trace,
    "metadata only starts prefix timestamp": "if (metadata && entry.marks == 0)" in trace and "entry.current_pipeline = pipeline;\n        return;" in trace,
    "SR scope excludes interior markers": "if (!ending) return;" in trace,
    "encoding failure is explicit": "++context.encoding_errors" in trace,
    "actual execute bracketed": aligned.index("game_work::before_submit(queue, count, lists)") < aligned.index("hook.original(queue, count, lists)") < aligned.index("game_work::after_submit(queue, count, lists)"),
    "all registered work callbacks continue original": "return true;" not in dll[dll.index("static ID3D12GraphicsCommandList *work_command"):dll.index("BOOL APIENTRY DllMain")],
    "raytracing callback registered": "dispatch_rays>(on_work_rays)" in dll,
    "mesh callback registered": "dispatch_mesh>(on_work_mesh)" in dll,
    "acceleration build callback registered": "build_acceleration_structure>(on_work_acceleration_build)" in dll,
    "bundle callback registered": "execute_secondary_command_list>(on_work_bundle)" in dll,
    "feature registry bounded": "context.features.size() < 128" in trace,
    "unknown features not guessed": "unsigned ngx_feature = UINT_MAX, ngx_known = 0" in trace,
    "feature identity retained": "ngx_handle=%llu ngx_feature=%u ngx_known=%u" in trace,
    "native timestamp only after successful Reset": native_reset.index("const auto result=original(command,allocator,pipeline)") < native_reset.index("if (SUCCEEDED(result))") < native_reset.index("append(context,*entry") < native_reset.index("entry->native_start=true"),
    "native Close brackets actual end": trace.index("close(command);",trace.index("HRESULT STDMETHODCALLTYPE native_close")) < trace.index("const auto result=original(command);",trace.index("HRESULT STDMETHODCALLTYPE native_close")),
    "early callback close deferred": "if (native_depth==0 && native_observed(command)) return;" in trace,
    "atomic original method publication": "std::atomic<reset_fn>" in trace and "std::atomic<close_fn>" in trace,
    "partial native boundaries explicit": "native_start=%u native_end=%u" in trace,
    "Reset and submit origins separate": "reset_origin=%u submit_origin=%u" in trace,
    "oversized records fragmented not truncated": "kind=job_part" in trace and "entry.output.drain(8-drained" in trace,
    "native Reset clears identities off-window": "outer && (active() || tracked.load" in trace and "active() ? record(context,command) : nullptr" in trace,
    "paired end allowed without new job off-window": "const bool finishing=" in trace and "entry->scope==phase::ngx_begin" in trace and "entry->scope==phase::sl_sr_begin" in trace,
    "replay includes exact identity": "kind=replay session=%llu job=%llu command=%p" in trace,
    "drain waits for semantic scopes": "context.jobs[index]->observed_scope" in trace and "scope_pending=%u" in trace,
}
for name,passed in checks.items(): print(("PASS" if passed else "FAIL")+": "+name)
if not all(checks.values()): raise SystemExit(1)
