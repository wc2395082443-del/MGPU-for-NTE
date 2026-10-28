"""Decode archived scheduler states without treating packet latency as GPU work."""

import argparse
from collections import Counter, defaultdict
import csv
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET


NAMESPACE = {"e": "http://schemas.microsoft.com/win/2004/08/events/event"}
STATE_NAMES = {0: "idle", 1: "running", 2: "ready", 3: "ready_standby"}
FENCE_PACKET_KINDS = {244: "fence_wait_pending", 245: "fence_signal_pending"}
FENCE_COMPLETION_TYPES = {244: "4", 245: "5"}


def integer(value):
    return int(value, 16 if value.lower().startswith("0x") else 10)


def union(intervals):
    merged = []
    for start, end in sorted(intervals):
        if end < start:
            raise ValueError("Negative interval")
        if end == start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(end, merged[-1][1])
        else:
            merged.append([start, end])
    return merged


def clipped(intervals, start, end):
    return union([(max(first, start), min(last, end)) for first, last in intervals
                  if first < end and last > start])


def milliseconds(intervals):
    return sum(end - start for start, end in union(intervals))


def without(intervals, start, end):
    gaps = []
    position = start
    for first, last in clipped(intervals, start, end):
        if first > position:
            gaps.append([position, first])
        position = last
    if position < end:
        gaps.append([position, end])
    return gaps


def read_events(raw_path, xml_path):
    with raw_path.open(newline="", encoding="utf-8-sig") as stream:
        raw = {int(row["index"]): row for row in csv.DictReader(stream)}
    events = []
    for _, node in ET.iterparse(xml_path, events=("end",)):
        if node.tag != "{http://schemas.microsoft.com/win/2004/08/events/event}Event":
            continue
        system = node.find("e:System", NAMESPACE)
        record = int(system.find("e:EventRecordID", NAMESPACE).text)
        event_id = int(system.find("e:EventID", NAMESPACE).text)
        provider = system.find("e:Provider", NAMESPACE).get("Name")
        execution = system.find("e:Execution", NAMESPACE)
        version = int(system.find("e:Version", NAMESPACE).text)
        opcode = int(system.find("e:Opcode", NAMESPACE).text)
        row = raw.get(record + 2)
        expected_provider = "d3d12" if provider == "Microsoft-Windows-Direct3D12" else "dxg"
        if row is None or (row["provider"], int(row["event_id"]), int(row["version"]),
                           int(row["opcode"]), int(row["pid"]), int(row["tid"])) != (
                expected_provider, event_id, version, opcode,
                int(execution.get("ProcessID")), int(execution.get("ThreadID"))):
            raise ValueError(f"Raw/XML identity mismatch at record {record}")
        fields = defaultdict(list)
        for field in node.findall("e:EventData/e:Data", NAMESPACE):
            fields[field.get("Name")].append(field.text or "")
        events.append({"record": record, "qpc_ms": int(row["raw_timestamp"]) / 10000,
                       "provider": row["provider"], "id": event_id, "version": version,
                       "pid": int(row["pid"]), "tid": int(row["tid"]),
                       "fields": dict(fields)})
        node.clear()
    return events


def decode_log(fields):
    payload = bytes.fromhex(fields["SchedulingLogData"][0])
    if len(payload) != int(fields["SchedulingLogSize"][0]) or len(payload) % 32:
        raise ValueError("Scheduling log size mismatch")
    cpu_first = int(fields["CpuTimestamp0"][0])
    cpu_last = int(fields["CpuTimestamp1"][0])
    gpu_first = int(fields["GpuTimestamp0"][0])
    gpu_last = int(fields["GpuTimestamp1"][0])
    if cpu_last <= cpu_first or gpu_last <= gpu_first:
        raise ValueError("Invalid GPU/QPC calibration")
    ratio = Decimal(cpu_last - cpu_first) / Decimal(gpu_last - gpu_first)
    rows = []
    for timestamp, operation, context, state in struct.iter_unpack("<QQQQ", payload):
        if operation != 0 or state not in STATE_NAMES:
            raise ValueError("Unsupported scheduling operation or state")
        qpc_ms = float((Decimal(cpu_first) + Decimal(timestamp - gpu_first) * ratio) / 10000)
        rows.append({"qpc_ms": qpc_ms, "gpu_timestamp": timestamp,
                     "context": hex(context), "state": state})
    return rows


