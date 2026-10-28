import json
from pathlib import Path
import statistics
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
DATA = json.loads((ROOT / 'docs/nte/evidence/five-frame-comparison.json').read_text(encoding='utf-8'))


class ComparisonTest(unittest.TestCase):
    def test_five_contiguous_actual_bins_each(self):
        for run in DATA['runs']:
            self.assertEqual([row['frame'] for row in run['bins']], [1, 2, 3, 4, 5])
            for index, row in enumerate(run['bins']):
                self.assertAlmostEqual(row['end'] - row['start'], row['period'], places=5)
                if index:
                    self.assertAlmostEqual(run['bins'][index - 1]['end'], row['start'], places=5)
            self.assertAlmostEqual(statistics.mean(row['period'] for row in run['bins']), run['means']['period'], places=5)
            self.assertAlmostEqual(run['bins'][-1]['end'], run['source_end'], places=5)

    def test_exact_plot_counts_and_shared_scale(self):
        namespace = {'svg': 'http://www.w3.org/2000/svg'}
        before_after = ET.parse(ROOT / 'docs/nte/figures/five-frame-comparison.svg')
        current = ET.parse(ROOT / 'docs/nte/figures/m364-instrumented-five-frames.svg')
        scales = []
        for run, document, role in ((DATA['runs'][0], before_after, 'before'),
                                    (DATA['runs'][1], before_after, 'after'),
                                    (DATA['runs'][2], current, 'current')):
            marks = [item for item in document.findall('.//svg:rect', namespace) if item.get('data-run') == role]
            self.assertEqual(len(marks), len(run['bars']))
            chosen = next(item for item in marks if float(item.get('data-end-ms')) - float(item.get('data-start-ms')) > 1)
            duration = float(chosen.get('data-end-ms')) - float(chosen.get('data-start-ms'))
            scales.append(float(chosen.get('width')) / duration)
        self.assertAlmostEqual(scales[0], scales[1], places=5)
        self.assertAlmostEqual(scales[0], scales[2], places=5)

    def test_handoff_change_is_actual_overlap(self):
        before, after = DATA['runs'][:2]
        def overlap(run):
            send = [bar for bar in run['example'] if bar['lane'] == 2]
            receive = [bar for bar in run['example'] if bar['lane'] == 3 and bar['kind'] != 'interval']
            return sum(max(0, min(first['end'], last['end']) - max(first['start'], last['start']))
                       for first in send for last in receive)
        self.assertEqual(overlap(before), 0)
        self.assertGreater(overlap(after), 1)

    def test_current_capture_not_promoted_to_clean_ab(self):
        current = DATA['runs'][2]
        self.assertEqual(current['role'], 'release_instrumented')
        self.assertEqual(current['capture_quality']['main_dropped_records'], 1)
        self.assertEqual(current['capture_quality']['fine_truncated_jobs'], 867)
        self.assertFalse(current['capture_quality']['complete_pipeline_verified'])
        self.assertIn('detailed collection ON', current['capture'])

    def test_no_private_paths_or_fabricated_display_bars(self):
        encoded = json.dumps(DATA)
        self.assertNotIn('Administrator', encoded)
        self.assertNotIn('C:\\', encoded)
        for run in DATA['runs']:
            self.assertEqual({bar['lane'] for bar in run['bars']}, {0, 1, 2, 3, 4})
            for bar in run['bars']:
                self.assertGreater(bar['end'], bar['start'])


if __name__ == '__main__':
    unittest.main()
