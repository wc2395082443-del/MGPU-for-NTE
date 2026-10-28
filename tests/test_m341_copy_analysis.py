import importlib.util
from pathlib import Path
import unittest

specification = importlib.util.spec_from_file_location("m341_copy", Path(__file__).resolve().parents[1] / "tools/analyze_m341_copy.py")
module = importlib.util.module_from_spec(specification)
specification.loader.exec_module(module)


def tuple_lines(seal=1, offset=0, session=7):
    return [
        f"[M341-COPY] kind=producer session={session} seal={seal} engine_frame={seal+100} "
        f"color_start={offset+10} color_end={offset+12} depth_start={offset+12} depth_end={offset+14} motion_start={offset+14} motion_end={offset+16}",
        f"[M341-COPY] kind=consumer session={session} seal={seal} engine_frame={seal+100} "
        f"color_start={offset+12} color_end={offset+13} depth_start={offset+14} depth_end={offset+15} motion_start={offset+16} motion_end={offset+17}",
        f"[M336] kind=consumer session={session} seal={seal} engine_frame={seal+100} neural_start={offset+17} neural_end={offset+32}",
    ]


class GuideAnalysisTest(unittest.TestCase):
    def test_depth_overlap_and_total_overlap_are_separate(self):
        frame = module.analyze(tuple_lines())["frames"][0]
        self.assertEqual(frame["depth_receive_motion_send_overlap_ms"], 1)
        self.assertEqual(frame["copy_overlap_ms"], 2)
        self.assertEqual(frame["transfer_elapsed_ms"], 7)
        self.assertEqual(frame["copy_brackets_sum_ms"], 9)
        self.assertEqual(frame["consumer_depth_motion_gap_ms"], 1)

    def test_identity_join_and_consecutive_cadence(self):
        result = module.analyze(list(reversed(tuple_lines() + tuple_lines(2, 40))))
        self.assertEqual(result["paired_frames"], 2)
        self.assertEqual(result["summary"]["neural_completion_ms"]["mean"], 40)

    def test_sessions_and_missing_seals_do_not_create_cadence(self):
        result = module.analyze(tuple_lines() + tuple_lines(3, 80) + tuple_lines(4, 120, 8))
        self.assertNotIn("neural_completion_ms", result["summary"])

    def test_invalid_depth_query_is_not_zero_cost(self):
        records = tuple_lines()
        records[0] = records[0].replace("depth_start=12", "depth_start=-1")
        result = module.analyze(records + [tuple_lines(2)[1]])
        self.assertEqual(result["paired_frames"], 0)
        self.assertEqual(len(result["rejected"]), 2)

    def test_unordered_stages_are_rejected(self):
        records = tuple_lines()
        records[0] = records[0].replace("motion_start=14", "motion_start=13")
        self.assertEqual(module.analyze(records)["paired_frames"], 0)

    def test_duplicates_are_rejected(self):
        with self.assertRaises(ValueError):
            module.analyze(tuple_lines() * 2)

    def test_all_engine_identities_must_match(self):
        for index in (1, 2):
            records = tuple_lines()
            records[index] = records[index].replace("engine_frame=101", "engine_frame=102")
            with self.assertRaises(ValueError):
                module.analyze(records)

    def test_early_depth_is_exposed(self):
        records = tuple_lines()
        records[1] = records[1].replace("depth_start=14", "depth_start=13.99")
        self.assertTrue(module.analyze(records)["frames"][0]["clock_or_gate_inversion"])


if __name__ == "__main__":
    unittest.main()
