"""Strict M337 analyzer: rejects loss, truncation, invalid state and incomplete coverage."""
import argparse, json, math, re
from collections import defaultdict
from pathlib import Path

PHASES={0:"unclassified",1:"graphics",2:"compute",3:"indirect",4:"raytrace",5:"copy",6:"target_change",7:"capture_tag",8:"sl_sr_begin",9:"sl_sr_end",10:"ngx_begin",11:"ngx_end",12:"tail"}

def parse_status(lines):
    rows=[]
    for line in lines:
        if '[M337] kind=status' in line: rows.append(dict(re.findall(r'(\w+)=([^\s]+)',line)))
    return rows

def analyze(lines):
    lines=list(lines)
    statuses=parse_status(lines)
    errors=[];segments=[];jobs=defaultdict(list);seen=set()
    if any(re.search(r'log queue: [1-9][0-9]* line\(s\) dropped',line) for line in lines): errors.append('log loss')
    if any('kind=truncated' in line for line in lines): errors.append('truncated session')
    for line in lines:
        if '[M337] kind=segment' not in line: continue
        row=dict(re.findall(r'(\w+)=([^\s]+)',line)); key=(row.get('session'),row.get('job'),row.get('point'))
        if key in seen: errors.append('duplicate segment')
        seen.add(key); jobs[(row.get('session'),row.get('job'))].append(row)
        try:
            start,end,duration=(float(row[name]) for name in ('start_ms','end_ms','gpu_ms'))
            if not all(math.isfinite(value) for value in (start,end,duration)) or start<0 or end<start or duration<0: raise ValueError('invalid timestamp')
            if abs(end-start-duration)>0.02: raise ValueError('duration mismatch')
            phase=int(row.get('phase','-1'))
            if phase not in PHASES: raise ValueError('unknown phase')
            if int(row.get('overflow','0')): errors.append('marker overflow')
            segments.append(row)
        except (KeyError,ValueError) as error: errors.append(str(error))
    if any(int(row.get('rejected','0')) or int(row.get('replayed','0')) or int(row.get('invalid','0')) or int(row.get('overflow_jobs','0')) for row in statuses): errors.append('M337 status indicates incomplete/rejected data')
    if any(row.get('reason')=='recording_stopped' and int(row.get('recording','0')) for row in statuses): errors.append('recording jobs still open')
    if any(row.get('reason')=='record_budget_exhausted' for row in statuses): errors.append('job budget exhausted')
    if any(row.get('reason')=='log_queue_drop' for row in statuses): errors.append('log loss')
    sr=[row for row in segments if int(row['phase'])==8]
    return {'valid':not errors,'errors':sorted(set(errors)),'segments':len(segments),'jobs':len(jobs),'gpu_sr_ms':sum(float(row['gpu_ms']) for row in sr) if sr and not errors else None,'low_resolution_scene_gpu_ms':None,'exclusive_game_render_ms':None,'status_count':len(statuses),'limitations':['Rejects incomplete collection before calculating totals.','Target size and pipeline identity do not prove a semantic scene pass.','SR remains unknown without a surviving SR bracket and valid session.']}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('log',type=Path);parser.add_argument('output',type=Path);args=parser.parse_args();report=analyze(args.log.read_text(encoding='utf-8',errors='replace').splitlines());args.output.write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report))
if __name__=='__main__':main()
