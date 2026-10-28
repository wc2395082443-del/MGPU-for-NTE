"""Validate clock projection against ETW emission, without game frame timings."""
import argparse
import json
from pathlib import Path

from analyze_etl_scheduler import decode_log, integer, read_events
from scheduler_clock_map import ClockMap


def analyze(events, clocks):
    clock_map=ClockMap(clocks)
    adapters={}
    for event in events:
        if event["provider"]=="dxg" and event["id"]==110 and "AdapterLuid" in event["fields"]:
            adapters[event["fields"]["pDxgAdapter"][0]]=integer(event["fields"]["AdapterLuid"][0])
    results={hex(luid):dict(mapped=0,outside_coverage=0,future_violations=0,max_future_ms=None) for luid in clock_map.adapters}
    unknown_adapters=0
    for event in events:
        if event["provider"]!="dxg" or event["id"]!=432: continue
        fields=event["fields"]
        luid=adapters.get(fields["hAdapter"][0])
        if luid not in clock_map.adapters:
            unknown_adapters+=1;continue
        row=results[hex(luid)]
        for entry in decode_log(fields):
            point=clock_map.project(luid,entry["gpu_timestamp"],integer(fields["NodeOrdinal"][0]))
            if point is None:
                row["outside_coverage"]+=1;continue
            row["mapped"]+=1
            future=point-event["qpc_ms"]
            row["max_future_ms"]=future if row["max_future_ms"] is None else max(row["max_future_ms"],future)
            row["future_violations"]+=future>1
    return dict(purpose="Clock-domain pilot, no FPS/frame decomposition",raw_xml_matches=len(events),
        validation=clock_map.validation,adapters=results,unknown_adapter_snapshots=unknown_adapters,
        all_requested_adapters_observed=all(row["mapped"]>0 for row in results.values()),
        observed_causality_pass=any(row["mapped"] for row in results.values()) and not any(row["future_violations"] for row in results.values()),
        absolute_precision_guaranteed=False)


def main():
    parser=argparse.ArgumentParser()
    for name in ("raw","xml","clocks","output"): parser.add_argument("--"+name,type=Path,required=True)
    args=parser.parse_args()
    result=analyze(read_events(args.raw,args.xml),json.loads(args.clocks.read_text(encoding="utf-8-sig")))
    args.output.write_text(json.dumps(result,indent=2),encoding="utf8")
    print(json.dumps(result,indent=2))


if __name__=="__main__": main()
