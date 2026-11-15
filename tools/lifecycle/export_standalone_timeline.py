import argparse
import base64
from collections import Counter, defaultdict
import gzip
import json
from pathlib import Path
import shutil
import sqlite3
import struct

import numpy as np
from trace_io import read_header


RECORD = struct.Struct("<6I")
COARSE = {"graphics": 0, "compute": 1, "indirect": 2, "raytrace": 3, "copy": 4, "target": 5, "capture": 5}
TRACK_NAMES = {"direct": "4080S · 主 DIRECT 区间", "ngx": "NGX 调用相关区间（主队列子集）",
               "controller": "手柄 · XInput原始状态", "controller_hid": "手柄 · HID报告 / 原始轴值",
               "out_copy": "4080S · 出站 COPY", "in_copy": "5080 · 接收 COPY", "nr": "5080 · NR 合计",
               "nr1": "5080 · NR 第 1 遍（查询区间）", "nr2": "5080 · NR 第 2 遍（查询区间）",
               "output": "5080 · NR 输出快照", "mouse": "鼠标 · 观察器收到位移", "game_cpu": "游戏 · CPU 阶段标记",
               "game_camera": "源相机 · 提交常量", "helper": "helper · CPU 提交 / Present", "fg_sdk": "5080 · 官方插帧 FG（SDK 状态）",
               "display": "系统 · helper 显示事件", "game_display": "系统 · 游戏显示事件", "fine": "4080S · 原始命令细分",
               "native_other": "4080S · 其他 DIRECT / COMPUTE", "capture_copy": "4080S · 颜色 / 深度 / MV 快照",
               "nr_other": "5080 · 其他复制区间", "bridge_cpu": "桥接器 · CPU 调用", "queue_api": "队列 · 原始 API 调用",
               "gpu_clock": "GPU · 时钟查询", "source_camera": "FG 输入 · 相机常量"}


