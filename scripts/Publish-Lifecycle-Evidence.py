import argparse
import hashlib
import html
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import zipfile


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def five_frames(folder):
    manifest = read_json(folder / 'html-viewer/viewer-manifest.json')
    frequency = manifest['frequency']
    frames = manifest['frames']
    index = max(position for position, frame in enumerate(frames) if frame[0] <= 5 * frequency)
    index = min(index, len(frames) - 6)
    begin, end = frames[index][0], frames[index + 5][0]
    sources = {frame[1] for frame in frames[index + 1:index + 6]}
    tracks = {}
    for track in ['direct', 'out_copy', 'in_copy', 'nr1', 'nr2', 'output',
                  *['fg_gpu_' + str(ordinal) for ordinal in range(1, 6)],
                  *['display_slot_' + str(ordinal) for ordinal in range(1, 7)]]:
        path = folder / 'html-viewer/event-data' / (track + '.bin')
        if not path.exists():
            continue
        tracks[track] = [dict(start_ms=(row[0]-begin)*1000/frequency,
            end_ms=(row[1]-begin)*1000/frequency, source=row[2])
            for row in struct.iter_unpack('<6I', path.read_bytes())
            if row[1] >= begin and row[0] <= end + frequency * .04]
    return dict(capture=folder.name, selection='Five consecutive source-ready intervals containing 5.000 seconds; deterministic selection, no fastest-window selection.',
        start_seconds=begin/frequency, end_seconds=end/frequency, duration_ms=(end-begin)*1000/frequency,
        frames=[dict(source=frame[1], boundary_ms=(frame[0]-begin)*1000/frequency) for frame in frames[index + 1:index + 6]],
        source_batches=[manifest['sdk_source_batches'][str(source)] for source in sorted(sources) if str(source) in manifest['sdk_source_batches']],
        fg_evaluations={str(source): manifest['fg_gpu_by_source'].get(str(source), []) for source in sorted(sources)},
        tracks=tracks)


def render_svg(data, target):
    lane_names={'direct': '4080S DIRECT query', 'out_copy': '4080S outbound COPY', 'in_copy': '5080 inbound COPY',
        'nr1': '5080 NR pass 1', 'nr2': '5080 NR pass 2', 'output': '5080 NR output'}
    lane_names.update({'fg_gpu_'+str(ordinal): '5080 FG '+str(ordinal)+' GPU query' for ordinal in range(1, 6)})
    lane_names.update({'display_slot_'+str(ordinal): 'Display Present ordinal '+str(ordinal) for ordinal in range(1, 7)})
    width=1500; left=225; plot=1245; row_height=34
    scale=plot/260
    height=155+len(data['tracks'])*row_height
    pieces=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        '<g font-family="Arial,sans-serif" font-size="12" fill="#263746">',
        '<text x="15" y="25" font-size="18">B09C + E172: five measured source-ready intervals</text>',
        f'<text x="15" y="46">Capture {html.escape(data["capture"])} | {data["start_seconds"]:.6f}–{data["end_seconds"]:.6f}s | five intervals {data["duration_ms"]:.3f}ms</text>',
        '<text x="15" y="66">Blue = NGX FG GPU query. Vertical lines = system display records, not verified individual scanout images.</text>']
    for tick in range(0,261,20):
        horizontal=left+tick*scale
        pieces.extend([f'<path d="M{horizontal:.2f},112V{height-24}" stroke="#e2e8ed"/>',f'<text x="{horizontal:.2f}" y="87">{tick}ms</text>'])
    previous=0
    for index,frame in enumerate(data['frames'],1):
        horizontal=left+frame['boundary_ms']*scale
        center=left+(previous+frame['boundary_ms'])*scale/2
        pieces.append(f'<text x="{center:.2f}" y="104" text-anchor="middle">F{index} / source {frame["source"]}</text>')
        pieces.append(f'<path d="M{horizontal:.2f},112V{height-24}" stroke="#98a9b8" stroke-dasharray="3 3"/>')
        previous=frame['boundary_ms']
    for index,(track,rows) in enumerate(data['tracks'].items()):
        top=119+index*row_height
        pieces.append(f'<text x="12" y="{top+17}">{html.escape(lane_names[track])}</text>')
        if track=='direct':
            merged=[]
            for row in sorted(rows,key=lambda row:row['start_ms']):
                if merged and row['start_ms']<=merged[-1]['end_ms']:
                    merged[-1]['end_ms']=max(merged[-1]['end_ms'],row['end_ms'])
                else:
                    merged.append(dict(row))
            rows=merged
        for row in rows:
            lower=max(0,row['start_ms']); upper=min(260,row['end_ms'])
            if upper<lower or lower>260:
                continue
            color='#309bff' if track.startswith('fg_gpu') else '#fb8535' if track.startswith('nr') else '#9c7af0' if 'copy' in track or track=='output' else '#728b9c'
            if track.startswith('display_slot'):
                horizontal=left+lower*scale
                pieces.append(f'<path d="M{horizontal:.2f},{top+1}v23" stroke="#267b70" stroke-width="2"><title>Source {row["source"]}; display {row["start_ms"]:.6f}ms</title></path>')
            else:
                pieces.append(f'<rect x="{left+lower*scale:.2f}" y="{top+1}" width="{max(.4,(upper-lower)*scale):.2f}" height="22" fill="{color}"><title>Source {row["source"]}: {row["start_ms"]:.6f}–{row["end_ms"]:.6f}ms</title></rect>')
    pieces.extend(['</g></svg>'])
    target.write_text('\n'.join(pieces),encoding='utf-8',newline='\n')


