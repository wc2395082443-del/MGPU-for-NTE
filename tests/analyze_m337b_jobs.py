"""Validate and expand compact M337b jobs without inferring missing stages."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re


def analyze(lines):
    errors=[]
    expanded=[]
    fragments=defaultdict(dict)
    fragment_counts={}
    for line in lines:
        if "[M337] kind=job_part " not in line:
            expanded.append(line)
            continue
        match=re.search(r"\[M337\] kind=job_part session=(\d+) job=(\d+) part=(\d+) parts=(\d+) payload=(.*)$",line)
        if not match:
            errors.append("malformed job fragment");continue
        session,job,part,count,payload=match.groups()
        key=(session,job);part=int(part);count=int(count)
        if not 2<=count<=32 or not 0<=part<count or not 0<len(payload)<=1300:
            errors.append("invalid job fragment");continue
        if key in fragment_counts and fragment_counts[key]!=count:
            errors.append("conflicting job fragment count")
        fragment_counts[key]=count
        if part in fragments[key]: errors.append("duplicate job fragment")
        fragments[key][part]=payload
    for key,parts in fragments.items():
        if set(parts)!=set(range(fragment_counts[key])):
            errors.append("missing job fragment");continue
        line="".join(parts[index] for index in range(fragment_counts[key]))
        identity=re.search(r"\[M337\] kind=job session=(\d+) job=(\d+) ",line)
        if not identity or identity.groups()!=key:
            errors.append("job fragment identity mismatch");continue
        expanded.append(line)
    lines=expanded
    records=[]
    statuses=defaultdict(list)
    jobs=defaultdict(dict)
    aggregates=defaultdict(list)
    origins=defaultdict(dict)
    identities=[]
    for line in lines:
        if re.search(r"log queue: [1-9][0-9]* line\(s\) dropped",line): errors.append("log loss")
        if "kind=truncated" in line: errors.append("session truncated")
        if "[M337]" not in line: continue
        row=dict(re.findall(r"(\w+)=([^\s]+)",line))
        session=row.get("session")
        if not session: errors.append("missing session");continue
        if row.get("kind")=="origin":
            try:
                origin=int(row["origin"])
                addresses=[int(value,16) for value in row["pcs"].split(",")]
                if not 1<=origin<=64 or len(addresses)!=8 or min(addresses)<0 or max(addresses)>=2**64:
                    raise ValueError("invalid origin record")
                if origin in origins[session]: raise ValueError("duplicate origin record")
                origins[session][origin]=addresses
            except (KeyError,ValueError) as error: errors.append(str(error))
            continue
        if row.get("kind")=="status":
            statuses[session].append(row)
            if row.get("reason")=="process_restart_required": errors.append("process restart required")
        if row.get("kind")=="aggregate": aggregates[session].append(row)
        if row.get("kind")!="job": continue
        try:
            job=int(row["job"])
            if job in jobs[session]: raise ValueError("duplicate job")
            start=float(row["start_ms"])
            frequency=int(row["frequency"])
            if not math.isfinite(start) or start<0 or frequency<=0: raise ValueError("invalid clock")
            points=[]
            for encoded in row["data"].split(","):
                phase,width,height,ticks=encoded.split(":")
                point=dict(phase=int(phase),target_width=int(width),target_height=int(height),ticks=int(ticks,16))
                if not 0<=point["phase"]<=12 or min(point["target_width"],point["target_height"],point["ticks"])<0:
                    raise ValueError("invalid point")
                if points and point["ticks"]<points[-1]["ticks"]: raise ValueError("nonmonotonic timestamp")
                points.append(point)
            if len(points)!=int(row["marks"]) or len(points)<2 or points[-1]["phase"]!=12 or points[0]["ticks"]!=0:
                raise ValueError("missing job boundary")
            if int(row["overflow"]): errors.append("marker overflow")
            if int(row["frame_known"])==2: errors.append("frame identity conflict")
            jobs[session][job]=row
            native_start=int(row["native_start"]) if "native_start" in row else None
            native_end=int(row["native_end"]) if "native_end" in row else None
            if native_start not in (None,0,1) or native_end not in (None,0,1): raise ValueError("invalid native boundary")
            identity=dict(session=session,job=job,command=row.get("command"),
                reset_origin=int(row.get("reset_origin",0)),submit_origin=int(row.get("submit_origin",0)),
                native_start=native_start,native_end=native_end,
                fine_selected=int(row.get("fine_selected",0)),
                fine_candidate=int(row["fine_candidate"]) if "fine_candidate" in row else None,
                start_ms=start,frequency=frequency,duration_ticks=points[-1]["ticks"],
                callback_record=int(row.get("callback_record",0)),
                full_native_boundary=native_start==1 and native_end==1)
            identities.append(identity)
            feature=int(row.get("ngx_feature","4294967295")) if row.get("ngx_known")=="1" else None
            for point_index,(first,last) in enumerate(zip(points,points[1:])):
                records.append(dict(session=session,job=job,queue=row["queue"],type=int(row["type"]),
                    engine_frame=int(row["engine_frame"]) if row["frame_known"]=="1" else None,
                    point_index=point_index,phase=first["phase"],target_width=first["target_width"],target_height=first["target_height"],
                    ngx_handle=row.get("ngx_handle"),ngx_feature=feature,
                    start_ms=start+first["ticks"]*1000/frequency,end_ms=start+last["ticks"]*1000/frequency,
                    gpu_ms=(last["ticks"]-first["ticks"])*1000/frequency))
        except (KeyError,ValueError) as error: errors.append(str(error))
    if not jobs: errors.append("no compact jobs")
    for session,rows in jobs.items():
        final=[row for row in statuses[session] if row.get("reason")=="submitted_jobs_drained"]
        if len(final)!=1: errors.append("missing unique final status");continue
        row=final[0]
        invalid_keys=["rejected","replayed","invalid","overflow_jobs","invalid_timestamps","encoding_errors","submitted_pending","scope_pending"]
        if row.get("native_boundary_mode")!="1": invalid_keys.extend(("closed_unsubmitted","recording"))
        if any(int(row.get(key,"0")) for key in invalid_keys):
            errors.append("final status invalid")
        if int(row.get("retired","-1"))!=len(rows): errors.append("missing retired job records")
        if not aggregates[session]: errors.append("missing queue summaries")
        elif sum(int(summary["jobs"]) for summary in aggregates[session])!=len(rows): errors.append("queue job count mismatch")
        if any(int(summary.get("frame_conflict","0")) or int(summary.get("overflow_jobs","0")) for summary in aggregates[session]):
            errors.append("queue summary invalid")
        queue_keys=[summary.get("queue",str(index)) for index,summary in enumerate(aggregates[session])]
        if len(set(queue_keys))!=len(queue_keys): errors.append("duplicate queue summary")
    by_queue=defaultdict(list)
    for record in records: by_queue[(record["session"],record["queue"])].append(record)
    queue_totals=[]
    for (session,queue),entries in by_queue.items():
        intervals=[]
        for entry in sorted(entries,key=lambda row:row["start_ms"]):
            if intervals and entry["start_ms"]<=intervals[-1][1]: intervals[-1][1]=max(intervals[-1][1],entry["end_ms"])
            else: intervals.append([entry["start_ms"],entry["end_ms"]])
        queue_totals.append(dict(session=session,queue=queue,inclusive_union_ms=sum(end-start for start,end in intervals),
            inclusive_sum_ms=sum(entry["gpu_ms"] for entry in entries),
            measured_start_ms=min(entry["start_ms"] for entry in entries),
            measured_end_ms=max(entry["end_ms"] for entry in entries),
            frame_known_segments=sum(entry["engine_frame"] is not None for entry in entries)))
    for identity in identities:
        for key in ("reset_origin","submit_origin"):
            if identity[key] and identity[key] not in origins[identity["session"]]: errors.append("missing origin record")
    valid=not errors
    return dict(valid=valid,errors=sorted(set(errors)),jobs=sum(map(len,jobs.values())),segments=records,
        job_identities=identities,origins=dict(origins),statuses=dict(statuses),
        summaries=dict(aggregates),queue_totals=queue_totals,
        ngx_feature_interval_ms={str(feature):sum(row["gpu_ms"] for row in records if row["phase"]==10 and row["ngx_feature"]==feature)
                                 for feature in sorted({row["ngx_feature"] for row in records if row["phase"]==10 and row["ngx_feature"] is not None})} if valid else {},
        gpu_sr_ms=sum(row["gpu_ms"] for row in records if row["phase"]==8 or (row["phase"]==10 and row["ngx_feature"]==1))
        if valid and any(row["phase"]==8 or (row["phase"]==10 and row["ngx_feature"]==1) for row in records) else None,
        low_resolution_scene_gpu_ms=None,exclusive_game_render_ms=None,
        limitations=["Intervals include barriers/dependencies between markers, not exclusive shader execution.",
                     "Unidentified command-list frames stay null.","Target size alone does not identify low-resolution scene rendering.",
                     "GPU scheduler must cover the actual job window before execution attribution."])


def main():
    parser=argparse.ArgumentParser();parser.add_argument("log",type=Path);parser.add_argument("output",type=Path)
    args=parser.parse_args();report=analyze(args.log.read_text(encoding="utf8",errors="replace").splitlines())
    args.output.write_text(json.dumps(report,indent=2),encoding="utf8")
    print(json.dumps({key:report[key] for key in ("valid","errors","jobs","gpu_sr_ms")}))
if __name__=="__main__":main()
