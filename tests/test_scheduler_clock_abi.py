import ctypes
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from query_scheduler_clock import abi, ClockData, Luid, OpenAdapter, QueryClock


class ClockAbiTests(unittest.TestCase):
    def test_verified_layout(self):
        self.assertEqual(abi()["QUERYCLOCKCALIBRATION"],40)
        self.assertEqual(ctypes.alignment(QueryClock),1)
        self.assertEqual(QueryClock.data.offset,12)
        self.assertEqual(ClockData.flags.offset,24)

    def test_luid_and_handle_not_packed_with_clock(self):
        self.assertEqual(ctypes.alignment(OpenAdapter),4)
        self.assertEqual(OpenAdapter.handle.offset,8)
        self.assertEqual(ctypes.sizeof(Luid),8)

    def test_output_is_zero_initialized(self):
        request=QueryClock()
        self.assertEqual(bytes(request),bytes(40))


if __name__=="__main__": unittest.main()
