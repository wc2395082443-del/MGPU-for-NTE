"""Join retired split-COPY timestamps, not CPU submission or summed elapsed spans."""
import argparse
import json
import math
from pathlib import Path
import re
from statistics import mean


FIELDS = re.compile(r"(\w+)=([^\s;]+)")
STAMPS = ("color_start", "color_end", "guides_start", "guides_end")


def analyze(lines):
    records = {}
    neural = {}
    for line in lines:
        split = "[M340-COPY] kind=" in line
        if not split and "[M336] kind=consumer " not in line:
            continue
        fields = dict(FIELDS.findall(line))
        key = (int(fields["session"]), int(fields["seal"]))
        kind = fields["kind"]
        if split:
            if kind not in ("producer", "consumer"):
                continue
            identity = key + (kind,)
            if identity in records:
                raise ValueError(f"Duplicate split-COPY identity: {identity}")
            records[identity] = fields
        else:
            if key in neural:
                raise ValueError(f"Duplicate neural identity: {key}")
            neural[key] = fields

    frames = []
    rejected = []
    for key in sorted({identity[:2] for identity in records}):
        pair = [records.get(key + (kind,)) for kind in ("producer", "consumer")]
        if any(record is None for record in pair):
            rejected.append({"session": key[0], "seal": key[1], "reason": "unpaired"})
            continue
        producer, consumer = ({name: float(record[name]) for name in STAMPS} for record in pair)
        if any(not math.isfinite(value) or value < 0 for stage in (producer, consumer) for value in stage.values()):
            rejected.append({"session": key[0], "seal": key[1], "reason": "missing timestamp"})
            continue
        if any(list(stage.values()) != sorted(stage.values()) for stage in (producer, consumer)):
            rejected.append({"session": key[0], "seal": key[1], "reason": "unordered timestamps"})
            continue
        margins = [consumer["color_start"] - producer["color_end"],
                   consumer["guides_start"] - producer["guides_end"]]
        frame = {"session": key[0], "seal": key[1], "engine_frame": int(pair[0]["engine_frame"]),
                 "producer": producer, "consumer": consumer,
                 "gate_margins_ms": margins, "clock_or_gate_inversion": min(margins) < 0,
                 "transfer_elapsed_ms": consumer["guides_end"] - producer["color_start"],
                 "copy_overlap_ms": max(0, min(producer["guides_end"], consumer["color_end"]) -
                                           max(producer["guides_start"], consumer["color_start"]))}
        for role, stage in (("producer", producer), ("consumer", consumer)):
            frame[role + "_color_ms"] = stage["color_end"] - stage["color_start"]
            frame[role + "_guides_ms"] = stage["guides_end"] - stage["guides_start"]
            frame[role + "_between_ms"] = stage["guides_start"] - stage["color_end"]
        frame["copy_brackets_sum_ms"] = sum(frame[role + "_" + plane + "_ms"]
            for role in ("producer", "consumer") for plane in ("color", "guides"))
        if key in neural:
            finished = neural[key]
            if int(finished["engine_frame"]) != frame["engine_frame"]:
                raise ValueError(f"Engine-frame mismatch: {key}")
            start, end = float(finished["neural_start"]), float(finished["neural_end"])
            if math.isfinite(start) and math.isfinite(end) and 0 <= start <= end:
                frame["neural_start"] = start
                frame["neural_end"] = end
                frame["neural_ms"] = end - start
                frame["receive_to_neural_ms"] = start - consumer["guides_end"]
        frames.append(frame)

    cadence = {"outbound_color_completion_ms": [], "neural_completion_ms": []}
    for previous, current in zip(frames, frames[1:]):
        if current["session"] != previous["session"] or current["seal"] != previous["seal"] + 1:
            continue
        cadence["outbound_color_completion_ms"].append(current["producer"]["color_end"] - previous["producer"]["color_end"])
        if "neural_end" in previous and "neural_end" in current:
            cadence["neural_completion_ms"].append(current["neural_end"] - previous["neural_end"])

    metrics = sorted({name for frame in frames for name in frame if name.endswith("_ms") and name != "gate_margins_ms"})
    summary = {}
    for name in metrics + list(cadence):
        values = cadence[name] if name in cadence else [frame[name] for frame in frames if name in frame]
        if values:
            summary[name] = {"count": len(values), "mean": mean(values), "min": min(values), "max": max(values)}
    return {"paired_frames": len(frames), "rejected": rejected, "summary": summary, "frames": frames,
            "limitations": ["GPU timestamp brackets can include stalls; they are not exclusive engine RUNNING time.",
                            "Cross-queue overlap depends on clock calibration. Inspect inversions and original clock coverage.",
                            "Sum of four COPY brackets is not the transfer critical path; between-stage waits are separate.",
                            "Cadence uses consecutive sealed inputs only, not display FPS or raw game render time."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    arguments = parser.parse_args()
    with arguments.log.open(encoding="utf-8-sig", errors="replace") as source:
        result = analyze(source)
    print(json.dumps(result, indent=2, allow_nan=False))
    raise SystemExit(0 if result["paired_frames"] else 1)
