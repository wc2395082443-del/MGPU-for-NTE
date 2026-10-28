import copy
import importlib.util
from pathlib import Path
import unittest

module_path = Path(__file__).resolve().parents[1] / "tools/analyze_m343_queue_trace.py"
spec = importlib.util.spec_from_file_location("queue_decoder", module_path)
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def entry(operation, **values):
    row = dict.fromkeys(decoder.FIELDS, 0)
    row.update(operation=operation, **values)
    return row


def fixture(records=None, **changes):
    metadata = [entry(1, object=1, other=1, raw=100, value=11),
                entry(1, object=2, other=2, raw=200, value=11),
                entry(1, object=3, other=2, raw=300, value=22),
                entry(2, object=2, other=3)]
    if records is None:
        records = [entry(5, object=1, other=2, value=7, begin=100, extra=101, auxiliary=102, end=103, thread=1),
                   entry(6, object=1, other=3, value=7, begin=105, extra=106, auxiliary=107, end=108, thread=1),
                   entry(7, object=2, value=0, begin=110, extra=111, auxiliary=112, end=113, thread=1),
                   entry(9, object=3, value=7, begin=115, extra=116, auxiliary=117, end=118, thread=1),
                   entry(7, object=2, value=7, begin=120, extra=121, auxiliary=122, end=123, thread=1)]
    for index, row in enumerate(records):
        row["sequence"] = index + 1
    header = dict(magic=0x314543415254514d, version=1, record_bytes=96, session=55, frequency=1000,
                  begin=100, end=200, count=len(records), overflow=0, failures=0, metadata=len(metadata), pending=0)
    header.update(changes)
    payload = decoder.HEADER.pack(*(header[name] for name in decoder.HEADER_FIELDS))
    return payload + b"".join(decoder.RECORD.pack(*(row[name] for name in decoder.FIELDS)) for row in metadata+records)


