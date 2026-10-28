"""Validate ETW identity/coverage and CPU off-CPU intervals; not GPU cause inference."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import xml.etree.ElementTree as ET

NAMESPACE = {"e": "http://schemas.microsoft.com/win/2004/08/events/event"}
PROVIDERS = {"802ec45a-1e99-4b83-9920-87c98277ba9d": "dxg",
             "5d8087dd-3a9b-4f56-90df-49196cdc4f11": "d3d12",
             "ca11c036-0102-4a2d-a6ad-f03cfed5d3c9": "dxgi",
             "3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c": "cpu_thread"}


def cpu_intervals(events, thread_ids):
    pending = {}
    intervals = []
    unknown_fields = 0
    for event in sorted(events, key=lambda row: row["qpc_ms"]):
        if event["provider"] != "cpu_thread":
            continue
        fields = event["fields"]
        if event["opcode"] == 36:
            if "OldThreadId" not in fields or "NewThreadId" not in fields:
                unknown_fields += 1
                continue
            old_thread, new_thread = int(fields["OldThreadId"], 0), int(fields["NewThreadId"], 0)
            if old_thread in thread_ids:
                pending[old_thread] = dict(thread=old_thread, start_ms=event["qpc_ms"], ready_ms=None,
                    old_state=fields.get("OldThreadState"), wait_reason=fields.get("OldThreadWaitReason"))
            if new_thread in pending:
                entry = pending.pop(new_thread)
                entry["end_ms"] = event["qpc_ms"]
                entry["off_cpu_ms"] = entry["end_ms"]-entry["start_ms"]
                entry["ready_to_running_ms"] = entry["end_ms"]-entry["ready_ms"] if entry["ready_ms"] is not None else None
                entry["gpu_fence_cause"] = None
                intervals.append(entry)
        elif event["opcode"] == 50:
            value = fields.get("TThreadId", fields.get("ThreadId"))
            if value is None:
                unknown_fields += 1
                continue
            thread = int(value, 0)
            if thread in pending and pending[thread]["ready_ms"] is None:
                pending[thread]["ready_ms"] = event["qpc_ms"]
    return dict(intervals=intervals, unclosed=list(pending.values()), unsupported_field_events=unknown_fields)


def audit(raw_path, xml_paths, status, trace):
    if status["status"] != 0 or status["lost"] or status.get("output_failed"):
        raise ValueError("ETW export failed/lost events")
    if status["clock_type"] != 1 or status["frequency"] != trace["header"]["frequency"]:
        raise ValueError("QPC clock identity/frequency not established")
    with raw_path.open(newline="", encoding="utf-8-sig") as stream:
        raw = {int(row["index"]): {name:value for name,value in row.items() if name not in ("index","payload_hex")}
               for row in csv.DictReader(stream)}
    cpu_events = []
    frequency = trace["header"]["frequency"]
    begin = trace["header"]["begin"]*1000/frequency
    end = trace["header"]["end"]*1000/frequency
    counts = Counter()
    inside_count = 0
    scheduling_logs = 0
    seen = set()
    for xml_path in xml_paths:
        for _, node in ET.iterparse(xml_path, events=("end",)):
            if node.tag != "{"+NAMESPACE["e"]+"}Event":
                continue
            system = node.find("e:System", NAMESPACE)
            provider_node = system.find("e:Provider", NAMESPACE)
            provider = PROVIDERS.get(provider_node.get("Guid", "").strip("{}").lower())
            if provider is None:
                raise ValueError("unknown exported provider")
            record = int(system.find("e:EventRecordID", NAMESPACE).text)
            row = raw.get(record+2)
            execution = system.find("e:Execution", NAMESPACE)
            expected = (provider, int(system.find("e:EventID", NAMESPACE).text),
                        int(system.find("e:Version", NAMESPACE).text), int(system.find("e:Opcode", NAMESPACE).text),
                        int(execution.get("ProcessID")), int(execution.get("ThreadID")))
            if row is None or (row["provider"], *(int(row[name]) for name in ("event_id", "version", "opcode", "pid", "tid"))) != expected:
                raise ValueError("raw/XML identity mismatch; record offset cannot be guessed")
            if record in seen:
                raise ValueError("duplicate XML event")
            seen.add(record)
            fields = {field.get("Name"): field.text or "" for field in node.findall("e:EventData/e:Data", NAMESPACE)}
            timestamp = int(row["raw_timestamp"])*1000/status["frequency"]
            scheduling_logs += provider == "dxg" and expected[1] == 432
            if begin <= timestamp <= end:
                inside_count += 1
                counts[f"{provider}:{expected[1]}:{expected[3]}"] += 1
                if provider == "cpu_thread":
                    cpu_events.append(dict(record=record, provider=provider, event_id=expected[1], opcode=expected[3],
                        qpc_ms=timestamp, fields=fields))
            node.clear()
    threads = {row["thread"] for row in trace["records"] if row["thread"]}
    cpu = cpu_intervals(cpu_events, threads)
    return dict(raw_xml_matched=len(seen), sampled_events=inside_count, event_counts=dict(counts),
        scheduling_log_events=scheduling_logs, cpu=cpu, kernel_running_verified=False,
        scheduler_status="No scheduling logs; cannot classify GPU ready/running" if not scheduling_logs else
            "Scheduling logs present; validate clock anchors, process/adapter/context mapping and temporal coverage before RUNNING attribution",
        limitations=["ETW provider/event presence alone is not target-queue coverage.",
            "CPU off-CPU duration is not GPU fence blocked time; queue/event association needs independent evidence.",
            "Only complete switches within the window are measured; no extrapolation.",
            "RecordId+2 mapping is checked against every raw event identity, not assumed successful."])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    status = json.loads((args.directory/"raw-status.json").read_text(encoding="utf-8-sig"))
    trace = json.loads(args.trace.read_text(encoding="utf-8-sig"))
    if not trace["valid"]:
        raise ValueError("invalid API trace")
    report = audit(args.directory/"raw.csv", [args.directory/"gpu.xml", args.directory/"system.xml"], status, trace)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("raw_xml_matched", "sampled_events", "scheduling_log_events", "kernel_running_verified", "scheduler_status")}))


if __name__ == "__main__":
    main()
