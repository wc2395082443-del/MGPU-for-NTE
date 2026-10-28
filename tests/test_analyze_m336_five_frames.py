import importlib.util
from pathlib import Path


path = Path(__file__).with_name("analyze_m336_five_frames.py")
spec = importlib.util.spec_from_file_location("m336", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def record(kind, seal, start, end):
    base = {
        "session": "7", "kind": kind, "seal": str(seal), "engine_frame": str(100 + seal),
        "color_start": str(start), "color_end": str(start + 1),
        "normalize_start": str(start + 1), "normalize_end": str(start + 1.2),
        "out_start": str(start + 2), "out_end": str(start + 3),
        "copy_start": str(start + 3.1), "copy_end": str(start + 3.5),
        "neural_start": str(start + 3.5), "neural_end": str(end),
        "output_start": str(end), "output_end": str(end + 0.2),
    }
    if kind == "handoff":
        base["retirement"] = str(seal)
    return base


lines = ["[MGPU][M336] kind=start session=7"]
for seal in range(1, 6):
    lines.extend([
        "[MGPU][M336] " + " ".join(f"{key}={value}" for key, value in record("producer", seal, seal * 10, seal * 10 + 5).items()),
        "[MGPU][M336] " + " ".join(f"{key}={value}" for key, value in record("handoff", seal, seal * 10, seal * 10 + 5).items()),
        "[MGPU][M336] " + " ".join(f"{key}={value}" for key, value in record("consumer", seal, seal * 10 + 4, seal * 10 + 6).items()),
    ])

session, bundles = module.parse(lines)
report = module.summarize(session, bundles)
assert len(report["frames"]) == 5
assert report["observed_bridge_stage_mode"] == "no-bridge-stage-overlap-observed"
assert "unknown" in report["whole_gpu_mode"]
print("PASS: M336 parser selects five contiguous frame-matched records")