def audit_log_clock(fields, rows, event_ms):
    cpu_first = int(fields["CpuTimestamp0"][0])
    cpu_last = int(fields["CpuTimestamp1"][0])
    maximum = max(row["qpc_ms"] for row in rows)
    future = maximum-event_ms
    return dict(adapter=fields["hAdapter"][0], event_ms=event_ms,
                calibration_age_ms=event_ms-cpu_first/10000,
                calibration_cpu_interval_us=(cpu_last-cpu_first)/10,
                maximum_decoded_ms=maximum, maximum_future_ms=future,
                coarse_causality_pass=future<=1,
                precision_validated=False)


def state_intervals(entries):
    previous = {}
    intervals = []
    for entry in entries:
        context = entry["context"]
        prior = previous.get(context)
        if prior is not None:
            if entry["qpc_ms"] < prior["qpc_ms"]:
                raise ValueError("Nonmonotonic context state timestamps")
            intervals.append({"context": context, "state": prior["state"],
                              "start_ms": prior["qpc_ms"], "end_ms": entry["qpc_ms"]})
        previous[context] = entry
    return intervals


def frame_records(log_path, session):
    records = defaultdict(dict)
    pattern = re.compile(r"\[M336\] kind=(\w+) (.*)")
    with log_path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = pattern.search(line)
            if not match:
                continue
            kind, body = match.groups()
            fields = dict(re.findall(r"(\w+)=([^\s]+)", body))
            if fields.get("session") != str(session) or "seal" not in fields:
                continue
            if kind in {"producer", "consumer", "handoff"}:
                records[int(fields["seal"])][kind] = fields
    return records


def select_contexts(contexts, adapters, game_pid, adapter_luid):
    selected, excluded = {}, {}
    for handle, identity in contexts.items():
        owner = identity.get("pid")
        luid = adapters.get(identity.get("adapter"), {}).get("luid")
        if owner is not None and owner != game_pid:
            excluded[handle] = {**identity, "exclusion_reason": "different_process"}
        elif owner == game_pid and luid is not None and luid != adapter_luid:
            excluded[handle] = {**identity, "exclusion_reason": "different_adapter"}
        elif owner == game_pid and luid == adapter_luid:
            selected[handle] = identity
        else:
            raise ValueError("Cannot safely select a scheduler context with unknown ownership/adapter")
    if not selected:
        raise ValueError("No scheduler contexts for the requested game and adapter")
    return selected, excluded


def rotate_full_scheduler_rings(entries, capacity=8192):
    grouped = defaultdict(list)
    for row in entries:
        grouped[(row["adapter"], row["node"], row["engine"])].append(row)
    result, rotations = [], []
    for key, rows in grouped.items():
        turns = [index for index in range(1, len(rows)) if rows[index]["gpu_timestamp"] < rows[index-1]["gpu_timestamp"]]
        if not turns:
            result.extend(rows)
            continue
        if len(rows) != capacity or len(turns) != 1:
            raise ValueError("Nonmonotonic scheduler log is not a single full-ring wrap")
        if len({tuple(row["calibration_identity"]) for row in rows}) != 1:
            raise ValueError("Cannot rotate scheduler entries with different clock calibrations")
        pivot = turns[0]
        ordered = rows[pivot:] + rows[:pivot]
        if any(following["gpu_timestamp"] < current["gpu_timestamp"] or following["qpc_ms"] < current["qpc_ms"]
               for current, following in zip(ordered, ordered[1:])):
            raise ValueError("Rotated scheduler log is still nonmonotonic")
        rotations.append(dict(adapter=key[0], node=key[1], engine=key[2], entries=len(rows), pivot=pivot,
                              first_ms=ordered[0]["qpc_ms"], last_ms=ordered[-1]["qpc_ms"]))
        result.extend(ordered)
    return result, rotations


