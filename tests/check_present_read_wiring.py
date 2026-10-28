"""Source guards complementing the CPU-only presentation retirement tests."""

import argparse
from pathlib import Path


parser = argparse.ArgumentParser()
parser.add_argument("source", nargs="?", type=Path,
                    default=Path(__file__).resolve().parents[1] / "src/gpu1_context.cpp")
source = parser.parse_args().source.read_text(encoding="utf-8")


def between(start, end):
    if start not in source:
        return ""
    return source.split(start, 1)[1].split(end, 1)[0]


present = between("bool present_frame(", "bool has_present_chain(")
poll = between("void stream_poll()", "if (mgpu::t119::producer_active()")
release = between("void stream_release()", "// ---- R26: DRAIN")
shutdown = between("void shutdown()", "IDXGISwapChain3 *sc")
waiter = between("static bool wait_present_reads(", "// R142.")
submitted = present.find("S.shared_reads.submitted(")
present_call = present.find("hr = sc->Present(")
checks = {
    "submitted output reads are tracked before Present can fail":
        0 <= submitted < present_call,
    "read tracking does not require secondary FG":
        submitted >= 0 and "fg_requested" not in present[:present_call]
        and "g_secondary_fg_present_active" not in present[:present_call],
    "signal failure is recorded as an unfenced read":
        "S.shared_reads.submitted(fence_value, SUCCEEDED(hr))" in present,
    "consumer retirement check precedes stream lock and GPU writes":
        "wait_present_reads(4, read_failed)" in poll
        and poll.index("wait_present_reads(4, read_failed)") < poll.index("std::unique_lock<std::mutex> lk(s.cs)"),
    "runtime wait is bounded and does not hold the presentation mutex":
        "WaitForSingleObject(event, timeout_ms)" in waiter
        and "fence->AddRef()" in waiter and "fence->Release()" in waiter
        and waiter.index("fence = presentation.fence;") < waiter.index("auto status ="),
    "retirement depends on completed GPU fence rather than Present result":
        "reads.check(fence->GetCompletedValue())" in waiter
        and "sc->Present" not in waiter,
    "stream teardown retains referenced outputs when completion is unknown":
        "wait_present_reads(2000, read_failed)" in release
        and "s.present_release_blocked = true" in release,
    "parameter teardown honors the retained-stream guard":
        "if (s.fg_release_blocked || s.present_release_blocked) return;" in source,
    "presentation teardown also checks outstanding output reads":
        "wait_present_reads(2000, read_failed)" in shutdown and "return;" in shutdown,
}
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'}: {name}")
raise SystemExit(0 if all(checks.values()) else 1)
