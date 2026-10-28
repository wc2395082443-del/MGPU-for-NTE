"""Summarize existing capture files; never operates on the running game."""
import argparse
import csv
import datetime as dt
import json
from pathlib import Path
import re
import statistics


def summarize_completion(entries):
    tags = {entry["tag"] for entry in entries}
    if len(tags) > 1:
        raise ValueError("Mixed pipeline log versions; select one deployment window")
    cadence = None
    if len(entries) >= 2:
        seals = [int(re.search(r"\bseal=(\d+)", entry["body"])[1]) for entry in entries]
        elapsed = (entries[-1]["time"] - entries[0]["time"]).total_seconds()
        if any(later <= earlier for earlier, later in zip(seals, seals[1:])) or any(
            later["time"] <= earlier["time"] for earlier, later in zip(entries, entries[1:])
        ):
            raise ValueError("Pipeline counter reset, duplicate, or nonmonotonic timestamp")
        cadence = {
            "tag": entries[0]["tag"], "first_seal": seals[0], "last_seal": seals[-1],
            "elapsed_s": elapsed, "seal_progress_per_s": (seals[-1] - seals[0]) / elapsed,
            "interval_ms": elapsed * 1000 / (seals[-1] - seals[0]),
            "engine_frame_progress_per_s": None, "engine_frames_per_seal": None,
        }
        engine_frames = [re.search(r"\bengine_frame=(\d+)", entry["body"]) for entry in entries]
        if all(engine_frames):
            frame_ids = [int(match[1]) for match in engine_frames]
            if any(later <= earlier for earlier, later in zip(frame_ids, frame_ids[1:])):
                raise ValueError("Engine frame counter reset or repeated frame identity")
            cadence["engine_frame_progress_per_s"] = (frame_ids[-1] - frame_ids[0]) / elapsed
            cadence["engine_frames_per_seal"] = (frame_ids[-1] - frame_ids[0]) / (seals[-1] - seals[0])
    pipeline = {}
    for field in ("neural", "output", "completion_wall_ms", "present_cpu_ms"):
        matches = [re.search(rf"\b{field}=([\d.]+)", entry["body"]) for entry in entries[1:]]
        values = [float(match[1]) for match in matches if match]
        pipeline[field] = statistics.mean(values) if values else None
    return cadence, pipeline


