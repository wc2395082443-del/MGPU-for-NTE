from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from scheduler_clock_map import ClockMap


def fixture():
    return dict(adapters=[dict(luid="0x1",open_status="0x0",close_status="0x0",samples=[dict(
        status="0x0",cpu_in_call_window=True,node=0,physical_adapter=0,flags=0,qpc_frequency=10000000,
        gpu_frequency=1000000000,cpu_counter=100000000+index*500000,gpu_counter=1000000000000+index*50000000,
        qpc_before=99999990+index*500000,qpc_after=100000010+index*500000,query_us=2)
        for index in range(3)])])


class MapTests(unittest.TestCase):
    def test_interpolates(self): self.assertAlmostEqual(ClockMap(fixture()).project(1,1000025000000),10025)
    def test_no_extrapolation(self): self.assertIsNone(ClockMap(fixture()).project(1,999999999999))
    def test_wrong_adapter(self): self.assertIsNone(ClockMap(fixture()).project(2,1000025000000))
    def test_wrong_node(self): self.assertIsNone(ClockMap(fixture()).project(1,1000025000000,1))
    def test_cached_counter_rejected(self):
        report=fixture();report["adapters"][0]["samples"][1]["cpu_in_call_window"]=False
        with self.assertRaises(ValueError): ClockMap(report)
    def test_large_gap_rejected(self):
        with self.assertRaisesRegex(ValueError,"gap too large"): ClockMap(fixture(),max_gap_ms=20)
    def test_slow_query_rejected(self):
        report=fixture();report["adapters"][0]["samples"][1]["query_us"]=1000
        with self.assertRaises(ValueError): ClockMap(report)
    def test_changed_domain_rejected(self):
        report=fixture();report["adapters"][0]["samples"][1]["flags"]=1
        with self.assertRaises(ValueError): ClockMap(report)
    def test_reversed_counter_rejected(self):
        report=fixture();report["adapters"][0]["samples"][1]["gpu_counter"]-=100000000
        with self.assertRaises(ValueError): ClockMap(report)
    def test_holdout_interpolation_error_rejected(self):
        report=fixture()
        for name in ("cpu_counter","qpc_before","qpc_after"): report["adapters"][0]["samples"][1][name]+=2000
        with self.assertRaisesRegex(ValueError,"holdout interpolation"): ClockMap(report)


if __name__=="__main__": unittest.main()