class QueueTraceTest(unittest.TestCase):
    def test_shared_alias_and_real_completion_are_distinct(self):
        result = decoder.decode(fixture())
        self.assertTrue(result["valid"])
        wait = result["dependencies"][0]
        self.assertEqual(wait["possible_signal_sequences"], [2, 4])
        self.assertEqual(wait["sampled_unmet_sequences"], [3])
        self.assertEqual(wait["sampled_met_sequences"], [5])
        self.assertIsNone(wait["gpu_blocked_ms"])
        self.assertFalse(result["kernel_running_verified"])
        self.assertFalse(result["all_gpu_events_visible"])

    def test_signal_return_is_not_completion(self):
        result = decoder.decode(fixture(records=[entry(5, object=1, other=2, value=7, begin=100, extra=101, auxiliary=102, end=103),
            entry(6, object=1, other=2, value=7, begin=104, extra=105, auxiliary=106, end=107)]))
        self.assertEqual(result["dependencies"][0]["sampled_met_sequences"], [])
        self.assertIsNone(result["dependencies"][0]["gpu_blocked_ms"])

    def test_failed_signal_not_a_producer(self):
        rows = [entry(5, object=1, other=2, value=7, begin=100, extra=101, auxiliary=102, end=103),
                entry(9, object=2, value=7, begin=104, extra=105, auxiliary=106, end=107, result=-1)]
        result = decoder.decode(fixture(rows))
        self.assertEqual(result["dependencies"][0]["possible_signal_sequences"], [])

    def test_overflow_hook_failure_and_pending_are_not_complete(self):
        for field in ("overflow", "failures", "pending"):
            self.assertFalse(decoder.decode(fixture(**{field: 1}))["valid"])

    def test_truncation_and_tail_rejected(self):
        for payload in (fixture()[:40], fixture()[:-1], fixture()+b"x"):
            with self.assertRaises(ValueError):
                decoder.decode(payload)

    def test_wrong_abi_rejected(self):
        with self.assertRaises(ValueError):
            decoder.decode(fixture(version=2))

    def test_unknown_object_not_guessed(self):
        with self.assertRaises(ValueError):
            decoder.decode(fixture([entry(7, object=900, begin=100, extra=101, auxiliary=102, end=103)]))

    def test_removed_device_is_not_ready(self):
        result = decoder.decode(fixture([entry(7, object=2, value=2**64-1, begin=100, extra=101, auxiliary=102, end=103)]))
        self.assertFalse(result["valid"])

    def test_concurrent_calls_do_not_establish_queue_order(self):
        rows = [entry(5, object=1, other=2, value=7, begin=100, extra=101, auxiliary=110, end=111, thread=1),
                entry(6, object=1, other=2, value=7, begin=102, extra=103, auxiliary=105, end=106, thread=2)]
        self.assertEqual(len(decoder.decode(fixture(rows))["concurrent_queue_calls"]), 1)

    def test_nested_forwarding_preserved_not_counted_twice(self):
        rows = [entry(5, object=1, other=2, value=7, begin=100, extra=101, auxiliary=110, end=111, thread=1),
                entry(5, object=1, other=2, value=7, begin=102, extra=103, auxiliary=105, end=106, thread=1, parent=1)]
        result = decoder.decode(fixture(rows))
        self.assertEqual(result["counts"]["queue_wait"], 2)
        self.assertEqual(len(result["dependencies"]), 1)

    def test_missing_list_refuses_valid_execute(self):
        with self.assertRaises(ValueError):
            decoder.decode(fixture([entry(3, object=1, value=1, begin=100, extra=101, auxiliary=102, end=103)]))

    def test_batch_job_generation_joins_exactly(self):
        rows = [entry(3, object=1, value=1, begin=100, extra=104, auxiliary=120, end=125, thread=1),
                entry(4, object=1, other=800, value=0, extra=1, begin=101, end=102, parent=1, thread=1),
                entry(10, object=1, other=800, value=17, extra=55, begin=106, end=107, parent=1, thread=1)]
        trace = decoder.decode(fixture(rows))
        compact = dict(valid=True, job_identities=[dict(session="55", job=17, start_ms=130, duration_ticks=5, frequency=1000)])
        joined = decoder.join_jobs(trace, compact)
        self.assertEqual(joined[0]["batch_sequence"], 1)
        gaps = decoder.gap_evidence(trace, joined, [dict(queue=1, start_ms=100, end_ms=110)])
        self.assertEqual(gaps[0]["known_not_submitted_ms"], 4)
        self.assertIsNone(gaps[0]["proven_ready_unscheduled_ms"])

    def test_stale_capture_job_rejected(self):
        with self.assertRaises(ValueError):
            decoder.decode(fixture([entry(10, object=1, other=800, value=17, extra=54, begin=101, end=102)]))

    def test_unparented_job_is_not_assigned_by_proximity(self):
        trace = decoder.decode(fixture([entry(10, object=1, other=800, value=17, extra=55, begin=101, end=102)]))
        compact = dict(valid=True, job_identities=[dict(session="55", job=17, start_ms=130, duration_ticks=5, frequency=1000)])
        self.assertIsNone(decoder.join_jobs(trace, compact)[0]["batch_sequence"])

    def test_missing_submission_does_not_skip_earlier_gpu_work(self):
        trace = decoder.decode(fixture())
        compact = dict(valid=True, job_identities=[dict(session="55",job=17,start_ms=130,duration_ticks=5,frequency=1000)],
                       segments=[dict(session="55",job=17,queue="64")])
        jobs = decoder.join_jobs(trace, compact)
        self.assertEqual(len(jobs),1)
        self.assertEqual(jobs[0]["queue"],1)
        self.assertIsNone(jobs[0]["batch_sequence"])
        evidence = decoder.gap_evidence(trace,jobs,[dict(queue=1,start_ms=120,end_ms=130)])
        self.assertIsNone(evidence[0]["known_not_submitted_ms"])


if __name__ == "__main__":
    unittest.main()
