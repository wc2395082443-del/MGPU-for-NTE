"""Static regression guard for cross-adapter motion-vector invalidation.

Usage: python tests/check_stream_event_filter.py [src/gpu1_context.cpp]
This checks source invariants, not GPU correctness or visual quality.
"""

from pathlib import Path
import re
import sys


def check_source(source):
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    callback = source.split("void stream_on_finish_effects(", 1)[1].split(
        "void stream_mvec_copy(", 1
    )[0]
    game_guard = callback.index(
        "if (!known || ll.LowPart != want.LowPart || ll.HighPart != want.HighPart)"
    )
    game_guard_end = callback.index("mgpu::aligned::lease aligned_frame;", game_guard)
    arm_start = callback.index("if (!s.armed)", game_guard_end)
    arm_end = callback.index("s.mvec_live.store(", arm_start)
    checks = {
        "adapter validation precedes motion-vector state access": not re.search(
            r"\bs\.mvec_\w+", callback[:game_guard_end]
        ),
        "whole-ring clearing occurs only while arming": len(
            re.findall(r"s\.mvec_packets\.reset\(", callback)
        ) == 1
        and "s.mvec_packets.reset(s.produced + 1ull)" in callback[arm_start:arm_end],
        "completed frame closes the packet without clearing a next-frame write":
            "sealed_motion = s.mvec_packets.close(fi)" in callback[arm_end:]
            and "s.mvec_slot_valid[" not in callback
            and callback.count("s.mvec_packets.recorded(") == 1
            and callback.index("if (FAILED(transfer))") < callback.index("s.mvec_packets.recorded("),
        "finish-effects does not discard kept vectors": not re.search(
            r"s\.mvec_keep_f\s*=", callback
        ),
    }
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'}: {name}")
    return all(checks.values())


if __name__ == "__main__":
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        Path(__file__).resolve().parents[1] / "src" / "gpu1_context.cpp"
    )
    sys.exit(0 if check_source(path.read_text(encoding="utf-8")) else 1)
