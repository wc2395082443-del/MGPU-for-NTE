"""Decode bounded native operation pairs; never infer shader meaning from API kind."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tests"))
from analyze_m337b_jobs import analyze as analyze_base

KINDS=("", "draw", "draw_indexed", "dispatch", "copy_buffer", "copy_texture",
       "copy_resource", "barrier", "indirect", "bundle", "meta_initialize",
       "meta_execute", "acceleration_build", "rays", "uav_clear_uint", "uav_clear_float")
CALLBACKS=("acceleration_build","uav_uint","uav_float","bundle")


def analyze(lines):
    lines=list(lines)
    base=analyze_base(lines)
    errors=list(base["errors"])
    jobs={(row["session"],row["job"]):row for row in base["job_identities"]}
    segments=defaultdict(dict)
    for row in base["segments"]:
        segments[(row["session"],row["job"])][row["point_index"]]=row
    fragments=defaultdict(dict)
    counts={}
    expanded=[]
    for line in lines:
        if "[M339] kind=fine_part " not in line:
            expanded.append(line)
            continue
        match=re.search(r"\[M339\] kind=fine_part session=(\d+) job=(\d+) part=(\d+) parts=(\d+) payload=(.*)$",line)
        if not match:
            errors.append("malformed fine fragment")
            continue
        session,job,part,count,payload=match.groups()
        key=(session,int(job)); part=int(part); count=int(count)
        if not 2<=count<=32 or not 0<=part<count or not 0<len(payload)<=1300:
            errors.append("invalid fine fragment")
            continue
        if key in counts and counts[key]!=count: errors.append("conflicting fine fragment count")
        if part in fragments[key]: errors.append("duplicate fine fragment")
        counts[key]=count; fragments[key][part]=payload
    for key,parts in fragments.items():
        if set(parts)!=set(range(counts[key])):
            errors.append("missing fine fragment")
            continue
        line="".join(parts[index] for index in range(counts[key]))
        match=re.search(r"\[M339\] kind=fine session=(\d+) job=(\d+) ",line)
        if not match or (match[1],int(match[2]))!=key:
            errors.append("fine fragment identity mismatch")
            continue
        expanded.append(line)
    labels={}
    fine={}
    coverage=[]
    observed={}
    callbacks={}
    for line in expanded:
        if "[M339]" not in line: continue
        row=dict(re.findall(r"(\w+)=([^\s]*)",line))
        try:
            if row.get("kind")=="coverage":
                mask=int(row["mask"]); extended=int(row["extended"])
                if not 0<=mask<32768 or extended not in (0,1): raise ValueError("invalid coverage mask")
                coverage.append(dict(session=row.get("session"),mask=mask,extended=bool(extended),failures=int(row["failures"])))
                if int(row["failures"]): errors.append("native hook coverage failure")
            if row.get("kind")=="label":
                key=(row["session"],int(row["label"]))
                encoding=int(row["encoding"]); data=bytes.fromhex(row["data"])
                if not 1<=key[1]<=64 or key in labels or encoding not in (0,1,2) or len(data)!=int(row["bytes"]) or len(data)>96:
                    raise ValueError("invalid label")
                if encoding==0 and data: raise ValueError("unknown label with data")
                if encoding==2 and len(data)%2: raise ValueError("invalid wide label")
                text=data.decode("utf-16le" if encoding==2 else "latin1").rstrip("\0") if encoding else None
                labels[key]=dict(object=int(row["object"],16),text=text,encoding=encoding)
            if row.get("kind")=="seen":
                key=(row["session"],int(row["job"]))
                if key in observed: raise ValueError("duplicate observed job")
                if key not in jobs or jobs[key].get("fine_candidate")!=1: raise ValueError("observed job not a candidate")
                counts=[int(value) for value in row["counts"].split(",")]
                total=int(row["total"]); budget_skipped=int(row["budget_skipped"]); scope_skipped=int(row["scope_skipped"])
                mixed=int(row["mixed"]); truncated=int(row["truncated"]); cutoff=int(row["cutoff"])
                continues=int(row.get("mixed_continues",0))
                if len(counts) not in (13,15) or min(counts)<0 or max(counts)>=2**32 or sum(counts)!=total or continues not in (0,1):
                    raise ValueError("invalid observed operation counts")
                if not 0<=budget_skipped<=total or not 0<=scope_skipped<=total-budget_skipped or any(value not in (0,1) for value in (mixed,truncated,cutoff)):
                    raise ValueError("invalid observed skip counts")
                observed[key]=dict(session=key[0],job=key[1],total=total,budget_skipped=budget_skipped,scope_skipped=scope_skipped,
                    mixed=bool(mixed),truncated=bool(truncated),cutoff=bool(cutoff),
                    mixed_continues=bool(continues),
                    complete_hook_observation=(not mixed or continues==1) and not truncated and not cutoff,
                    operation_counts=dict(zip(KINDS[1:],counts)))
            if row.get("kind")=="callbacks":
                key=(row["session"],int(row["job"]))
                if key in callbacks: raise ValueError("duplicate callback job")
                if key not in jobs or jobs[key].get("callback_record")!=1: raise ValueError("unexpected callback job")
                counts=[int(value) for value in row["counts"].split(",")]
                origins=[int(value) for value in row["origins"].split(",")]
                points=[int(value) for value in row["points"].split(",")]
                total=int(row["total"])
                if any(len(values)!=4 for values in (counts,origins,points)) or min(counts)<0 or sum(counts)!=total or total<=0:
                    raise ValueError("invalid callback counts")
                details=[]
                for kind,count,origin,point in zip(CALLBACKS,counts,origins,points):
                    if not 0<=origin<=64 or (origin and origin not in base["origins"].get(key[0],{})):
                        raise ValueError("invalid callback origin")
                    if point!=2**32-1 and point not in segments[key]: raise ValueError("invalid callback point")
                    if not count and (origin or point!=2**32-1): raise ValueError("unused callback metadata")
                    details.append(dict(kind=kind,count=count,origin=origin,first_point=None if point==2**32-1 else point,
                        first_start_ms=segments[key][point]["start_ms"] if point in segments[key] else None))
                callbacks[key]=dict(session=key[0],job=key[1],total=total,callbacks=details)
            if row.get("kind")!="fine": continue
            key=(row["session"],int(row["job"]))
            if key in fine: raise ValueError("duplicate fine job")
            if key not in jobs or jobs[key]["fine_selected"]!=1: raise ValueError("fine job not selected")
            if not jobs[key]["full_native_boundary"]: raise ValueError("fine job lacks native bounds")
            count=int(row["count"]); truncated=int(row["truncated"]); mixed=int(row["mixed"])
            timing=row.get("timing","points"); continues=int(row.get("mixed_continues",0))
            if not 0<=count<=160 or truncated not in (0,1) or mixed not in (0,1): raise ValueError("invalid fine flags")
            if (timing,continues) not in (("points",0),("queries",1)): raise ValueError("invalid fine timing mode")
            operations=[]
            previous_end=-1
            for encoded in row["data"].split(",") if row["data"] else []:
                values=encoded.split(":")
                if len(values)!=11: raise ValueError("malformed operation")
                begin,end=(int(value,16 if timing=="queries" else 10) for value in values[:2])
                kind,origin=map(int,values[2:4])
                pipeline,obj,arg0,arg1,arg2=(int(value,16) for value in values[4:9])
                pipeline_label,object_label=map(int,values[9:])
                points=segments[key]
                if timing=="points":
                    if not 0<=begin<end<=max(points)+1 or begin not in points or begin<=previous_end:
                        raise ValueError("invalid or overlapping operation pair")
                    start=points[begin]["start_ms"]
                    finish=points[end]["start_ms"] if end in points else points[end-1]["end_ms"]
                else:
                    identity=jobs[key]
                    if not 0<=begin<=end<=identity["duration_ticks"] or begin<previous_end:
                        raise ValueError("invalid or overlapping independent query pair")
                    start=identity["start_ms"]+begin*1000/identity["frequency"]
                    finish=identity["start_ms"]+end*1000/identity["frequency"]
                if not 1<=kind<len(KINDS) or not 0<=origin<=64 or min(pipeline,obj,arg0,arg1,arg2)<0 or max(pipeline,obj,arg0,arg1,arg2)>=2**64:
                    raise ValueError("invalid operation metadata")
                if origin and origin not in base["origins"].get(key[0],{}): raise ValueError("missing operation origin")
                if not 0<=pipeline_label<=64 or not 0<=object_label<=64: raise ValueError("invalid operation label reference")
                operations.append(dict(begin_index=begin if timing=="points" else None,end_index=end if timing=="points" else None,
                    begin_ticks=begin if timing=="queries" else None,end_ticks=end if timing=="queries" else None,
                    kind=KINDS[kind],origin=origin,
                    pipeline=pipeline,object=obj,args=[arg0,arg1,arg2],pipeline_label=pipeline_label,object_label=object_label,
                    start_ms=start,end_ms=finish,bracket_ms=finish-start))
                previous_end=end
            if len(operations)!=count: raise ValueError("fine count mismatch")
            points=segments[key]
            total=max(item["end_ms"] for item in points.values())-min(item["start_ms"] for item in points.values())
            measured=sum(item["bracket_ms"] for item in operations)
            if measured>total+0.00001: raise ValueError("fine brackets exceed job")
            fine[key]=dict(session=key[0],job=key[1],truncated=bool(truncated),mixed=bool(mixed),
                timing=timing,mixed_continues=bool(continues),
                complete_probe=not truncated and (not mixed or continues==1),operations=operations,job_bracket_ms=total,
                unclassified_remainder_ms=max(0,total-measured))
        except (KeyError,ValueError,TypeError) as error:
            errors.append(str(error))
    for key,job in jobs.items():
        if job["fine_selected"] and key not in fine: errors.append("missing fine job")
        if job.get("fine_candidate")==1 and key not in observed: errors.append("missing observed job")
        if job.get("fine_candidate") not in (None,0,1): errors.append("invalid candidate flag")
        if job.get("callback_record")==1 and key not in callbacks: errors.append("missing callback record")
        if job.get("callback_record") not in (None,0,1): errors.append("invalid callback flag")
    for key,job in fine.items():
        if key not in observed: continue
        counts=observed[key]
        job["complete_probe"]=job["complete_probe"] and counts["complete_hook_observation"] and not counts["scope_skipped"] and not counts["budget_skipped"]
        if counts["total"]<len(job["operations"]) or counts["mixed"]!=job["mixed"] or counts["truncated"]!=job["truncated"]:
            errors.append("observed and fine records conflict")
        if counts["mixed_continues"]!=job["mixed_continues"]: errors.append("mixed continuation conflict")
    if fine and not coverage: errors.append("missing native hook coverage")
    budgets=[]
    operation_limits={}
    for session,statuses in base["statuses"].items():
        for row in statuses:
            if row.get("reason")!="submitted_jobs_drained" or "fine_selected" not in row: continue
            try:
                selected=int(row["fine_selected"]); general=int(row["fine_general"]); meta=int(row["fine_meta"])
                refused=int(row["fine_selection_refused"])
                limits=tuple(int(row.get(name,default)) for name,default in
                    (("fine_general_limit",8),("fine_meta_limit",16),("fine_job_limit",24)))
                if limits not in ((8,16,24),(1024,16,1040)) or not 0<=general<=limits[0] or not 0<=meta<=limits[1] or selected!=general+meta or refused<0:
                    raise ValueError("invalid detail budget")
                operation_limit=int(row.get("fine_operation_limit",16))
                if operation_limit not in (16,160): raise ValueError("invalid detail budget")
                operation_limits[session]=operation_limit
                declared_continues=int(row.get("fine_mixed_continues",0))
                if declared_continues not in (0,1) or any(job["mixed_continues"]!=bool(declared_continues) for key,job in fine.items() if key[0]==session):
                    raise ValueError("invalid detail budget")
                budgets.append(dict(session=session,selected=selected,general=general,meta=meta,selection_refused=refused,
                    general_limit=limits[0],meta_limit=limits[1],job_limit=limits[2],operation_limit=operation_limit))
            except (KeyError,ValueError): errors.append("invalid detail budget")
    for key,job in fine.items():
        if len(job["operations"])>operation_limits.get(key[0],16): errors.append("fine count exceeds declared operation limit")
        for operation in job["operations"]:
            for field,pointer in (("pipeline_label","pipeline"),("object_label","object")):
                label=operation[field]
                if label:
                    metadata=labels.get((key[0],label))
                    if metadata is None or metadata["object"]!=operation[pointer]: errors.append("missing or mismatched label")
                    else: operation[field+"_text"]=metadata["text"]
    return dict(valid=not errors,errors=sorted(set(errors)),base_valid=base["valid"],base_jobs=base["jobs"],
        fine_jobs=list(fine.values()),observed_jobs=list(observed.values()),callback_jobs=list(callbacks.values()),hook_coverage=coverage,detail_budgets=budgets,actual_sr_ms=None,low_resolution_scene_gpu_ms=None,
        limitations=["GPU timestamp brackets include dependencies/barriers, not exclusive shader execution.",
                     "MetaCommand opcode or CPU caller alone does not identify actual SR.",
                     "Legacy mixed-stop, truncated, cut-off or suppressed scopes are incomplete; unobserved remainder remains unknown.",
                     "No native MetaCommand hardware coverage is established by WARP."])


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("log",type=Path); parser.add_argument("output",type=Path)
    args=parser.parse_args()
    result=analyze(args.log.read_text(encoding="utf8",errors="replace").splitlines())
    args.output.write_text(json.dumps(result,indent=2),encoding="utf8")
    print(json.dumps(dict(valid=result["valid"],errors=result["errors"],base_jobs=result["base_jobs"],fine_jobs=len(result["fine_jobs"]))))
    raise SystemExit(0 if result["valid"] else 1)


if __name__=="__main__": main()