def summarize(directory):
    metadata = json.loads((directory / "sample-window.json").read_text(encoding="utf-8-sig"))
    anchor = dt.datetime.fromisoformat(metadata["start"])
    with (directory / "gpu-bus.csv").open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    observations = sorted({(row["timestamp"], row["game_foreground"] == "True") for row in rows})
    segments = []
    active = []
    for stamp, foreground in observations:
        timestamp = dt.datetime.fromisoformat(stamp)
        if not foreground or (active and (timestamp - active[-1]).total_seconds() > 1):
            if active:
                segments.append(active)
                active = []
        if foreground:
            active.append(timestamp)
    if active:
        segments.append(active)
    if not segments:
        raise ValueError("No foreground segment")
    segment = max(segments, key=lambda values: values[-1] - values[0])
    start, end = segment[0], segment[-1]
    if (end - start).total_seconds() < 15:
        raise ValueError("Longest uninterrupted foreground segment is under 15 seconds")
    logs = []
    for line in (directory / "after.log").read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"(\d{2}):(\d{2}):(\d{2}):(\d{3}) .*?\[MGPU\]\[(R\d+-[A-Z]+)\] (.*)", line)
        if not match:
            continue
        hour, minute, second, millis = map(int, match.groups()[:4])
        timestamp = anchor.replace(hour=hour, minute=minute, second=second, microsecond=millis * 1000)
        timestamp = min((timestamp + dt.timedelta(days=offset) for offset in (-1, 0, 1)),
                        key=lambda value: abs(value - anchor))
        logs.append({"time": timestamp, "tag": match[5], "body": match[6]})

    def in_window(entry):
        return start <= entry["time"] <= end

    metrics, previous = {}, {}
    for entry in logs:
        if entry["tag"] != "R329-TIMING":
            continue
        values = dict(re.findall(r"(\w+)=([\w.]+)", entry["body"]))
        if "stage" not in values:
            continue
        stage = values["stage"]
        prior = previous.get(stage)
        previous[stage] = entry
        if prior is None or not in_window(prior) or not in_window(entry):
            continue
        metric = metrics.setdefault(stage, {"n": 0, "invalid": 0, "sum_ms": 0, "max_ms": 0, "seals": []})
        count = int(values["n"])
        metric["n"] += count
        metric["invalid"] += int(values["invalid"])
        metric["sum_ms"] += float(values["mean_ms"]) * count
        metric["max_ms"] = max(metric["max_ms"], float(values["max_ms"]))
        metric["seals"].append(int(values["seal"]))
    for metric in metrics.values():
        metric["mean_ms"] = metric.pop("sum_ms") / metric["n"] if metric["n"] else None
    completion = [entry for entry in logs if entry["tag"] in {"R326-PIPE", "R330-PIPE"} and in_window(entry)]
    cadence, pipeline = summarize_completion(completion)
    devices = {}
    for row in rows:
        if not start <= dt.datetime.fromisoformat(row["timestamp"]) <= end:
            continue
        devices.setdefault(row["name"], []).append(row)
    utilization = {}
    for name, observations in devices.items():
        utilization[name] = {"n": len(observations)}
        for field in ("gpu_util", "power_mw", "tx_kb_s", "rx_kb_s"):
            values = [float(row[field]) for row in observations if row[field]]
            utilization[name][field + "_mean"] = statistics.mean(values) if values else None

    copy_execution = [entry for entry in logs if entry["tag"] == "R328-TRANSFER" and
                      "copy_gpu_samples=" in entry["body"] and in_window(entry)]
    copy_queue = [entry for entry in logs if entry["tag"] == "R329-COPY" and in_window(entry)]
    copies = {}
    for entries, count_key, value_key, cumulative in (
        (copy_execution, "copy_gpu_samples", "copy_gpu_ms_mean", False),
        (copy_queue, "queue_wait_n", "queue_wait_sum_ms", True),
    ):
        if len(entries) < 2:
            continue
        endpoints = [dict(re.findall(r"(\w+)=([\w.]+)", entry["body"])) for entry in (entries[0], entries[-1])]
        counts = [int(endpoint[count_key]) for endpoint in endpoints]
        values = [float(endpoint[value_key]) for endpoint in endpoints]
        sums = values if cumulative else [count * value for count, value in zip(counts, values)]
        count = counts[-1] - counts[0]
        copies[value_key] = {"n": count, "mean_ms": (sums[-1] - sums[0]) / count if count else None}
    return {
        "sample": str(directory.resolve()), "metadata": metadata,
        "foreground_start": start.isoformat(), "foreground_end": end.isoformat(),
        "foreground_s": (end - start).total_seconds(), "timings": metrics,
        "completion_cadence": cadence, "complete_pipeline_windows_mean": pipeline,
        "copy_cumulative_differences": copies, "gpu": utilization,
        "history": [entry["body"] for entry in logs if entry["tag"] == "R327-HISTORY" and in_window(entry)],
        "pipeline": [entry["body"] for entry in completion],
        "dispatch": [entry["body"] for entry in logs if entry["tag"] == "R330-DISPATCH" and in_window(entry)],
        "limitations": "No exclusive game render time, visual quality assessment or causal proof from telemetry. Seal/engine-frame progress is inferred from sparse completion logs, not measured displayed FPS. Present callback counts can include multiple swapchains or repeated frames. GPU stages can overlap; queue latency is not GPU copy duration. COPY cumulative means are rounded. Dates are matched to the recording anchor.",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    report = json.dumps(summarize(options.directory), indent=2)
    if options.output:
        with options.output.open("x", encoding="utf-8") as stream:
            stream.write(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
