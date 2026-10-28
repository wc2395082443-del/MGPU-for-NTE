from pathlib import Path
import re

root=Path(__file__).resolve().parents[1]
trace=(root/"src/game_work_trace.cpp").read_text()
observer=(root/"src/native_work_observer.cpp").read_text()
policy=(root/"src/native_work_policy.hpp").read_text()
cmake=(root/"CMakeLists.txt").read_text()
checks={
    "no waits introduced":not re.search(r"WaitFor|->Wait\(|Sleep\(",trace+observer),
    "opaque production threshold": "milliseconds<2.0" in policy and "marks!=2" in policy,
    "bounded origins and jobs":"std::array<unsigned,4>" in policy and "general_job_limit=1024" in policy and "job_limit=general_job_limit+meta_job_limit" in policy,
    "bounded operation pairs":"operation_limit=160" in policy and "timestamp_limit=384" in policy,
    "metadata-only lists cannot exhaust detail budget":"context.detail_policy.claim(meta)" in trace and "entry->fine_count==0" in trace,
    "reserved MetaCommand budget":"meta_job_limit=16" in policy and "meta_selected>=meta_job_limit" in policy,
    "operation counts before quota":trace.index("++entry->fine_seen_total")<trace.index("context.detail_policy.claim(meta)"),
    "budget skips explicit":"++entry->fine_budget_skipped" in trace,
    "empty and rejected candidates emit observations":"if (entry.fine_candidate)" in trace and "kind=seen" in trace,
    "learn only retired GPU data":"entry.collected = true;" in trace and "context.detail_policy.learn(entry.reset_origin" in trace,
    "after callback survives stop":"if (token!=UINT_MAX)" in observer and "void native_after" in trace and
        "active()" not in trace.split("void native_after",1)[1].split("void native_pipeline",1)[0],
    "incomplete pairs block final drain":"entry->observed_scope=true;" in trace and "observed_scope &&" in trace,
    "exact native PSO only":"operation.pipeline_label=label_for(context,entry->native_pipeline_state)" in trace,
    "debug names are bounded public metadata":"bytes[96]" in trace and "GetPrivateData(WKPDID_D3DDebugObjectNameW" in trace,
    "fine encoding preflight precedes any base emission":trace.index("entry.output.append(fine_line")<trace.index("entry.output.drain(8-drained"),
    "large records drain incrementally":"entry.encoding_prepared=true" in trace and "entry.emitted=entry.output.empty()" in trace,
    "explicit mixed and truncation":"entry->fine_mixed=true" in trace and "entry->fine_truncated=true" in trace,
    "extended interface checked":"static_cast<ID3D12GraphicsCommandList *>(extended.Get())==command" in observer,
    "test-only threshold not in DLL definitions":"MGPU_FINE_TEST_LEARN=1" in cmake and "MGPU_FINE_TEST_LEARN" not in cmake.split("option(MGPU_BUILD_TESTS",1)[0],
    "native APIs only forward original opaque parameters":"(command,meta,parameters,bytes),meta_execute" in observer,
    "mixed flag never deselects":"entry->fine_mixed=true;\n            native_work::select" not in trace,
    "mixed flag never suppresses native":"entry->fine_mixed || entry->scope" not in trace,
    "coarse and native query slots separate":"operation.begin=max_marks+token*2" in trace and "entry.fine_count*2,context.readback.Get()" in trace,
    "native query payload explicit":"timing=queries mixed_continues=1" in trace,
    "unclassified callback identities":"kind=callbacks" in trace and "callback_points" in trace,
}
for name,valid in checks.items(): print(f"{'PASS' if valid else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
