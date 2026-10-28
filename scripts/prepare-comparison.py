import argparse
import hashlib
import json
from pathlib import Path
import statistics


def merge(intervals):
    result = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if result and start <= result[-1][1]:
            result[-1][1] = max(end, result[-1][1])
        else:
            result.append([start, end])
    return result


def build(historical_path, current_path):
    historical = json.loads(historical_path.read_text(encoding='utf-8-sig'))
    runs = [{key: run[key] for key in ('version', 'session', 'first_seal', 'bars', 'bins', 'example', 'means')}
            for run in historical['runs']]
    for run, captured in zip(runs, historical['captures']):
        run['capture'] = captured
        run['source_end'] = run['bins'][-1]['end']
        run['detailed_capture'] = True
        run['role'] = 'before' if run['version'] == 'M339c' else 'historical_after'
        run['frame_count'] = 5
    source = json.loads(current_path.read_text(encoding='utf-8-sig'))
    begin, end = source['window']['begin_seconds'], source['window']['end_seconds']
    frame_ids = [row[1] for row in source['frames']]
    boundaries = [begin] + [row[0] / 10_000_000 for row in source['frames']]
    assert len(frame_ids) == 5 and len(set(frame_ids)) == 5
    assert abs(boundaries[-1] - end) < 1e-7
    bins, bars = [], []
    for index in range(5):
        start, finish = boundaries[index:index + 2]
        measured = []
        for lane, track in enumerate(('direct', 'source_compute')):
            intervals = merge([(max(start, event['开始秒']), min(finish, event['结束秒']))
                               for event in source['events'][track]])
            measured.append(sum(last - first for first, last in intervals) * 1000)
            for first, last in intervals:
                bars.append({'lane': lane, 'kind': 'direct' if lane == 0 else 'compute',
                             'start': round((first - begin) * 1000, 6),
                             'end': round((last - begin) * 1000, 6), 'frame': index + 1,
                             'identity': 'source-bin-only'})
        bins.append({'frame': index + 1, 'source_frame': frame_ids[index],
                     'seal': source['frames'][index][2], 'start': round((start - begin) * 1000, 6),
                     'end': round((finish - begin) * 1000, 6), 'period': round((finish - start) * 1000, 6),
                     'direct_ms': measured[0], 'compute_ms': measured[1]})
    for lane, track, kind in ((2, 'out_copy', 'copy'), (3, 'in_copy', 'copy'), (4, 'nr', 'neural')):
        for event in source['events'][track]:
            source_frame = event['源帧']
            if source_frame in frame_ids:
                frame = frame_ids.index(source_frame) + 1
            elif source_frame == frame_ids[0] - 1:
                frame = 0
            else:
                continue
            bars.append({'lane': lane, 'kind': kind, 'start': round((event['开始秒'] - begin) * 1000, 6),
                         'end': round((event['结束秒'] - begin) * 1000, 6), 'frame': frame,
                         'source_frame': source_frame, 'seal': event['seal']})
    runs.append({'version': 'M364 + M363', 'role': 'release_instrumented', 'session': 'M364-120s-window-4.983s',
                 'capture': '2026-10-27 08:11, detailed collection ON', 'frame_count': 5, 'bins': bins, 'bars': bars,
                 'source_end': bins[-1]['end'], 'capture_quality': source['capture_quality'],
                 'means': {key: statistics.mean(row[key] for row in bins) for key in ('period', 'direct_ms', 'compute_ms')},
                 'example': []})
    return {'schema': 'nte-five-frame-comparison-v1', 'runs': runs, 'statistics': historical['statistics'],
            'limitations': historical['limitations'] + [
                'M364/M363 is a separately instrumented capture, not a controlled comparison with M339c or M340.',
                'M364 capture lost one main-stream record and truncated 867 fine jobs; complete coverage is unverified.',
                'No generated-frame scanout timestamps are fabricated from SDK frame counts.'],
            'input_hashes': {'historical_comparison_json': hashlib.sha256(historical_path.read_bytes()).hexdigest(),
                             'm364_export_json': hashlib.sha256(current_path.read_bytes()).hexdigest()},
            'historical_raw_hashes': list(historical['provenance'].values())}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--historical', type=Path, required=True)
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    result = build(arguments.historical, arguments.current)
    arguments.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps([{'version': run['version'], 'frames': len(run['bins']), 'marks': len(run['bars']),
                       'five_frame_total_ms': run['source_end']} for run in result['runs']], indent=2))
