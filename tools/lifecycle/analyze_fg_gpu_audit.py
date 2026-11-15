import argparse
import bisect
import collections
import csv
import json
from pathlib import Path
import statistics


def read_rows(path):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        return [{key: float(value) if key == 'gpu_elapsed_ms' else int(value)
                 for key, value in row.items()} for row in csv.DictReader(stream)]


def input_epochs(fences):
    signals = collections.defaultdict(list)
    for row in fences:
        if row['result'] >= 0 and not row['is_wait'] and row['caller_sequence'] and row['caller_source']:
            signals[(row['fence'], row['value'])].append(row)
    epochs = collections.defaultdict(list)
    for row in fences:
        if row['result'] < 0 or not row['is_wait']:
            continue
        matched = [candidate for candidate in signals[(row['fence'], row['value'])]
                   if candidate['queue'] != row['queue']]
        identities = {(candidate['caller_sequence'], candidate['caller_source']) for candidate in matched}
        if len(identities) != 1:
            continue
        sequence, source = next(iter(identities))
        epochs[row['queue']].append(dict(start=row['cpu_begin_qpc'], sequence=sequence, source=source,
            fence=row['fence'], value=row['value'], signal_qpc=matched[0]['cpu_begin_qpc']))
    for rows in epochs.values():
        rows.sort(key=lambda row: row['start'])
    return epochs


class Clock:
    def __init__(self, samples, frequency):
        self.raw_samples = samples
        self.rejected_long_call_samples = [row for row in samples if 'before' in row and 'after' in row and
            (row['after']-row['before'])*1000/frequency > .1]
        accepted = [row for row in samples if row not in self.rejected_long_call_samples]
        self.samples = sorted({row['gpu']: row for row in accepted}.values(), key=lambda row: row['gpu'])
        self.ticks = [row['gpu'] for row in self.samples]
        self.frequency = frequency
        self.residual_ms = []
        for earlier, middle, later in zip(self.samples, self.samples[1:], self.samples[2:]):
            predicted = earlier['cpu'] + (middle['gpu'] - earlier['gpu']) * (later['cpu'] - earlier['cpu']) / (later['gpu'] - earlier['gpu'])
            self.residual_ms.append(abs(predicted - middle['cpu']) * 1000 / frequency)
        self.valid = len(self.samples) >= 3 and bool(self.residual_ms) and max(self.residual_ms) <= .1

    def project(self, timestamp):
        if not self.valid or timestamp < self.ticks[0] or timestamp > self.ticks[-1]:
            return None
        position = min(len(self.samples) - 2, max(0, bisect.bisect_right(self.ticks, timestamp) - 1))
        earlier, later = self.samples[position:position + 2]
        if (later['cpu'] - earlier['cpu']) / self.frequency > 1.5:
            return None
        return earlier['cpu'] + (timestamp - earlier['gpu']) * (later['cpu'] - earlier['cpu']) / (later['gpu'] - earlier['gpu'])


