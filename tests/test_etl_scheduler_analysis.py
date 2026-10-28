import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_etl_scheduler import audit_log_clock, clipped, decode_log, milliseconds, rotate_full_scheduler_rings, select_contexts, state_intervals, union, without


def fields(entries):
    payload = b"".join(struct.pack("<QQQQ", *entry) for entry in entries)
    return {"SchedulingLogData": [payload.hex()], "SchedulingLogSize": [str(len(payload))],
            "CpuTimestamp0": ["10000000"], "CpuTimestamp1": ["10010000"],
            "GpuTimestamp0": ["1792446138481207744"],
            "GpuTimestamp1": ["1792446138482207744"]}


class SchedulerTests(unittest.TestCase):
    def test_future_scheduler_history_is_rejected(self):
        calibration=fields([(1792446138481207744,0,42,1)])
        calibration["hAdapter"]=["a"]
        result=audit_log_clock(calibration,[dict(qpc_ms=1010)],1000)
        self.assertFalse(result["coarse_causality_pass"])
        self.assertEqual(result["maximum_future_ms"],10)

    def test_past_history_not_a_precision_certificate(self):
        calibration=fields([(1792446138481207744,0,42,1)])
        calibration["hAdapter"]=["a"]
        result=audit_log_clock(calibration,[dict(qpc_ms=1000)],1100)
        self.assertTrue(result["coarse_causality_pass"])
        self.assertFalse(result["precision_validated"])

    def ring_rows(self, stamps):
        return [dict(adapter="a",node=0,engine=0,gpu_timestamp=stamp,qpc_ms=stamp,calibration_identity=[1,2,3,4]) for stamp in stamps]

    def test_single_complete_ring_rotates_without_sorting(self):
        rows,rotations=rotate_full_scheduler_rings(self.ring_rows([3,4,1,2]),capacity=4)
        self.assertEqual([row["gpu_timestamp"] for row in rows],[1,2,3,4])
        self.assertEqual(rotations[0]["pivot"],2)

    def test_partial_ring_not_reordered(self):
        with self.assertRaises(ValueError): rotate_full_scheduler_rings(self.ring_rows([3,1,2]),capacity=4)

    def test_multiple_wraps_not_sorted_away(self):
        with self.assertRaises(ValueError): rotate_full_scheduler_rings(self.ring_rows([4,1,3,2]),capacity=4)

    def test_mixed_calibration_not_reordered(self):
        rows=self.ring_rows([3,4,1,2]);rows[-1]["calibration_identity"][0]=9
        with self.assertRaises(ValueError): rotate_full_scheduler_rings(rows,capacity=4)

    def test_noncyclic_disorder_rejected(self):
        with self.assertRaises(ValueError): rotate_full_scheduler_rings(self.ring_rows([2,4,1,3]),capacity=4)

    def test_explicit_adapter_and_process_selection(self):
        contexts={"game":dict(pid=9,adapter="a"),"neural":dict(pid=9,adapter="b"),"desktop":dict(pid=1,adapter="a")}
        selected,excluded=select_contexts(contexts,{"a":dict(luid=100),"b":dict(luid=200)},9,100)
        self.assertEqual(set(selected),{"game"})
        self.assertEqual(excluded["neural"]["exclusion_reason"],"different_adapter")
        self.assertEqual(excluded["desktop"]["exclusion_reason"],"different_process")

    def test_unknown_context_ownership_not_filtered_away(self):
        with self.assertRaises(ValueError):
            select_contexts({"unknown":dict(adapter="a")},{"a":dict(luid=100)},9,100)

    def test_unknown_game_adapter_not_filtered_away(self):
        with self.assertRaises(ValueError):
            select_contexts({"game":dict(pid=9,adapter="a")},{},9,100)

    def test_empty_requested_adapter_rejected(self):
        with self.assertRaises(ValueError):
            select_contexts({"game":dict(pid=9,adapter="a")},{"a":dict(luid=200)},9,100)

    def test_union_does_not_double_count(self):
        self.assertEqual(union([(0, 4), (2, 6), (6, 8)]), [[0, 8]])

    def test_clipped_execution(self):
        self.assertEqual(clipped([(0, 4), (7, 12)], 2, 9), [[2, 4], [7, 9]])
        self.assertEqual(milliseconds(clipped([(0, 4), (7, 12)], 2, 9)), 4)

    def test_negative_interval_is_rejected(self):
        with self.assertRaises(ValueError):
            union([(5, 3)])

    def test_non_running_is_complement_not_fence_wait(self):
        self.assertEqual(without([(1, 3), (4, 8)], 0, 6), [[0, 1], [3, 4]])

    def test_missing_execution_is_not_extrapolated(self):
        self.assertEqual(without([], 2, 5), [[2, 5]])

    def test_calibration_retains_large_timestamp_precision(self):
        rows = decode_log(fields([(1792446138481207745, 0, 42, 1)]))
        self.assertAlmostEqual(rows[0]["qpc_ms"], 1000.000001, places=9)

    def test_binary_size_is_validated(self):
        data = fields([(1792446138481207744, 0, 42, 1)])
        data["SchedulingLogSize"] = ["31"]
        with self.assertRaises(ValueError):
            decode_log(data)

    def test_unknown_operation_is_not_running(self):
        with self.assertRaises(ValueError):
            decode_log(fields([(1792446138481207744, 1, 42, 1)]))

    def test_unknown_state_is_rejected(self):
        with self.assertRaises(ValueError):
            decode_log(fields([(1792446138481207744, 0, 42, 4)]))

    def test_no_state_extrapolation(self):
        self.assertEqual(state_intervals([{"context": "a", "qpc_ms": 5, "state": 1}]), [])

    def test_running_begins_on_running_transition(self):
        rows = state_intervals([{"context": "a", "qpc_ms": 0, "state": 0},
                                {"context": "a", "qpc_ms": 3, "state": 1},
                                {"context": "a", "qpc_ms": 8, "state": 0}])
        self.assertEqual(rows[0]["state"], 0)
        self.assertEqual(rows[1], {"context": "a", "state": 1, "start_ms": 3, "end_ms": 8})

    def test_context_interleaving(self):
        rows = state_intervals([{"context": "a", "qpc_ms": 1, "state": 1},
                                {"context": "b", "qpc_ms": 2, "state": 1},
                                {"context": "a", "qpc_ms": 3, "state": 0},
                                {"context": "b", "qpc_ms": 4, "state": 0}])
        self.assertEqual(milliseconds([(row["start_ms"], row["end_ms"]) for row in rows]), 3)

    def test_out_of_order_states_are_rejected(self):
        with self.assertRaises(ValueError):
            state_intervals([{"context": "a", "qpc_ms": 3, "state": 1},
                             {"context": "a", "qpc_ms": 1, "state": 0}])


if __name__ == "__main__":
    unittest.main()
