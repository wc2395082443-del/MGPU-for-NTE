"""Resolve recorded Reset/submit stacks without naming GPU shader workloads."""
import argparse
import csv
from collections import defaultdict
import json
from pathlib import Path


def resolve(address, modules):
    if address == 0:
        return dict(address=0, status="padding", candidates=[])
    matches=[]
    for module in modules:
        base=int(module["BaseAddress"])
        size=int(module["ModuleMemorySize"])
        if base < 0 or size <= 0 or base+size > 2**64:
            raise ValueError("invalid module address range")
        if base <= address < base+size:
            matches.append(dict(module=module["ModuleName"], path=module["FileName"],
                                base=base, offset=address-base, offset_hex=hex(address-base)))
    return dict(address=address, address_hex=hex(address),
                status="resolved" if len(matches)==1 else "ambiguous" if matches else "unmapped",
                candidates=matches)


def analyze(compact, modules):
    if not compact.get("valid"):
        raise ValueError("compact trace is invalid; do not attribute an incomplete capture")
    stacks={}
    for session,origins in compact.get("origins",{}).items():
        stacks[session]={str(origin):[resolve(address,modules) for address in addresses]
                         for origin,addresses in origins.items()}
    routes=[]
    by_job=defaultdict(list)
    for segment in compact.get("segments",[]):
        by_job[(segment["session"],segment["job"])].append(segment)
    for identity in compact.get("job_identities",[]):
        session=identity["session"]
        row=dict(identity)
        row["reset_stack"]=stacks.get(session,{}).get(str(identity["reset_origin"]))
        row["submit_stack"]=stacks.get(session,{}).get(str(identity["submit_origin"]))
        intervals=by_job[(session,identity["job"])]
        row["queue"]=intervals[0]["queue"] if intervals else None
        row["start_ms"]=min((item["start_ms"] for item in intervals),default=None)
        row["end_ms"]=max((item["end_ms"] for item in intervals),default=None)
        routes.append(row)
    return dict(jobs=len(routes),full_native_boundary_jobs=sum(row["full_native_boundary"] for row in routes),
                origins=stacks,job_routes=routes,actual_sr_gpu_ms=None,exclusive_game_render_ms=None,
                limitations=["CPU Reset and submission callers are provenance, not GPU shader ownership.",
                             "A complete native list bracket still includes barriers and GPU scheduling gaps.",
                             "Module ranges come from the supplied same-process snapshot; no symbols are inferred.",
                             "Unmapped/ambiguous addresses and partial native boundaries remain explicit."])


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("compact",type=Path)
    parser.add_argument("modules",type=Path)
    parser.add_argument("output",type=Path)
    args=parser.parse_args()
    compact=json.loads(args.compact.read_text(encoding="utf8"))
    with args.modules.open(encoding="utf-8-sig",newline="") as stream:
        modules=list(csv.DictReader(stream))
    report=analyze(compact,modules)
    args.output.write_text(json.dumps(report,indent=2),encoding="utf8")
    print(json.dumps({key:report[key] for key in ("jobs","full_native_boundary_jobs","actual_sr_gpu_ms")}))


if __name__=="__main__":
    main()
