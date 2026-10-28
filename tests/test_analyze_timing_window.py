import csv
import datetime as dt
import json
from pathlib import Path
import tempfile
import unittest

from analyze_timing_window import summarize, summarize_completion


class TimingWindowTest(unittest.TestCase):
    def capture(self, root, tag="R330-PIPE", seconds=20):
        anchor = dt.datetime.fromisoformat("2026-10-17T17:25:00+08:00")
        (root / "sample-window.json").write_text(
            json.dumps({"start": anchor.isoformat()}), encoding="utf-8-sig")
        with (root / "gpu-bus.csv").open("w", encoding="utf-8-sig", newline="") as stream:
            fields = ["timestamp", "game_foreground", "name", "gpu_util", "power_mw", "tx_kb_s", "rx_kb_s"]
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for offset in range(seconds + 1):
                writer.writerow({"timestamp": (anchor + dt.timedelta(seconds=offset)).isoformat(),
                                 "game_foreground": "True", "name": "test GPU", "gpu_util": 50})
        logs = []
        for offset in (0, 5, 10, 15, 20):
            stamp = anchor + dt.timedelta(seconds=offset)
            logs.append(f"{stamp:%H:%M:%S}:000 [1] | INFO | [MGPU][{tag}] "
                        f"seal={120 + offset * 24} engine_frame={220 + offset * 24} "
                        "gpu_ms=(neural=14.0 output=0.2) completion_wall_ms=16.0 present_cpu_ms=0.4")
        (root / "after.log").write_text("\n".join(logs), encoding="utf-8")

    def test_current_and_previous_versions(self):
        for tag in ("R326-PIPE", "R330-PIPE"):
            with self.subTest(tag=tag), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.capture(root, tag)
                report = summarize(root)
                self.assertEqual(report["completion_cadence"]["tag"], tag)
                self.assertEqual(report["completion_cadence"]["seal_progress_per_s"], 24)
                self.assertEqual(report["completion_cadence"]["engine_frame_progress_per_s"], 24)
                self.assertEqual(report["completion_cadence"]["engine_frames_per_seal"], 1)
                self.assertEqual(report["complete_pipeline_windows_mean"]["neural"], 14)

    def test_under_fifteen_seconds_is_not_a_valid_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.capture(root, seconds=14)
            with self.assertRaisesRegex(ValueError, "under 15 seconds"):
                summarize(root)

    def entry(self, offset, seal, tag="R330-PIPE", extra=""):
        return {"time": dt.datetime(2026, 10, 17) + dt.timedelta(seconds=offset),
                "tag": tag, "body": f"seal={seal} {extra}"}

    def test_missing_optional_metrics_are_not_zero(self):
        cadence, metrics = summarize_completion([self.entry(0, 1), self.entry(5, 121)])
        self.assertEqual(cadence["seal_progress_per_s"], 24)
        self.assertIsNone(cadence["engine_frame_progress_per_s"])
        self.assertIsNone(metrics["neural"])

    def test_mixed_versions_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Mixed pipeline"):
            summarize_completion([self.entry(0, 1, "R326-PIPE"), self.entry(5, 121)])

    def test_reset_duplicate_and_nonmonotonic_time_are_rejected(self):
        for offset, seal in ((5, 1), (5, 0), (0, 121), (-1, 121)):
            with self.subTest(offset=offset, seal=seal), self.assertRaises(ValueError):
                summarize_completion([self.entry(0, 1), self.entry(offset, seal)])

    def test_engine_gaps_are_exposed_separately(self):
        cadence, _ = summarize_completion([
            self.entry(0, 1, extra="engine_frame=1"), self.entry(5, 121, extra="engine_frame=241")])
        self.assertEqual(cadence["engine_frames_per_seal"], 2)
        self.assertEqual(cadence["engine_frame_progress_per_s"], 48)

    def test_engine_reset_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Engine frame"):
            summarize_completion([
                self.entry(0, 1, extra="engine_frame=100"), self.entry(5, 121, extra="engine_frame=1")])

    def test_insufficient_completion_samples(self):
        for entries in ([], [self.entry(0, 1)]):
            cadence, metrics = summarize_completion(entries)
            self.assertIsNone(cadence)
            self.assertIsNone(metrics["neural"])


if __name__ == "__main__":
    unittest.main()