def publish(args):
    repo=Path(__file__).resolve().parent.parent
    destination=repo/'docs/nte/evidence/preview4'
    destination.mkdir(parents=True,exist_ok=True)
    report=[]
    for label,folder in [('b7d3',args.failed),('ae5c',args.previous),('b09c',args.current)]:
        manifest=read_json(folder/'html-viewer/viewer-manifest.json')
        summary=read_json(folder/'compact-summary.json')
        diagnosis=read_json(folder/'fg-display-pacing-diagnosis.json')['summary']
        selection=five_frames(folder)
        (destination/(label+'-five-frames.json')).write_text(json.dumps(selection,ensure_ascii=False,separators=(',',':')),encoding='utf-8',newline='\n')
        public=dict(version=label,capture=folder.name,duration_seconds=manifest['duration'],
            source_fps=summary['helper_source_per_second'],sdk_count_per_second=summary['sdk_reported_per_second'],
            sdk_count_distribution=summary['sdk_counts'],display_intervals_ms=summary['positive_display_interval_ms'],
            display_gaps_over20ms=diagnosis['long_display_gaps'],source_cadence=diagnosis['source_cadence_comparison'],
            mouse_records=summary['mouse_records'],foreground_samples=summary['foreground_samples'],window_samples=summary['gui_records'],
            main_dropped=manifest['main_dropped_records'],fine_truncated=manifest['fine_truncated_jobs'],
            fg_summary=manifest['fg_gpu_summary'],sdk_batch_audit=manifest['sdk_batch_audit'],
            runtime_admission_audit=manifest.get('source_admission_runtime_audit'),
            five_frame_window=dict(start_seconds=selection['start_seconds'],end_seconds=selection['end_seconds'],duration_ms=selection['duration_ms']))
        (destination/(label+'-summary.json')).write_text(json.dumps(public,ensure_ascii=False,indent=2),encoding='utf-8',newline='\n')
        report.append(public)
        if label=='b09c':
            render_svg(selection,repo/'docs/nte/figures/lifecycle-five-frames.svg')
        if args.viewers:
            temporary=args.viewers/label
            temporary.mkdir(parents=True,exist_ok=True)
            shutil.copytree(folder/'html-viewer/event-data',temporary/'event-data',dirs_exist_ok=True)
            manifest['raw_directory']='Published capture '+folder.name+'; raw ETL retained by maintainer'
            specification=importlib.util.spec_from_file_location('package_timeline',args.tools/'export_standalone_timeline.py')
            module=importlib.util.module_from_spec(specification)
            import sys
            sys.path.insert(0,str(args.tools))
            specification.loader.exec_module(module)
            module.package_html(temporary,manifest)
            page=temporary/'pipeline-timeline.html'
            page.write_bytes(page.read_bytes().replace(b'\r\n',b'\n'))
            if label=='b09c':
                shutil.copy2(temporary/'pipeline-timeline.html',repo/'docs/nte/lifecycle-five-frame-timeline.html')
    (destination/'comparison.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8',newline='\n')
    if args.viewers:
        archive=args.viewers/'MGPU-for-NTE-five-frame-evidence-preview.4.zip'
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as output:
            for label in ['b7d3','ae5c','b09c']:
                output.write(args.viewers/label/'pipeline-timeline.html',label+'-pipeline-timeline.html')
            for path in destination.glob('*.json'):
                output.write(path,'data/'+path.name)
        print(str(archive))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--failed',type=Path,required=True)
    parser.add_argument('--previous',type=Path,required=True)
    parser.add_argument('--current',type=Path,required=True)
    parser.add_argument('--tools',type=Path,default=Path(__file__).resolve().parent.parent/'tools/lifecycle')
    parser.add_argument('--viewers',type=Path)
    publish(parser.parse_args())