def analyze(folder):
    status = json.loads((folder / 'fg-gpu-audit-status.json').read_text(encoding='utf-8-sig'))
    records = read_rows(folder / 'fg-gpu-evaluations.csv')
    fences = read_rows(folder / 'fg-gpu-queue-fences.csv')
    samples = collections.defaultdict(list)
    with (folder / 'fg-gpu-api.csv').open(encoding='utf-8-sig', newline='') as stream:
        for row in csv.reader(stream):
            if row[0] == 'clock' and int(row[9]) >= 0:
                samples[int(row[3])].append(dict(gpu=int(row[4]), cpu=int(row[5]), before=int(row[7]), after=int(row[8])))
    clocks = {queue: Clock(rows, status['qpc_frequency']) for queue, rows in samples.items()}
    epochs = input_epochs(fences)
    epoch_ticks = {queue: [row['start'] for row in rows] for queue, rows in epochs.items()}
    receipt_path = folder / 'receipt.json'
    receipt = json.loads(receipt_path.read_text(encoding='utf-8-sig')) if receipt_path.exists() else {}
    origin = receipt.get('InputBeginQpc', min((row['cpu_begin_qpc'] for row in records), default=0))
    groups = collections.defaultdict(list)
    for record in records:
        clock = clocks.get(record['queue_index'])
        for field in ['gpu_begin', 'gpu_end']:
            record[field + '_qpc'] = clock.project(record[field]) if clock and record['status'] == 5 else None
            record[field + '_seconds'] = (record[field + '_qpc'] - origin) / status['qpc_frequency'] if record[field + '_qpc'] is not None else None
        queue = record['queue_pointer']
        position = bisect.bisect_right(epoch_ticks.get(queue, []), record['cpu_begin_qpc']) - 1
        epoch = epochs[queue][position] if position >= 0 else None
        record['source_association'] = epoch
        record['generated_ordinal_valid'] = (record['parameter_mask'] & 6) == 6 and 1 <= record['multi_frame_index'] <= record['multi_frame_count']
        if epoch:
            groups[epoch['sequence']].append(record)
    batches = []
    for sequence, rows in sorted(groups.items()):
        counts = {row['multi_frame_count'] for row in rows}
        expected = next(iter(counts)) if len(counts) == 1 else None
        ordinals = [row['multi_frame_index'] for row in rows]
        complete = expected is not None and len(rows) == expected and sorted(ordinals) == list(range(1, expected + 1))
        batches.append(dict(sequence=sequence, source=rows[0]['source_association']['source'],
            expected_generated=expected, ordinals=ordinals, complete_ordinal_set=complete,
            gpu_elapsed_sum_ms=sum(row['gpu_elapsed_ms'] for row in rows if row['status'] == 5),
            evaluation_ids=[row['id'] for row in rows]))
    values = [row['gpu_elapsed_ms'] for row in records if row['status'] == 5]
    result = dict(status=status, qpc_origin=origin, recorded=len(records), gpu_completed=len(values),
        calibrated_records=sum(row['gpu_begin_qpc'] is not None and row['gpu_end_qpc'] is not None for row in records),
        source_associated_records=sum(row['source_association'] is not None for row in records),
        valid_generated_ordinal_records=sum(row['generated_ordinal_valid'] for row in records),
        complete_source_batches=sum(row['complete_ordinal_set'] for row in batches),
        elapsed_ms=dict(mean=statistics.mean(values), minimum=min(values), maximum=max(values)) if values else None,
        clocks={str(queue): dict(samples=len(clock.samples), valid=clock.valid,
            raw_samples=len(clock.raw_samples), rejected_long_call_samples=clock.rejected_long_call_samples,
            max_accepted_calibration_call_ms=.1,
            max_holdout_residual_ms=max(clock.residual_ms, default=None),
            max_call_us=max(((row['after'] - row['before']) * 1e6 / status['qpc_frequency'] for row in clock.samples),default=None),
            extrapolation_allowed=False) for queue, clock in clocks.items()},
        batches=batches, evaluations=records,
        limitations='Timestamps bracket commands recorded by NGX FrameGeneration EvaluateFeature on its supplied list. GPU elapsed includes preemption/barriers; independent-queue work is excluded. Generated ordinal comes from public NGX parameters. Source association uses an exact same-fence/value signal from an identified helper Present and the SDK queue input-wait epoch. This does not assign a native PresentId or prove pixel scanout identity.')
    if 'InputBeginQpc' in receipt:
        formal=[row for row in records if row['gpu_begin_qpc'] is not None and
                receipt['InputBeginQpc']<=row['gpu_begin_qpc']<receipt['InputEndQpc']]
        result['formal_window']=dict(gpu_start_evaluations=len(formal),
            completed=sum(row['status']==5 for row in formal),
            calibrated=sum(row['gpu_end_qpc'] is not None for row in formal),
            source_associated=sum(row['source_association'] is not None for row in formal),
            valid_generated_ordinal=sum(row['generated_ordinal_valid'] for row in formal),
            unassociated_outside_formal_window=[row['id'] for row in records if row['source_association'] is None and
                row not in formal])
    (folder / 'fg-gpu-analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('folder', type=Path)
    parser.add_argument('--require-complete', action='store_true')
    parser.add_argument('--require-formal-complete', action='store_true')
    args = parser.parse_args()
    result = analyze(args.folder)
    print(json.dumps({key: value for key, value in result.items() if key not in ['evaluations', 'batches']}, indent=2))
    if args.require_complete:
        if (not result['recorded'] or result['gpu_completed'] != result['recorded'] or
            result['calibrated_records'] != result['recorded'] or
            result['source_associated_records'] != result['recorded'] or
            result['valid_generated_ordinal_records'] != result['recorded'] or
            result['status']['failures'] or result['status']['overflow']):
            raise SystemExit('FG GPU timing/source/ordinal acceptance failed; preserve raw files')
    if args.require_formal_complete:
        formal=result.get('formal_window',{})
        count=formal.get('gpu_start_evaluations',0)
        if (not count or any(formal.get(key)!=count for key in ('completed','calibrated','source_associated','valid_generated_ordinal')) or
            result['gpu_completed']!=result['recorded'] or result['status']['failures'] or result['status']['overflow']):
            raise SystemExit('Formal-window FG GPU timing/source/ordinal acceptance failed; preserve raw files')
