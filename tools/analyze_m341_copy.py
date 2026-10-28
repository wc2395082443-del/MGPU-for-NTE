"""Join retired color/depth/motion COPY stages with full-frame completion records."""
import argparse
import json
import math
from pathlib import Path
import re
from statistics import mean

FIELDS = re.compile(r"(\w+)=([^\s;]+)")
PLANES = ("color", "depth", "motion")
STAMPS = tuple(plane + suffix for plane in PLANES for suffix in ("_start", "_end"))


def intersection(first_start, first_end, second_start, second_end):
    return max(0, min(first_end, second_end) - max(first_start, second_start))


def analyze(lines):
    records, neural = {}, {}
    for line in lines:
        split = "[M341-COPY] kind=" in line
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
    frames, rejected = [], []
    for key in sorted({identity[:2] for identity in records}):
        pair = [records.get(key + (kind,)) for kind in ("producer", "consumer")]
        if any(record is None for record in pair):
            rejected.append({"session": key[0], "seal": key[1], "reason": "unpaired"})
            continue
        if pair[0]["engine_frame"] != pair[1]["engine_frame"]:
            raise ValueError(f"COPY engine-frame mismatch: {key}")
        producer, consumer = ({name: float(record[name]) for name in STAMPS} for record in pair)
        if any(not math.isfinite(value) or value < 0 for stage in (producer, consumer) for value in stage.values()):
            rejected.append({"session": key[0], "seal": key[1], "reason": "missing timestamp"})
            continue
        if any(list(stage.values()) != sorted(stage.values()) for stage in (producer, consumer)):
            rejected.append({"session": key[0], "seal": key[1], "reason": "unordered timestamps"})
            continue
        margins = [consumer[plane + "_start"] - producer[plane + "_end"] for plane in PLANES]
        frame = {"session": key[0], "seal": key[1], "engine_frame": int(pair[0]["engine_frame"]),
                 "producer": producer, "consumer": consumer, "gate_margins_ms": margins,
                 "clock_or_gate_inversion": min(margins) < 0,
                 "transfer_elapsed_ms": consumer["motion_end"] - producer["color_start"],
                 "depth_receive_motion_send_overlap_ms": intersection(consumer["depth_start"], consumer["depth_end"], producer["motion_start"], producer["motion_end"]),
                 "copy_overlap_ms": sum(intersection(producer[first + "_start"], producer[first + "_end"],
                                                     consumer[second + "_start"], consumer[second + "_end"])
                                        for first in PLANES for second in PLANES)}
        for role, stage in (("producer", producer), ("consumer", consumer)):
            for plane in PLANES:
                frame[role + "_" + plane + "_ms"] = stage[plane + "_end"] - stage[plane + "_start"]
            frame[role + "_color_depth_gap_ms"] = stage["depth_start"] - stage["color_end"]
            frame[role + "_depth_motion_gap_ms"] = stage["motion_start"] - stage["depth_end"]
        frame["copy_brackets_sum_ms"] = sum(frame[role + "_" + plane + "_ms"] for role in ("producer", "consumer") for plane in PLANES)
        if key in neural:
            finished = neural[key]
            if int(finished["engine_frame"]) != frame["engine_frame"]:
                raise ValueError(f"Neural engine-frame mismatch: {key}")
            start, end = float(finished["neural_start"]), float(finished["neural_end"])
            if math.isfinite(start) and math.isfinite(end) and 0 <= start <= end:
                frame.update(neural_start=start, neural_end=end, neural_ms=end-start,
                             receive_to_neural_ms=start-consumer["motion_end"])
        frames.append(frame)
    cadence = {"outbound_color_completion_ms": [], "neural_completion_ms": []}
    for previous, current in zip(frames, frames[1:]):
        if current["session"] != previous["session"] or current["seal"] != previous["seal"] + 1:
            continue
        cadence["outbound_color_completion_ms"].append(current["producer"]["color_end"]-previous["producer"]["color_end"])
        if "neural_end" in previous and "neural_end" in current:
            cadence["neural_completion_ms"].append(current["neural_end"]-previous["neural_end"])
    metrics = sorted({name for frame in frames for name in frame if name.endswith("_ms") and name != "gate_margins_ms"})
    summary = {}
    for name in metrics + list(cadence):
        values = cadence[name] if name in cadence else [frame[name] for frame in frames if name in frame]
        if values:
            summary[name] = {"count": len(values), "mean": mean(values), "min": min(values), "max": max(values)}
    return {"paired_frames": len(frames), "rejected": rejected, "summary": summary, "frames": frames,
            "limitations": ["GPU brackets can include stalls, not exclusive engine RUNNING time.",
                "Cross-queue overlap needs valid clock calibration; inspect gate inversions.",
                "Compare full-tuple transport endpoints and unique completion cadence separately.",
                "Do not reuse M340 aggregate guide spans as active guide cost: depth/motion waits are now inside them."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    arguments = parser.parse_args()
    with arguments.log.open(encoding="utf-8-sig", errors="replace") as source:
        result = analyze(source)
    print(json.dumps(result, indent=2, allow_nan=False))
    raise SystemExit(0 if result["paired_frames"] else 1)
