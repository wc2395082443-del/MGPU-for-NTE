from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from analyze_etl_scheduler import analyze
from scheduler_clock_map import ClockMap
from test_scheduler_clock_map import fixture


def events():
    def event(identity,fields): return dict(provider="dxg",id=identity,fields=fields,qpc_ms=10120,pid=9,tid=1)
    payload=b"".join(struct.pack("<QQQQ",stamp,0,42,state) for stamp,state in
                     ((1000000000000,0),(1000025000000,1),(1000075000000,0)))
    return [event(110,dict(pDxgAdapter=["a"],AdapterLuid=["1"],VendorID=["4318"],DeviceID=["1"])),
            event(29,dict(hDevice=["dev"],hProcessId=["9"],pDxgAdapter=["a"],hThunkHandle=["1"])),
            event(32,dict(hContext=["ctx"],hDevice=["dev"],NodeOrdinal=["0"])),
            event(433,dict(hOsHandle=["0x2a"],pDxgObject=["ctx"])),
            event(432,dict(hAdapter=["a"],NodeOrdinal=["0"],EngineOrdinal=["0"],
                           CpuTimestamp0=["200000000"],CpuTimestamp1=["200010000"],
                           GpuTimestamp0=["1000000000000"],GpuTimestamp1=["1000001000000"],
                           SchedulingLogData=[payload.hex()],SchedulingLogSize=[str(len(payload))]))]


class IntegrationTests(unittest.TestCase):
    def test_cached_future_clock_still_rejected(self):
        with self.assertRaisesRegex(ValueError,"alignment rejected"):
            analyze(events(),{},0,0,9,adapter_luid=1)

    def test_explicit_fresh_clock_replaces_only_projection(self):
        result=analyze(events(),{},0,0,9,adapter_luid=1,external_clock=ClockMap(fixture()))
        self.assertEqual(result["scheduler_clock_source"],"external_fresh_kmt")
        self.assertEqual(result["state_intervals"][1]["end_ms"],10075)
        self.assertEqual(result["scheduler_entry_count"],3)
        self.assertLess(result["projected_clock_max_future_ms"],0)

    def test_wrong_luid_cannot_supply_clock(self):
        report=fixture();report["adapters"][0]["luid"]="0x2"
        with self.assertRaisesRegex(ValueError,"No scheduler entries"):
            analyze(events(),{},0,0,9,adapter_luid=1,external_clock=ClockMap(report))

    def test_fresh_projection_has_its_own_causality_check(self):
        report=fixture()
        for row in report["adapters"][0]["samples"]:
            for name in ("cpu_counter","qpc_before","qpc_after"): row[name]+=100000000
        with self.assertRaisesRegex(ValueError,"after ETW emission"):
            analyze(events(),{},0,0,9,adapter_luid=1,external_clock=ClockMap(report))


if __name__=="__main__": unittest.main()
