import unittest
from analyze_output_trace import STAGES, NAMES, analyze, select_window, reflex_intervals


class OutputAnalysisTest(unittest.TestCase):
    def frame(self, sequence, base, visible=1):
        result = dict(sequence=sequence, source=sequence + 10, capture_ns=1_000_000,
                      visible=visible, requested=6, backlog=1, qpc_frequency=1000, input_poll_wait_ticks=10)
        result.update({name: base + index for index, name in enumerate(STAGES)})
        return result

    def test_partition_and_gap(self):
        result = analyze([self.frame(1, 100), self.frame(2, 130)])
        self.assertEqual(result["frames"], 2)
        self.assertEqual(result["summary"]["host_frame_ms"]["mean"], 10)
        self.assertEqual(sum(result["summary"][name + "_ms"]["mean"] for name in NAMES), 10)
        self.assertEqual(result["summary"]["host_gap_ms"]["mean"], 20)
        self.assertEqual(result["summary"]["nonpoll_gap_ms"]["mean"], 10)

    def test_visibility_or_sequence_gap_not_bridged(self):
        result = analyze([self.frame(1, 100), self.frame(2, 130, 0), self.frame(3, 160), self.frame(7, 190)])
        self.assertEqual(result["summary"]["acquisition_interval_ms"]["count"], 0)

    def test_corrupt_rejected(self):
        bad = self.frame(2, 130)
        bad["present_returned"] = 1
        result = analyze([self.frame(1, 100), bad, self.frame(3, 160)])
        self.assertEqual(result["rejected"], 1)
        self.assertEqual(result["summary"]["acquisition_interval_ms"]["count"], 0)

    def test_only_complete_overlapping_frames(self):
        rows = [self.frame(1, 100), self.frame(2, 130), self.frame(3, 160)]
        selected = select_window(rows, 110, 165, 1000)
        self.assertEqual([row["sequence"] for row in selected], [2])

    def test_nonoverlap_is_empty_not_old_data(self):
        self.assertEqual(select_window([self.frame(1, 100)], 200, 300, 1000), [])

    def test_incompatible_clock_rejected(self):
        with self.assertRaises(ValueError):
            select_window([self.frame(1, 100)], 100, 200, 10000000)

    def test_native_sleep_and_helper_sleep_are_separate(self):
        values = reflex_intervals(dict(sleep_native_begin_ns=1_000_000, sleep_native_end_ns=3_000_000,
            sleep_queued_ns=3_100_000, sleep_dispatch_begin_ns=3_400_000, sleep_dispatch_end_ns=8_400_000,
            sim_start_native_begin_ns=8_700_000))
        self.assertEqual(values["native_sleep_ms"], 2)
        self.assertEqual(values["helper_sleep_ms"], 5)
        self.assertAlmostEqual(values["sleep_relay_queue_ms"], .3)
        self.assertAlmostEqual(values["sleep_ack_to_simulation_ms"], .3)

    def test_marker_replay_can_collapse_actual_simulation(self):
        values = reflex_intervals(dict(sim_start_native_begin_ns=10_000_000, sim_end_native_begin_ns=14_000_000,
            sim_start_dispatch_begin_ns=15_000_000, sim_end_dispatch_begin_ns=15_100_000))
        self.assertEqual(values["source_simulation_ms"], 4)
        self.assertAlmostEqual(values["replayed_simulation_ms"], .1)
        self.assertEqual(values["sim_start_delivery_ms"], 5)

    def test_absent_timestamps_are_not_zero_wait_measurements(self):
        result = analyze([self.frame(1, 100)])
        self.assertEqual(result["summary"]["native_sleep_ms"]["count"], 0)
        self.assertEqual(reflex_intervals(dict(sleep_native_begin_ns=0, sleep_native_end_ns=50)), {})

    def test_invalid_reflex_timestamp_order_rejected(self):
        with self.assertRaises(ValueError):
            reflex_intervals(dict(sleep_native_begin_ns=200, sleep_native_end_ns=100))


if __name__ == "__main__":
    unittest.main()
