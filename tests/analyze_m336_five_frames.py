"""Frame-match M336 records and render the first complete five-frame window."""
import argparse
import json
import math
import re
from pathlib import Path


def value(row, key):
    try:
        result = float(row[key])
        return result if math.isfinite(result) and result >= 0 else None
    except (KeyError, TypeError, ValueError):
        return None


def span(row, first, last):
    begin, end = value(row, first), value(row, last)
    return (begin, end) if begin is not None and end is not None and end >= begin else None


def overlap(left, right):
    if left is None or right is None:
        return None
    return max(0.0, min(left[1], right[1]) - max(left[0], right[0]))


def parse(lines):
    records = []
    for line in lines:
        marker = line.find("[MGPU][M336]")
        if marker < 0:
            continue
        records.append(dict(re.findall(r"([A-Za-z_]+)=([^\s;]+)", line[marker:])))
    sessions = {row.get("session") for row in records if row.get("kind") == "start"}
    if len(sessions) != 1:
        raise ValueError("log must contain exactly one M336 session")
    session = next(iter(sessions))
    selected = [row for row in records if row.get("session") == session]
    bundles = {}
    for row in selected:
        if row.get("kind") not in {"producer", "handoff", "consumer"}:
            continue
        seal = row.get("seal")
        if seal is not None:
            bundles.setdefault(seal, {})[row["kind"]] = row
    complete = []
    for seal in sorted(bundles, key=int):
        bundle = bundles[seal]
        if set(bundle) != {"producer", "handoff", "consumer"}:
            continue
        if len({bundle[k].get("engine_frame") for k in bundle}) != 1:
            continue
        complete.append(bundle)
    for start in range(len(complete) - 4):
        window = complete[start:start + 5]
        seals = [int(item["producer"]["seal"]) for item in window]
        if seals == list(range(seals[0], seals[0] + 5)):
            return session, window
    raise ValueError("fewer than five contiguous frame-matched records")


def summarize(session, bundles):
    rows = []
    for index, bundle in enumerate(bundles):
        producer, handoff, consumer = (bundle[name] for name in ("producer", "handoff", "consumer"))
        current_neural = span(consumer, "neural_start", "neural_end")
        previous_neural = None
        if index:
            previous_neural = span(bundles[index - 1]["consumer"], "neural_start", "neural_end")
        producer_out = span(producer, "out_start", "out_end")
        consumer_copy = span(consumer, "copy_start", "copy_end")
        rows.append({
            "session": int(session),
            "seal": int(producer["seal"]),
            "engine_frame": int(producer["engine_frame"]),
            "capture_ms": (value(producer, "color_end") - value(producer, "color_start")) if value(producer, "color_start") is not None and value(producer, "color_end") is not None else None,
            "normalize_ms": (value(producer, "normalize_end") - value(producer, "normalize_start")) if value(producer, "normalize_start") is not None and value(producer, "normalize_end") is not None else None,
            "outgoing_copy_ms": (producer_out[1] - producer_out[0]) if producer_out else None,
            "consumer_copy_ms": (consumer_copy[1] - consumer_copy[0]) if consumer_copy else None,
            "neural_ms": (current_neural[1] - current_neural[0]) if current_neural else None,
            "output_ms": (value(consumer, "output_end") - value(consumer, "output_start")) if value(consumer, "output_start") is not None and value(consumer, "output_end") is not None else None,
            "outgoing_with_previous_neural_ms": overlap(producer_out, previous_neural),
            "consumer_copy_with_previous_neural_ms": overlap(consumer_copy, previous_neural),
        })
    observed_overlap = any(
        row["outgoing_with_previous_neural_ms"] not in (None, 0.0) or
        row["consumer_copy_with_previous_neural_ms"] not in (None, 0.0)
        for row in rows[1:]
    )
    return {
        "session": int(session),
        "frames": rows,
        "observed_bridge_stage_mode": "pipeline-overlap-observed" if observed_overlap else "no-bridge-stage-overlap-observed",
        "whole_gpu_mode": "unknown: M336 does not identify exclusive 4080S game DIRECT/COMPUTE packets",
        "caveats": [
            "outgoing_with_previous_neural measures bridge COPY versus prior 5080 NR only",
            "capture spans include bridge barriers/copies and are not whole-game render time",
            "missing or invalid endpoints stay null and are never treated as zero",
        ],
    }


def timeline(report):
    entries = []
    for frame in report["frames"]:
        seal = frame["seal"]
        entries.extend((name, frame[name]) for name in ("capture_ms", "outgoing_copy_ms", "consumer_copy_ms", "neural_ms", "output_ms") if frame[name] is not None)
    if not entries:
        return "timeline unavailable: no valid stage durations"
    max_value = max(value for _, value in entries)
    lines = []
    for frame in report["frames"]:
        parts = []
        for name, label in (("capture_ms", "CAP"), ("outgoing_copy_ms", "OUT"), ("consumer_copy_ms", "COPY"), ("neural_ms", "NR"), ("output_ms", "OUTP")):
            duration = frame[name]
            if duration is None:
                parts.append(f"{label}:?")
            else:
                width = max(1, round(duration / max_value * 24))
                parts.append(f"{label}:{'#' * width} {duration:.3f}ms")
        lines.append(f"f{frame['seal']} " + " | ".join(parts))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    session, bundles = parse(args.log.read_text(encoding="utf-8-sig", errors="replace").splitlines())
    report = summarize(session, bundles)
    print(json.dumps(report, indent=2, allow_nan=False))
    print("\n" + timeline(report))
    if args.json:
        args.json.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
