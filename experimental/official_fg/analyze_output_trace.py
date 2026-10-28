"""Analyze bounded M352 CPU trace. No stage is a GPU execution measurement."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics

STAGES = ["acquired", "markers_ready", "slot_ready", "osd_begin", "osd_end",
          "recorded", "submitted", "tagged", "present_returned", "state_read", "finished"]
NAMES = ["markers_and_config", "slot_retirement_wait", "import_record",
         "osd_record", "close_record", "queue_submit", "constants_tags_markers",
         "official_proxy_present", "end_marker_and_sdk_state", "retire_and_counters"]

REFLEX_INTERVALS = {
    "native_sleep_ms": ("sleep_native_begin_ns", "sleep_native_end_ns"),
    "helper_sleep_ms": ("sleep_dispatch_begin_ns", "sleep_dispatch_end_ns"),
    "sleep_relay_queue_ms": ("sleep_queued_ns", "sleep_dispatch_begin_ns"),
    "sleep_ack_to_simulation_ms": ("sleep_dispatch_end_ns", "sim_start_native_begin_ns"),
    "sim_start_delivery_ms": ("sim_start_native_begin_ns", "sim_start_dispatch_begin_ns"),
    "sim_end_delivery_ms": ("sim_end_native_begin_ns", "sim_end_dispatch_begin_ns"),
    "source_simulation_ms": ("sim_start_native_begin_ns", "sim_end_native_begin_ns"),
    "replayed_simulation_ms": ("sim_start_dispatch_begin_ns", "sim_end_dispatch_begin_ns"),
    "game_render_submit_ms": ("source_render_start_native_begin_ns", "source_render_end_native_begin_ns"),
    "helper_render_submit_ms": ("helper_render_begin_ns", "helper_render_end_ns"),
    "game_render_end_to_helper_submit_ms": ("source_render_end_native_begin_ns", "helper_render_begin_ns"),
}


def reflex_intervals(row):
    values = {}
    for name, (begin_name, end_name) in REFLEX_INTERVALS.items():
        begin, end = row.get(begin_name, 0), row.get(end_name, 0)
        if not begin or not end:
            continue
        if end < begin:
            raise ValueError("invalid source Reflex timestamp order")
        values[name] = (end - begin) / 1e6
    return values


def summarize(values):
    if not values:
        return {"count": 0}
    ordered = sorted(values)
    return {"count": len(values), "mean": statistics.mean(values),
            "median": statistics.median(values),
            "p95": ordered[max(0, math.ceil(len(values) * .95) - 1)], "max": ordered[-1]}


def select_window(rows, begin_qpc, end_qpc, frequency):
    if begin_qpc >= end_qpc or frequency <= 0:
        raise ValueError("invalid capture QPC bounds")
    selected = []
    for row in rows:
        if int(row["qpc_frequency"]) != frequency:
            raise ValueError("capture and helper QPC frequency differ")
        if int(row["acquired"]) >= begin_qpc and int(row["finished"]) <= end_qpc:
            selected.append(row)
    return selected


def analyze(rows):
    result = []
    previous = None
    rejected = 0
    for raw in rows:
        try:
            row = {key: int(value) for key, value in raw.items()}
            frequency = row["qpc_frequency"]
            stamps = [row[name] for name in STAGES]
            if frequency <= 0 or not stamps[0] or any(end < begin for begin, end in zip(stamps, stamps[1:])):
                raise ValueError("invalid QPC sequence")
            if not row["visible"]:
                previous = None
                continue
            scale = 1000 / frequency
            frame = {"sequence": row["sequence"], "source": row["source"],
                     "requested": row["requested"], "backlog": row["backlog"],
                     "acquired_qpc_seconds": stamps[0] / frequency,
                     "source_age_at_acquire_ms": stamps[0] * scale - row["capture_ns"] / 1e6,
                     "host_frame_ms": (stamps[-1] - stamps[0]) * scale,
                     "input_poll_wait_ms": row["input_poll_wait_ticks"] * scale}
            frame.update({name + "_ms": (end - begin) * scale
                          for name, begin, end in zip(NAMES, stamps, stamps[1:])})
            frame.update(reflex_intervals(row))
            if previous and row["sequence"] == previous["sequence"] + 1 and frequency == previous["qpc_frequency"]:
                frame["acquisition_interval_ms"] = (row["acquired"] - previous["acquired"]) * scale
                frame["host_gap_ms"] = (row["acquired"] - previous["finished"]) * scale
                frame["nonpoll_gap_ms"] = frame["host_gap_ms"] - frame["input_poll_wait_ms"]
                if frame["nonpoll_gap_ms"] < -0.001:
                    raise ValueError("poll wait exceeds entire host gap")
            result.append(frame)
            previous = row
        except (ValueError, KeyError, TypeError):
            rejected += 1
            previous = None
    keys = [name + "_ms" for name in NAMES] + ["host_frame_ms", "input_poll_wait_ms",
            "acquisition_interval_ms", "host_gap_ms", "nonpoll_gap_ms", "source_age_at_acquire_ms"] + list(REFLEX_INTERVALS)
    return {"frames": len(result), "rejected": rejected,
            "summary": {key: summarize([frame[key] for frame in result if key in frame]) for key in keys},
            "longest_acquisition_intervals": sorted(
                [frame for frame in result if "acquisition_interval_ms" in frame],
                key=lambda frame: frame["acquisition_interval_ms"], reverse=True)[:12],
            "longest_proxy_calls": sorted(result, key=lambda frame: frame["official_proxy_present_ms"], reverse=True)[:12],
            "limits": ["CPU durations only; queue submission is not GPU copy duration.",
                       "Host gap includes source polling, window/metrics work and logging; not pure source-render time.",
                       "Official proxy Present can contain driver and GPU waits; this trace alone cannot split them.",
                       "Source age is not input-to-photon latency; hardware-paced display changes require a verified-capable collector.",
                       "Reflex intervals are CPU/API timing, not GPU execution; missing timestamps are not zero-duration evidence.",
                       "Helper Sleep includes token/API overhead; ack-to-simulation includes IPC wakeup and game work."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--window", type=Path, help="samples.json from the simultaneous external capture")
    arguments = parser.parse_args()
    with arguments.trace.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    bounds = None
    if arguments.window:
        samples = json.loads(arguments.window.read_text(encoding="utf-8-sig"))
        receipt = json.loads((arguments.window.parent / "receipt.json").read_text(encoding="utf-8-sig"))
        bounds = [int(samples[0]["Qpc"]), int(samples[-1]["Qpc"])]
        rows = select_window(rows, *bounds, int(receipt["QpcFrequency"]))
    report = analyze(rows)
    report["window_qpc_bounds"] = bounds
    report["overlapping_complete_rows"] = len(rows) if bounds else None
    print(json.dumps(report, indent=2))
