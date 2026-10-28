import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1]/"tools/audit_m343_system_events.py"
spec = importlib.util.spec_from_file_location("system_audit", path)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def event(time, opcode, **fields):
    return dict(provider="cpu_thread", qpc_ms=time, opcode=opcode, fields={name:str(value) for name,value in fields.items()})


class SystemEventsTest(unittest.TestCase):
    def test_wait_then_ready_then_run(self):
        result = audit.cpu_intervals([event(10,36,OldThreadId=7,NewThreadId=8,OldThreadState=5,OldThreadWaitReason=6),
            event(13,50,TThreadId=7),event(14,36,OldThreadId=8,NewThreadId=7)],{7})
        self.assertEqual(result["intervals"][0]["off_cpu_ms"],4)
        self.assertEqual(result["intervals"][0]["ready_to_running_ms"],1)
        self.assertIsNone(result["intervals"][0]["gpu_fence_cause"])

    def test_missing_ready_stays_unknown(self):
        result = audit.cpu_intervals([event(10,36,OldThreadId=7,NewThreadId=8),event(14,36,OldThreadId=8,NewThreadId=7)],{7})
        self.assertIsNone(result["intervals"][0]["ready_to_running_ms"])

    def test_window_boundary_not_extrapolated(self):
        result = audit.cpu_intervals([event(14,36,OldThreadId=8,NewThreadId=7)],{7})
        self.assertEqual(result["intervals"],[])

    def test_missing_switch_in_is_unclosed(self):
        result = audit.cpu_intervals([event(10,36,OldThreadId=7,NewThreadId=8)],{7})
        self.assertEqual(len(result["unclosed"]),1)
        self.assertEqual(result["intervals"],[])

    def test_unknown_schema_is_counted(self):
        result = audit.cpu_intervals([event(10,36,Something=7)],{7})
        self.assertEqual(result["unsupported_field_events"],1)

    def test_unobserved_threads_excluded(self):
        result = audit.cpu_intervals([event(10,36,OldThreadId=7,NewThreadId=8),event(14,36,OldThreadId=8,NewThreadId=7)],{9})
        self.assertEqual(result["intervals"],[])

    def test_event_loss_refuses_attribution(self):
        with self.assertRaises(ValueError):
            audit.audit(None,[],dict(status=0,lost=1),{})

    def test_wrong_clock_refuses_attribution(self):
        with self.assertRaises(ValueError):
            audit.audit(None,[],dict(status=0,lost=0,clock_type=2,frequency=1000),dict(header=dict(frequency=1000)))


if __name__ == "__main__":
    unittest.main()