def analyze(events, records, first_seal, count, game_pid, state_window_start=None, adapter_luid=None, rotate_full_rings=False, external_clock=None):
    def first(event, name):
        return event["fields"][name][0]

    devices = {}
    contexts = {}
    aliases = {}
    hardware = {}
    queues = []
    adapters = {}
    clock_audits = []
    scheduler_entries = []
    log_coverage = []
    generation_conflicts = []
    for event in events:
        fields = event["fields"]
        if event["provider"] == "dxg":
            if event["id"] == 29:
                devices[first(event, "hDevice")] = {
                    "pid": integer(first(event, "hProcessId")),
                    "adapter": first(event, "pDxgAdapter"),
                    "kmt_handle": integer(first(event, "hThunkHandle"))}
            elif event["id"] == 32:
                contexts[first(event, "hContext")] = {
                    "device": first(event, "hDevice"), "node": integer(first(event, "NodeOrdinal"))}
            elif event["id"] == 433:
                alias = first(event, "hOsHandle")
                target = first(event, "pDxgObject")
                if alias in aliases and aliases[alias] != target:
                    generation_conflicts.append(alias)
                aliases[alias] = target
            elif event["id"] == 424:
                hardware[integer(first(event, "hHwQueue"))] = {
                    "context": first(event, "hContext"),
                    "scheduler_queue": first(event, "ParentDxgHwQueue")}
            elif event["id"] == 110 and "AdapterLuid" in fields:
                adapters[first(event, "pDxgAdapter")] = {
                    "luid": integer(first(event, "AdapterLuid")),
                    "vendor": integer(first(event, "VendorID")),
                    "device_id": integer(first(event, "DeviceID"))}
            elif event["id"] == 432:
                rows = decode_log(fields)
                clock_audits.append(audit_log_clock(fields, rows, event["qpc_ms"]))
                for row in rows:
                    row["adapter"] = first(event, "hAdapter")
                    row["node"] = integer(first(event, "NodeOrdinal"))
                    row["engine"] = integer(first(event, "EngineOrdinal"))
                    row["source_event_ms"] = event["qpc_ms"]
                    row["calibration_identity"] = [first(event, name) for name in ("CpuTimestamp0", "CpuTimestamp1", "GpuTimestamp0", "GpuTimestamp1")]
                scheduler_entries.extend(rows)
                log_coverage.append({"adapter": first(event, "hAdapter"),
                                     "node": integer(first(event, "NodeOrdinal")),
                                     "start_ms": rows[0]["qpc_ms"], "end_ms": rows[-1]["qpc_ms"]})
        elif event["id"] == 49 and event["pid"] == game_pid:
            queues.append({"device": first(event, "pID3D12Device"),
                           "queue": first(event, "pCommandQueue"),
                           "type": integer(first(event, "type")),
                           "hw_handles": [integer(value) for value in fields.get("HwQueues", [])]})
    for queue in queues:
        queue["contexts"] = sorted({hardware[handle]["context"] for handle in queue["hw_handles"] if handle in hardware})
        queue["scheduler_queues"] = sorted({hardware[handle]["scheduler_queue"] for handle in queue["hw_handles"] if handle in hardware})
        queue["adapters"] = sorted({devices.get(contexts.get(context, {}).get("device"), {}).get("adapter")
                                    for context in queue["contexts"]} - {None})
    if generation_conflicts:
        raise ValueError("Scheduler aliases reused across generations; time-scoped mapping required")
    scheduled_contexts = {}
    for handle in {row["context"] for row in scheduler_entries}:
        kernel_context = aliases.get(handle)
        context = contexts.get(kernel_context, {})
        device = devices.get(context.get("device"), {})
        owners = [queue for queue in queues if kernel_context in queue["contexts"]]
        scheduled_contexts[handle] = {
            "kernel_context": kernel_context, **context, **device,
            "queues": [{"queue": queue["queue"], "type": queue["type"]} for queue in owners]}
    bad_clocks = [row for row in clock_audits if not row["coarse_causality_pass"] and
                  (adapter_luid is None or adapters.get(row["adapter"], {}).get("luid") in (None, adapter_luid))]
    if bad_clocks and external_clock is None:
        worst = max(bad_clocks, key=lambda row: row["maximum_future_ms"])
        raise ValueError(f"Scheduler clock alignment rejected: history maps {worst['maximum_future_ms']:.3f} ms after its ETW event; calibration age {worst['calibration_age_ms']:.3f} ms")
    rotations = []
    if rotate_full_rings:
        scheduler_entries, rotations = rotate_full_scheduler_rings(scheduler_entries)
    excluded_contexts = {}
    excluded_adapter_entries = 0
    if adapter_luid is not None:
        scheduled_contexts, excluded_contexts = select_contexts(scheduled_contexts, adapters, game_pid, adapter_luid)
        excluded_adapter_entries = sum(row["context"] not in scheduled_contexts for row in scheduler_entries)
        scheduler_entries = [row for row in scheduler_entries if row["context"] in scheduled_contexts]
    if any(identity.get("pid") != game_pid for identity in scheduled_contexts.values()):
        raise ValueError("This analysis requires game-owned scheduler contexts")
    unmapped_clock_entries = 0
    projected_clock_max_future = None
    if external_clock is not None:
        projected = []
        for row in scheduler_entries:
            identity = scheduled_contexts[row["context"]]
            luid = adapters.get(identity.get("adapter"), {}).get("luid")
            point = external_clock.project(luid, row["gpu_timestamp"], row["node"])
            if point is None:
                unmapped_clock_entries += 1
                continue
            if point > row["source_event_ms"]+1:
                raise ValueError("Fresh KMT clock maps scheduler history after ETW emission")
            future = point-row["source_event_ms"]
            projected_clock_max_future = future if projected_clock_max_future is None else max(projected_clock_max_future,future)
            projected.append({**row, "qpc_ms": point})
        if not projected:
            raise ValueError("No scheduler entries inside fresh KMT clock coverage")
        scheduler_entries = projected
        projected_groups = defaultdict(list)
        for row in projected:
            projected_groups[(row["adapter"],row["node"])].append(row["qpc_ms"])
        original_log_coverage = log_coverage
        log_coverage = [dict(adapter=key[0],node=key[1],start_ms=min(values),end_ms=max(values),clock_source="external_fresh_kmt")
                        for key,values in projected_groups.items()]
    else:
        original_log_coverage = log_coverage
    excluded_scheduler_entries = []
    if state_window_start is not None:
        excluded_scheduler_entries = [row for row in scheduler_entries if row["qpc_ms"] < state_window_start]
        scheduler_entries = [row for row in scheduler_entries if row["qpc_ms"] >= state_window_start]
    intervals = state_intervals(scheduler_entries)
    spans = defaultdict(list)
    for interval in intervals:
        spans[(interval["context"], interval["state"])].append([interval["start_ms"], interval["end_ms"]])
    packet_starts = {}
    packet_latencies = []
    execute_cpu = []
    execute_starts = {}
    for event in events:
        fields = event["fields"]
        if event["provider"] == "dxg" and event["id"] in {178, 244, 245, 180}:
            key = (first(event, "hContext"), first(event, "SubmitSequence"), first(event, "pQueuePacket"))
            if event["id"] != 180:
                packet_starts[key] = event
            else:
                start = packet_starts.pop(key, None)
                if start:
                    expected_type = FENCE_COMPLETION_TYPES.get(start["id"])
                    if expected_type is not None and first(event, "PacketType") != expected_type:
                        raise ValueError("WAIT/SIGNAL event disagrees with completion packet type")
                    packet_latencies.append({"context": key[0], "submit_sequence": key[1],
                                             "start_ms": start["qpc_ms"], "end_ms": event["qpc_ms"],
                                             "kind": FENCE_PACKET_KINDS.get(start["id"], "command_packet_pending"),
                                             "fields": start["fields"]})
        if event["provider"] == "d3d12" and event["id"] in {139, 140} and event["pid"] == game_pid:
            queue = first(event, "pID3D12CommandQueue")
            key = (event["tid"], queue)
            if event["id"] == 139:
                execute_starts[key] = event
            else:
                start = execute_starts.pop(key, None)
                if start:
                    execute_cpu.append({"queue": queue, "start_ms": start["qpc_ms"],
                                        "end_ms": event["qpc_ms"], "tid": event["tid"]})
    selected = []
    for seal in range(first_seal, first_seal + count):
        row = records[seal]
        producer, consumer = row["producer"], row["consumer"]
        if producer["engine_frame"] != consumer["engine_frame"]:
            raise ValueError("Producer/consumer engine identity mismatch")
        start = min(float(producer[name]) for name in ("color_submit", "depth_submit", "motion_submit"))
        ready = float(producer["ready_ms"])
        previous = records.get(seal - 1, {}).get("producer")
        cycle_start = float(previous["ready_ms"]) if previous else None
        frame = {"seal": seal, "engine_frame": int(producer["engine_frame"]),
                 "source_submit_ms": start, "source_ready_ms": ready,
                 "producer": producer, "consumer": consumer, "windows": {},
                 "exclusive_game_render_ms": None, "exclusive_direct_ms": None,
                 "exclusive_compute_ms": None, "pure_fence_blocked_ms": None}
        for name, begin, end in (("source_submit_to_ready", start, ready),
                                 ("source_cycle", cycle_start, ready),
                                 ("outgoing_submit_to_start", float(producer["out_submit"]), float(producer["out_start"]))):
            if begin is None:
                continue
            window = {"start_ms": begin, "end_ms": end, "duration_ms": end-begin,
                      "context_states": {}, "fence_packet_pending": []}
            for context, identity in scheduled_contexts.items():
                running = clipped(spans[(context, 1)], begin, end)
                state_totals = {STATE_NAMES[state]: milliseconds(clipped(spans[(context, state)], begin, end)) for state in STATE_NAMES}
                window["context_states"][context] = {**state_totals, "running_intervals": running}
            all_running = [span for context, identity in scheduled_contexts.items()
                           if identity.get("pid") == game_pid
                           for span in clipped(spans[(context, 1)], begin, end)]
            window["game_process_running_union_ms"] = milliseconds(all_running)
            window["running_intervals"] = union(all_running)
            window["not_running_intervals"] = without(all_running, begin, end)
            window["not_running_union_ms"] = end-begin-window["game_process_running_union_ms"]
            for packet in packet_latencies:
                if packet["kind"] != "fence_wait_pending" or packet["end_ms"] <= begin or packet["start_ms"] >= end:
                    continue
                if not any(packet["context"] in queue["scheduler_queues"] and
                           any(adapter == identity.get("adapter") for adapter in queue["adapters"]
                               for identity in scheduled_contexts.values()) for queue in queues):
                    continue
                window["fence_packet_pending"].append(packet)
            window["fence_wait_pending_union_ms"] = milliseconds(clipped([(packet["start_ms"],packet["end_ms"]) for packet in window["fence_packet_pending"]],begin,end))
            frame["windows"][name] = window
        frame["cpu_execute_near_submissions"] = [call for call in execute_cpu if abs(call["end_ms"]-start) < 1 or abs(call["end_ms"]-float(producer["color_submit"])) < 1]
        if previous:
            begin, end = float(previous["out_start"]), float(previous["out_end"])
            source_cycle = frame["windows"]["source_cycle"]
            previous_copy = clipped([(begin, end)], source_cycle["start_ms"], source_cycle["end_ms"])
            run_during_copy = milliseconds([segment for first, last in previous_copy
                                            for segment in clipped(source_cycle["running_intervals"], first, last)])
            frame["previous_outgoing_copy"] = {
                "seal": seal-1, "start_ms": begin, "end_ms": end,
                "within_cycle_ms": milliseconds(previous_copy), "node0_overlap_ms": run_during_copy,
                "no_node0_execution_ms": milliseconds(previous_copy)-run_during_copy}
            frame["other_no_node0_execution_ms"] = source_cycle["not_running_union_ms"]-frame["previous_outgoing_copy"]["no_node0_execution_ms"]
        selected.append(frame)
    all_running = [(span["start_ms"], span["end_ms"]) for span in intervals if span["state"] == 1]
    overlaps = []
    for current, following in zip(selected, selected[1:]):
        start = max(float(current["consumer"]["neural_start"]), following["windows"]["source_cycle"]["start_ms"])
        end = min(float(current["consumer"]["neural_end"]), following["windows"]["source_cycle"]["end_ms"])
        overlap = milliseconds(clipped(all_running, start, end)) if end > start else 0
        overlaps.append({"neural_seal": current["seal"], "next_source_cycle": following["seal"],
                         "gpu_execution_overlap_ms": overlap})
    return {"frames": selected, "scheduler_contexts": scheduled_contexts,
            "selected_adapter_luid": adapter_luid,
            "excluded_scheduler_contexts": excluded_contexts,
            "excluded_scheduler_entry_count": excluded_adapter_entries,
            "scheduler_ring_rotations": rotations,
            "original_scheduler_clock_audits": clock_audits,
            "scheduler_clock_source": "external_fresh_kmt" if external_clock is not None else "etw_cached_pair",
            "external_clock_validation": external_clock.validation if external_clock is not None else None,
            "unmapped_clock_entries": unmapped_clock_entries,
            "projected_clock_max_future_ms": projected_clock_max_future,
            "queues": queues, "adapters": adapters, "scheduler_log_coverage": log_coverage,
            "original_scheduler_log_coverage": original_log_coverage,
            "scheduler_entry_count": len(scheduler_entries),
            "scheduler_state_counts": dict(Counter(row["state"] for row in scheduler_entries)),
            "state_intervals": intervals, "raw_xml_identity_matches": len(events),
            "scheduler_validation_start_ms": state_window_start,
            "scheduler_entries_before_validation_window": len(excluded_scheduler_entries),
            "cross_gpu_execution_overlap": overlaps,
            "limitations": [
                "Context RUNNING is measured execution, including any bridge work scheduled in that context; it is not exclusive game-shader time.",
                "Source-cycle windows are bounded by consecutive texture readiness, not independently instrumented engine render begin/end.",
                "Some DIRECT and COMPUTE queues share a scheduler context; their execution cannot be split using context states alone.",
                "Fence wait packet submission-to-completion includes earlier queued work; it is not pure GPU blocked time and may overlap RUNNING.",
                "Only node 0 has scheduling log coverage in this ETL; other engines do not have measured RUNNING states.",
                "No state is extrapolated before its first or beyond its last recorded transition."]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--xml", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--session", default="345855662499")
    parser.add_argument("--first-seal", type=int, default=44545)
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--game-pid", type=int, default=50164)
    parser.add_argument("--state-window-start", type=float)
    parser.add_argument("--adapter-luid", type=lambda value: int(value, 0))
    parser.add_argument("--rotate-full-rings", action="store_true")
    parser.add_argument("--clock-samples", type=Path)
    args = parser.parse_args()
    external_clock = None
    if args.clock_samples:
        from scheduler_clock_map import ClockMap
        external_clock = ClockMap(json.loads(args.clock_samples.read_text(encoding="utf-8-sig")))
    result = analyze(read_events(args.raw, args.xml), frame_records(args.log, args.session), args.first_seal, args.count, args.game_pid, args.state_window_start, args.adapter_luid, args.rotate_full_rings, external_clock)
    paths = [args.raw, args.xml, args.log]
    if args.clock_samples:
        paths.append(args.clock_samples)
    result["inputs"] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    for frame in result["frames"]:
        window = frame["windows"].get("source_cycle")
        if window is None:
            print(frame["seal"], "source_cycle unknown: preceding source boundary missing")
            continue
        print(frame["seal"], "cycle", round(window["duration_ms"],6), "running",round(window["game_process_running_union_ms"],6), "not_running",round(window["not_running_union_ms"],6))
        print({context:{state:round(value,6) for state,value in data.items() if state != "running_intervals"} for context,data in window["context_states"].items()})
    print("scheduler entries", result["scheduler_entry_count"], "raw/xml matched",result["raw_xml_identity_matches"])


if __name__ == "__main__":
    main()