def safe_json(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")


def sort_records(path):
    values = np.fromfile(path, dtype="<u4").reshape(-1, 6)
    if len(values) and np.any(values[1:, 0] < values[:-1, 0]):
        values = values[np.argsort(values[:, 0], kind="stable")]
    values.tofile(path)
    return values


def export(directory, destination):
    analysis = directory / "analysis-v2"
    summary = json.loads((analysis / "summary.json").read_text(encoding="utf-8"))
    receipt = json.loads((directory / "receipt.json").read_text(encoding="utf-8-sig"))
    metadata = json.loads((analysis / "reference-format-metadata.json").read_text(encoding="utf-8"))
    origins = summary["begin_qpc"]
    frequency = summary["qpc_frequency"]
    duration_ticks = summary["end_qpc"] - origins
    if duration_ticks >= 2 ** 32:
        raise ValueError("Capture is too long for lossless 32-bit relative QPC storage")
    selected_path = next((path for path in (analysis / "reference-five-frames-1.json", analysis / "reference-five-frames-normal.json") if path.exists()), None)
    if selected_path:
        primary = json.loads(selected_path.read_text(encoding="utf-8"))["main_direct_queue"]
    else:
        direct_queues = Counter()
        for row in metadata["jobs"].values():
            if int(row["type"]) == 0:
                direct_queues[row["queue"]] += 1
        if not direct_queues:
            raise ValueError("No observed DIRECT queue; main queue identity unknown")
        primary = direct_queues.most_common(1)[0][0]
    queues = {row["queue"]: int(row["type"]) for row in metadata["jobs"].values()}
    destination.mkdir(parents=True, exist_ok=True)
    data_directory = destination / "event-data"
    data_directory.mkdir(exist_ok=True)
    handles = {}
    kinds = []
    kind_lookup = {}
    counts = Counter()
    jobs, job_lookup, cameras, controller_payloads, mouse_sources = [], {}, {}, [], {}
    bins = defaultdict(lambda: np.zeros((601, 3), dtype=np.float64))
    def kind_id(value):
        if value not in kind_lookup:
            kind_lookup[value] = len(kinds)
            kinds.append(value)
        return kind_lookup[value]
    def add(track, start, end, source, kind, reference=0, value=0):
        start = max(0, int(start) - origins)
        end = min(duration_ticks, int(end) - origins)
        if end < start or start > duration_ticks:
            return
        if track not in handles:
            handles[track] = (data_directory / (track + ".bin")).open("wb", buffering=1024 * 1024)
        handles[track].write(RECORD.pack(start, end, int(source) & 0xffffffff, kind_id(kind), int(reference) & 0xffffffff, int(value) & 0xffffffff))
        counts[track] += 1
        bucket = min(600, int(start / max(1, duration_ticks) * 600))
        bins[track][bucket, 0] += 1
        bins[track][bucket, 1] += end - start
        bins[track][bucket, 2] = max(bins[track][bucket, 2], end - start)
    database = sqlite3.connect((analysis / "timeline.sqlite").resolve().as_uri() + "?mode=ro", uri=True)
    scanned = 0
    try:
        for lane, start, end, source, sequence, kind, detail in database.execute("SELECT lane,start,end,source,sequence,kind,detail FROM events"):
            values = json.loads(detail)
            if lane == "4080s_gpu":
                identity = values["job"]
                if identity not in job_lookup:
                    job_lookup[identity] = len(jobs)
                    queue = values.get("queue", "unknown")
                    jobs.append([identity, queue, queues.get(queue, -1)])
                track = "direct" if values.get("queue") == primary else "native_other"
                add(track, start, end, source, kind, job_lookup[identity])
                if track == "direct" and kind in ("ngx_begin", "sl_sr_begin"):
                    add("ngx", start, end, source, kind, job_lookup[identity])
            elif lane == "4080s_operations":
                identity = values["job"]
                if identity not in job_lookup:
                    raise ValueError("Fine operation without its native job identity")
                add("fine", start, end, source, kind, job_lookup[identity], int(values.get("origin", "0")))
            elif lane == "transfer":
                add("out_copy" if kind == "out" else "capture_copy", start, end, source, kind, int(values.get("seal") or 0))
            elif lane == "5080s_nr":
                track = "in_copy" if kind == "copy" else "nr" if kind == "neural" else "output" if kind == "output" else "nr_other"
                add(track, start, end, source, kind, int(values.get("seal") or 0))
            elif lane in ("controller", "controller_hid"):
                controller_payloads.append(values)
                add(lane, start, end, source, kind, len(controller_payloads) - 1, sequence)
            elif lane == "fg_sdk":
                add(lane, start, end, source, kind, values["reported_since_previous_state"], values["status"])
            elif lane == "mouse":
                add(lane, start, end, source, kind, values["dx"], values["dy"])
                if values.get("raw_device") is not None:
                    mouse_sources[str(max(0, int(start) - origins))] = {"device": values["raw_device"], "extra_information": values.get("extra_information", 0)}
            elif lane in ("game_camera", "source_camera"):
                add(lane, start, end, source, kind, sequence)
                if lane == "game_camera":
                    cameras[str(source)] = {"forward": values["forward"], "position": values["position"]}
            elif lane == "display":
                track = lane if int(values["pid"]) == receipt["HelperPID"] else "game_display"
                add(track, start, end, source, kind, sequence)
            elif lane == "queue_api":
                add(lane, start, end, source, kind, sequence, values["thread"])
            elif lane in TRACK_NAMES:
                add(lane, start, end, source, kind, sequence, int(values.get("thread") or 0))
            else:
                raise ValueError("Unexported lane: " + lane)
            scanned += 1
            if scanned % 1000000 == 0:
                print(f"exported {scanned} events", flush=True)
    finally:
        database.close()
        for handle in handles.values():
            handle.close()
    frames = []
    for source, row in metadata["producers"].items():
        ready = round(float(row["ready_ms"]) * frequency / 1000) - origins
        frames.append([ready, int(source), int(row["seal"])])
    frames.sort()
    main_header = read_header(directory / receipt["MainTrace"])
    recorder = summary["native"].get("recorder_backend", "unknown")
    manifest = {"version": 1, "record_words": 6, "frequency": frequency, "qpc_origin": origins,
                "duration": duration_ticks / frequency, "native_until": (summary["native"]["last_gpu_job_end_qpc"] - origins) / frequency,
                "fine_truncated_jobs": summary["native"]["fine_truncated"], "raw_directory": str(directory),
                "binary_stream_seconds": (main_header["end"] - main_header["begin"]) / main_header["frequency"],
                "complete_pipeline_verified": summary.get("complete_pipeline_verified", False), "recorder_backend": recorder,
                "recording_mutex_used": recorder == "shared_recording_mutex",
                "controlled_fixture": receipt.get("ControlledFixture", False),
                "main_queue": primary, "kinds": kinds, "jobs": jobs, "cameras": cameras, "frames": frames,
                "controller_payloads": controller_payloads,
                "mouse_sources": mouse_sources,
                "phases": [{"name": row["Name"], "start": (row["Qpc"] - origins) / frequency} for row in receipt.get("Phases", [])],
                "onsets": [{"time_s": (row["mouse_qpc"] - origins) / frequency, "source": row.get("source"),
                            "observed_mouse_to_camera_ms": row.get("observed_mouse_to_camera_ms"), "causal_pair_verified": False}
                           for row in summary.get("mouse_restart_candidates", [])],
                "tracks": {}, "exported_events": sum(counts.values()),
                "scope": "All timeline.sqlite events with exact relative QPC timestamps; tooltips show selected identity fields. Full original fields remain in raw files/SQLite."}
    for track, count in counts.items():
        raw_path = data_directory / (track + ".bin")
        records = sort_records(raw_path)
        maximum_duration = int(np.max(records[:, 1] - records[:, 0])) if len(records) else 0
        compressed = raw_path.with_suffix(".bin.gz")
        with raw_path.open("rb") as source, compressed.open("wb") as target:
            with gzip.GzipFile(fileobj=target, mode="wb", compresslevel=6, mtime=0) as compressor:
                shutil.copyfileobj(source, compressor)
        del records
        manifest["tracks"][track] = {"name": TRACK_NAMES[track], "count": count, "max_duration_ticks": maximum_duration,
                                      "bins": bins[track].astype(np.int64).tolist(), "gzip_bytes": compressed.stat().st_size}
        print(f"packed {track}: {count} events, {compressed.stat().st_size} bytes", flush=True)
    (destination / "viewer-manifest.json").write_text(safe_json(manifest), encoding="utf-8")
    html_path = package_html(destination, manifest)
    audit = {"html": str(html_path), "bytes": html_path.stat().st_size, "duration_seconds": manifest["duration"],
             "sql_event_counts": summary["event_counts"], "exported_tracks": {track: info["count"] for track, info in manifest["tracks"].items()},
             "exported_records": manifest["exported_events"], "expected_records": sum(summary["event_counts"].values()),
             "all_event_counts_match": manifest["exported_events"] == sum(summary["event_counts"].values()),
             "no_new_capture": True, "standalone": True}
    if not audit["all_event_counts_match"]:
        raise ValueError("Exported event count differs from the original timeline")
    (destination / "export-audit.json").write_text(json.dumps(audit, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(safe_json(audit), flush=True)


def package_html(destination, manifest):
    data_directory = destination / "event-data"
    template = Path(__file__).with_name("standalone_timeline.html").read_text(encoding="utf-8")
    prefix, suffix = template.split("<!-- PIPELINE_PAYLOAD -->")
    html_path = destination / "pipeline-timeline.html"
    with html_path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(prefix)
        payload = gzip.compress(safe_json(manifest).encode("utf-8"), mtime=0)
        stream.write('<script type="application/octet-stream" id="pipeline-manifest">' + base64.b64encode(payload).decode("ascii") + '</script>\n')
        for track in manifest["tracks"]:
            stream.write('<script type="application/octet-stream" id="track-' + track + '">')
            with (data_directory / (track + ".bin.gz")).open("rb") as source:
                while block := source.read(3 * 1024 * 1024):
                    stream.write(base64.b64encode(block).decode("ascii"))
            stream.write('</script>\n')
        stream.write(suffix)
    shutil.copyfile(Path(__file__).with_name("standalone_timeline.js"), destination / "pipeline-viewer.js")
    with html_path.open("a", encoding="utf-8") as stream:
        stream.write('<script>\n' + Path(__file__).with_name("standalone_timeline.js").read_text(encoding="utf-8") + '\n</script>\n')
    return html_path


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--repack", action="store_true")
    arguments = parser.parse_args()
    if arguments.repack:
        manifest_path = arguments.destination / "viewer-manifest.json"
        if not manifest_path.is_file():
            html = (arguments.destination / "pipeline-timeline.html").read_text(encoding="utf-8")
            marker = '<script type="application/octet-stream" id="pipeline-manifest">'
            payload = html.split(marker, 1)[1].split('</script>', 1)[0]
            manifest_path.write_bytes(gzip.decompress(base64.b64decode(payload)))
        print(package_html(arguments.destination, json.loads(manifest_path.read_text(encoding="utf-8"))))
    else:
        export(arguments.directory, arguments.destination)
