import importlib.util
from pathlib import Path
import unittest


specification = importlib.util.spec_from_file_location("m340_copy", Path(__file__).resolve().parents[1] / "tools/analyze_m340_copy.py")
module = importlib.util.module_from_spec(specification)
specification.loader.exec_module(module)


def tuple_lines(seal=1, offset=0, session=7):
    return [
        f"[M340-COPY] kind=producer session={session} seal={seal} engine_frame={seal+100} "
        f"color_start={offset+10} color_end={offset+14} guides_start={offset+14} guides_end={offset+16}",
        f"[M340-COPY] kind=consumer session={session} seal={seal} "
        f"color_start={offset+14} color_end={offset+15} guides_start={offset+16} guides_end={offset+17}",
        f"[M336] kind=consumer session={session} seal={seal} engine_frame={seal+100} "
        f"neural_start={offset+17} neural_end={offset+32}",
    ]


class CopyAnalysisTest(unittest.TestCase):
    def test_overlap_is_not_sum_of_elapsed_spans(self):
        frame = module.analyze(tuple_lines())["frames"][0]
        self.assertEqual(frame["copy_overlap_ms"], 1)
        self.assertEqual(frame["transfer_elapsed_ms"], 7)
        self.assertEqual(frame["copy_brackets_sum_ms"], 8)
        self.assertEqual(frame["consumer_between_ms"], 1)

    def test_unordered_records_join_by_identity(self):
        result = module.analyze(list(reversed(tuple_lines() + tuple_lines(2, 40))))
        self.assertEqual(result["paired_frames"], 2)
        self.assertEqual(result["summary"]["neural_completion_ms"]["mean"], 40)

    def test_gaps_and_sessions_do_not_create_cadence(self):
        result = module.analyze(tuple_lines() + tuple_lines(3, 80) + tuple_lines(4, 120, 8))
        self.assertNotIn("neural_completion_ms", result["summary"])

    def test_invalid_and_unpaired_timestamps_are_not_zero_cost(self):
        records = tuple_lines()
        records[1] = records[1].replace("color_start=14", "color_start=-1")
        result = module.analyze(records + [tuple_lines(2, 40)[0]])
        self.assertEqual(result["paired_frames"], 0)
        self.assertEqual(len(result["rejected"]), 2)

    def test_duplicate_or_wrong_engine_identity_is_rejected(self):
        with self.assertRaises(ValueError):
            module.analyze(tuple_lines() * 2)
        records = tuple_lines()
        records[2] = records[2].replace("engine_frame=101", "engine_frame=102")
        with self.assertRaises(ValueError):
            module.analyze(records)

    def test_cross_queue_inversion_is_exposed(self):
        records = tuple_lines()
        records[1] = records[1].replace("color_start=14", "color_start=13.99")
        self.assertTrue(module.analyze(records)["frames"][0]["clock_or_gate_inversion"])


if __name__ == "__main__":
    unittest.main()
