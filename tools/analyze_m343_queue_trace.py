"""Validate observed API causality; never interpret queue Wait CPU time as GPU wait."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

HEADER = struct.Struct("<12Q")
RECORD = struct.Struct("<10QIIiI")
HEADER_FIELDS = "magic version record_bytes session frequency begin end count overflow failures metadata pending".split()
FIELDS = "sequence parent begin end object other value extra raw auxiliary operation thread result flags".split()
KINDS = {1: "object", 2: "alias", 3: "execute", 4: "command", 5: "queue_wait", 6: "queue_signal",
         7: "completed", 8: "set_event", 9: "cpu_signal", 10: "job"}
CALLS = {3, 5, 6, 7, 8, 9}


def decode(payload):
    if len(payload) < HEADER.size:
        raise ValueError("truncated header")
    header = dict(zip(HEADER_FIELDS, HEADER.unpack_from(payload)))
    if (header["magic"], header["version"], header["record_bytes"]) != (0x314543415254514d, 1, RECORD.size):
        raise ValueError("unsupported ABI")
    if not header["session"] or not header["frequency"] or not 0 < header["begin"] <= header["end"]:
        raise ValueError("invalid session/clock")
    if header["count"] > 262144 or header["metadata"] > 8192:
        raise ValueError("invalid bounded count")
    expected = HEADER.size + (header["count"] + header["metadata"]) * RECORD.size
    if len(payload) != expected:
        raise ValueError("truncated or trailing payload")
    entries = [dict(zip(FIELDS, RECORD.unpack_from(payload, offset)))
               for offset in range(HEADER.size, len(payload), RECORD.size)]
    metadata, records = entries[:header["metadata"]], entries[header["metadata"]:]
    errors = [name for name in ("overflow", "failures", "pending") if header[name]]
    objects = {}
    parents = {}
    aliases = []
    for entry in metadata:
        if entry["operation"] == 1:
            identity = entry["object"]
            if identity == 0 or identity in objects or entry["other"] not in (1, 2):
                raise ValueError("invalid object identity")
            objects[identity] = entry
            parents[identity] = identity
        elif entry["operation"] == 2:
            aliases.append(entry)
        else:
            raise ValueError("invalid metadata record")

    def canonical(identity):
        while parents[identity] != identity:
            identity = parents[identity]
        return identity

    for entry in aliases:
        first, second = entry["object"], entry["other"]
        if first not in objects or second not in objects or any(objects[value]["other"] != 2 for value in (first, second)):
            raise ValueError("alias is not a pair of known fences")
        parents[canonical(second)] = canonical(first)
    by_sequence = {}
    for index, entry in enumerate(records):
        if entry["sequence"] != index + 1 or entry["operation"] not in KINDS or entry["operation"] < 3:
            raise ValueError("invalid event sequence or kind")
        if not header["begin"] <= entry["begin"] <= entry["end"] or entry["parent"] >= entry["sequence"]:
            raise ValueError("invalid event timestamps or parent")
        if entry["object"] not in objects:
            raise ValueError("unknown event object")
        queue_kind = entry["operation"] in (3, 4, 5, 6, 10)
        if objects[entry["object"]]["other"] != (1 if queue_kind else 2):
            raise ValueError("event object category mismatch")
        if entry["operation"] in (5, 6) and (entry["other"] not in objects or objects[entry["other"]]["other"] != 2):
            raise ValueError("unknown dependency fence")
        if entry["operation"] in CALLS and not entry["begin"] <= entry["extra"] <= entry["auxiliary"] <= entry["end"]:
            raise ValueError("invalid original API bracket")
        if entry["operation"] == 10 and entry["extra"] != header["session"]:
            raise ValueError("job belongs to a different capture session")
        if entry["operation"] == 7 and entry["value"] == 2**64-1:
            errors.append("device_removed_completion")
        by_sequence[entry["sequence"]] = entry
        entry["kind"] = KINDS[entry["operation"]]
        for field in ("begin", "end"):
            entry[field+"_ms"] = entry[field] * 1000 / header["frequency"]
        if entry["operation"] in CALLS:
            entry["api_begin_ms"] = entry["extra"] * 1000 / header["frequency"]
            entry["api_end_ms"] = entry["auxiliary"] * 1000 / header["frequency"]
        if entry["parent"]:
            parent = by_sequence[entry["parent"]]
            if not parent["begin"] <= entry["begin"] <= entry["end"] <= parent["end"] or parent["thread"] != entry["thread"]:
                raise ValueError("invalid nested forwarding bracket")
    commands = defaultdict(list)
    for entry in records:
        if entry["operation"] == 4:
            parent = by_sequence.get(entry["parent"])
            if not parent or parent["operation"] != 3 or parent["object"] != entry["object"]:
                raise ValueError("command without matching Execute parent")
            commands[parent["sequence"]].append(entry)
    for entry in records:
        if entry["operation"] == 3:
            children = commands[entry["sequence"]]
            if len(children) != entry["value"] or sorted(child["value"] for child in children) != list(range(entry["value"])):
                raise ValueError("missing/duplicate submitted command-list identity")
    for entry in records:
        fence = entry["other"] if entry["operation"] in (5, 6) else entry["object"]
        if entry["operation"] in (5, 6, 7, 8, 9):
            entry["canonical_fence"] = canonical(fence)
    root_calls = []
    for entry in records:
        if entry["operation"] not in CALLS:
            continue
        parent = by_sequence.get(entry["parent"])
        forwarding = False
        while parent:
            if parent["operation"] == entry["operation"] and parent["object"] == entry["object"]:
                forwarding = True
                break
            parent = by_sequence.get(parent["parent"])
        entry["same_object_forwarding"] = forwarding
        if not forwarding:
            root_calls.append(entry)
    queue_calls = defaultdict(list)
    for entry in root_calls:
        if entry["operation"] in (3, 5, 6):
            queue_calls[entry["object"]].append(entry)
    concurrent = []
    for queue, calls in queue_calls.items():
        ordered = sorted(calls, key=lambda row: row["api_begin_ms"])
        pending = []
        for current in ordered:
            pending = [previous for previous in pending if previous["api_end_ms"] > current["api_begin_ms"]]
            for previous in pending:
                if previous["thread"] != current["thread"] and previous["api_end_ms"] > current["api_begin_ms"]:
                    concurrent.append(dict(queue=queue, first=previous["sequence"], second=current["sequence"]))
            pending.append(current)
    samples = defaultdict(list)
    signals = defaultdict(list)
    for entry in root_calls:
        if entry["operation"] == 7:
            samples[entry["canonical_fence"]].append(entry)
        elif entry["operation"] in (6, 9) and entry["result"] >= 0:
            signals[entry["canonical_fence"]].append(entry)
    dependencies = []
    for wait in root_calls:
        if wait["operation"] != 5:
            continue
        reads = samples[wait["canonical_fence"]]
        unmet = [row for row in reads if row["value"] < wait["value"] and row["api_end_ms"] >= wait["api_begin_ms"]]
        met = [row for row in reads if wait["value"] <= row["value"] < 2**64-1]
        candidates = [row for row in signals[wait["canonical_fence"]] if row["value"] >= wait["value"]]
        dependencies.append(dict(wait_sequence=wait["sequence"], queue=wait["object"], fence=wait["canonical_fence"],
            target=wait["value"], result=wait["result"], cpu_api_ms=wait["api_end_ms"]-wait["api_begin_ms"],
            sampled_unmet_sequences=[row["sequence"] for row in unmet], sampled_met_sequences=[row["sequence"] for row in met],
            possible_signal_sequences=[row["sequence"] for row in candidates], gpu_blocked_ms=None,
            reason="samples are point observations; Signal call is not GPU completion; queue predecessors and scheduler coverage required"))
    return dict(valid=not errors, errors=sorted(set(errors)), header=header, objects=list(objects.values()),
        aliases=aliases, records=records, counts=dict(Counter(row["kind"] for row in records)),
        concurrent_queue_calls=concurrent, dependencies=dependencies,
        kernel_running_verified=False, all_gpu_events_visible=False,
        limitations=["Only observed public D3D12 APIs; no private driver/kernel completeness claim.",
            "Same-object nested forwarding retained but excluded from root dependency counts.",
            "Cross-object wrapper aliases and external shared fences are unknown unless explicitly registered.",
            "CPU Wait/Signal call duration is not GPU blocked duration or completion time.",
            "Concurrent API calls do not establish a total queue order.",
            "GetCompletedValue is an existing application read, not added continuous GPU polling.",
            "Fence values can rewind; point observations do not authorize monotonic interpolation.",
            "CPU event registration does not measure OS wait duration; use matched CPU ETW coverage."])


def join_jobs(trace, compact):
    if not trace["valid"] or not compact.get("valid"):
        raise ValueError("invalid API/native trace cannot establish causality")
    session = str(trace["header"]["session"])
    identities = {(row["session"], row["job"]): row for row in compact["job_identities"]}
    native_queues = defaultdict(set)
    for row in compact.get("segments", []):
        native_queues[(row["session"], row["job"])].add(int(row["queue"],16))
    by_sequence = {row["sequence"]: row for row in trace["records"]}
    commands = defaultdict(list)
    for row in trace["records"]:
        if row["operation"] == 4:
            commands[row["parent"]].append(row["other"])
    joined = []
    for row in trace["records"]:
        if row["operation"] != 10:
            continue
        identity = identities.get((session, row["value"]))
        if identity is None:
            continue
        parent = by_sequence.get(row["parent"])
        candidates = []
        while parent:
            if parent["operation"] == 3 and parent["object"] == row["object"] and row["other"] in commands[parent["sequence"]]:
                candidates.append(parent)
            parent = by_sequence.get(parent["parent"])
        batch = candidates[-1] if candidates else None
        joined.append(dict(job=row["value"], command=row["other"], queue=row["object"],
            gpu_start_ms=identity["start_ms"],
            gpu_end_ms=identity["start_ms"]+identity["duration_ticks"]*1000/identity["frequency"],
            batch_sequence=batch["sequence"] if batch else None,
            submit_api_begin_ms=batch["api_begin_ms"] if batch else None,
            submit_api_end_ms=batch["api_end_ms"] if batch else None,
            reason="exact nested batch/list/job identity" if batch else "no exact enclosing batch; not guessed from proximity"))
    grouped = defaultdict(list)
    for row in joined:
        grouped[row["job"]].append(row)
    output = []
    for (identity_session, job_id), identity in identities.items():
        if identity_session != session:
            continue
        matches = grouped[job_id]
        if matches:
            selected = dict(matches[0])
            if any(row["queue"] != selected["queue"] or row["batch_sequence"] != selected["batch_sequence"] for row in matches[1:]):
                selected.update(batch_sequence=None, submit_api_begin_ms=None, submit_api_end_ms=None,
                                reason="ambiguous nested/native submission association")
            output.append(selected)
            continue
        queues = native_queues[(session,job_id)]
        known = [row["object"] for row in trace["objects"] if row["other"] == 1 and row["raw"] in queues]
        output.append(dict(job=job_id, command=None, queue=known[0] if len(known) == 1 else None,
            gpu_start_ms=identity["start_ms"], gpu_end_ms=identity["start_ms"]+identity["duration_ticks"]*1000/identity["frequency"],
            batch_sequence=None, submit_api_begin_ms=None, submit_api_end_ms=None,
            reason="native job has no exact submission record; do not skip it when selecting next GPU work"))
    return output


def gap_evidence(trace, jobs, gaps):
    by_sequence = {row["sequence"]: row for row in trace["records"]}
    dependencies = {row["wait_sequence"]: row for row in trace["dependencies"]}
    result = []
    for gap in gaps:
        begin, end, queue = gap["start_ms"], gap["end_ms"], gap["queue"]
        if not begin < end:
            raise ValueError("invalid gap")
        following = sorted((row for row in jobs if row["queue"] == queue and row["gpu_start_ms"] >= end-0.0001), key=lambda row: row["gpu_start_ms"])
        target = following[0] if following else None
        not_submitted = max(0, min(end, target["submit_api_begin_ms"])-begin) if target and target["batch_sequence"] else None
        waits = []
        if target and target["batch_sequence"]:
            for dependency in trace["dependencies"]:
                event = by_sequence[dependency["wait_sequence"]]
                if event["object"] == queue and event["api_end_ms"] <= target["submit_api_begin_ms"] and event["result"] >= 0:
                    unmet = [sequence for sequence in dependency["sampled_unmet_sequences"]
                             if begin <= by_sequence[sequence]["api_begin_ms"] <= end]
                    if unmet:
                        waits.append(dict(wait=dependency["wait_sequence"], sampled_unmet=unmet,
                                          signals=dependencies[dependency["wait_sequence"]]["possible_signal_sequences"]))
        result.append(dict(**gap, next_job=target, known_not_submitted_ms=not_submitted,
            observed_unmet_wait_candidates=waits, proven_gpu_wait_ms=None, proven_ready_unscheduled_ms=None,
            classification="CPU submission lateness exists" if not_submitted else "submitted or unassociated; GPU cause unresolved",
            limitation="No automatic driver/bandwidth blame; prior queue work and validated scheduler data still required"))
    return result


def source_gaps(trace, compact, copies, log_path):
    from analyze_etl_scheduler import frame_records, union, clipped, without
    if copies["rejected"]:
        raise ValueError("invalid COPY records")
    session = trace["header"]["session"]
    frames = {row["seal"]: row for row in copies["frames"] if int(row["session"]) == session}
    records = frame_records(log_path, session)
    queue_pointers = set()
    for line in log_path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if "[M336] kind=queue " not in line:
            continue
        row = dict(re.findall(r"(\w+)=([^\s]+)", line))
        if row.get("session") == str(session) and row.get("role") == "tagged_submission":
            queue_pointers.add(int(row["queue"], 16))
    if len(queue_pointers) != 1:
        raise ValueError("main source queue identity ambiguous")
    pointer = queue_pointers.pop()
    identities = [row["object"] for row in trace["objects"] if row["other"] == 1 and row["raw"] == pointer]
    if len(identities) != 1:
        raise ValueError("main queue lifetime cannot be uniquely mapped")
    main = union([(row["start_ms"], row["end_ms"]) for row in compact["segments"]
                  if row["session"] == str(session) and int(row["queue"],16) == pointer])
    bins = []
    for seal in sorted(frames):
        if seal-1 not in frames:
            continue
        begin, end = float(records[seal-1]["producer"]["ready_ms"]), float(records[seal]["producer"]["ready_ms"])
        if main and main[0][0] <= begin < end <= main[-1][1]:
            bins.append((seal, begin, end))
    first = next((seal for seal, _, _ in bins if all(any(other == seal+offset for other, _, _ in bins) for offset in range(5))), None)
    if first is None:
        raise ValueError("no five consecutive native-covered source cycles")
    gaps = []
    for seal, begin, end in bins:
        if not first <= seal < first+5:
            continue
        previous = frames[seal-1]["producer"]
        outbound = [(previous[name+"_start"], previous[name+"_end"]) for name in ("color","depth","motion")]
        for gap_begin, gap_end in without(main,begin,end):
            for label, portions in (("COPY outside main DIRECT", clipped(outbound,gap_begin,gap_end)),
                                    ("neither main DIRECT nor outbound COPY", without(outbound,gap_begin,gap_end))):
                for start, stop in portions:
                    gaps.append(dict(seal=seal, queue=identities[0], start_ms=start, end_ms=stop, coverage_class=label))
    return gaps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compact", type=Path)
    parser.add_argument("--gaps", type=Path)
    parser.add_argument("--copy", type=Path)
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    payload = args.trace.read_bytes()
    report = decode(payload)
    report["sha256"] = hashlib.sha256(payload).hexdigest()
    if args.compact:
        compact = json.loads(args.compact.read_text(encoding="utf-8-sig"))
        report["joined_jobs"] = join_jobs(report, compact)
        report["job_join_coverage"] = dict(native_jobs=len(report["joined_jobs"]),
            exact_batch_links=sum(row["batch_sequence"] is not None for row in report["joined_jobs"]),
            unknown_queue=sum(row["queue"] is None for row in report["joined_jobs"]))
    if args.copy or args.log:
        if not args.copy or not args.log or not args.compact:
            parser.error("--copy and --log require --compact and each other")
        gaps = source_gaps(report, compact, json.loads(args.copy.read_text(encoding="utf-8-sig")), args.log)
        report["gaps"] = gap_evidence(report, report["joined_jobs"], gaps)
    if args.gaps:
        report["gaps"] = gap_evidence(report, report.get("joined_jobs", []), json.loads(args.gaps.read_text(encoding="utf-8-sig")))
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("valid", "errors", "counts", "kernel_running_verified", "all_gpu_events_visible")}))
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
