import sys
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from analyze_m338_origins import analyze, resolve


class OriginTests(unittest.TestCase):
    def modules(self):
        return [dict(ModuleName="game.exe",FileName="C:/game.exe",BaseAddress="4096",ModuleMemorySize="256")]

    def test_resolved_offset(self):
        row=resolve(4100,self.modules())
        self.assertEqual(row["status"],"resolved")
        self.assertEqual(row["candidates"][0]["offset"],4)

    def test_end_exclusive(self): self.assertEqual(resolve(4352,self.modules())["status"],"unmapped")
    def test_padding(self): self.assertEqual(resolve(0,self.modules())["status"],"padding")
    def test_unmapped(self): self.assertEqual(resolve(4000,self.modules())["status"],"unmapped")
    def test_ambiguous(self): self.assertEqual(resolve(4100,self.modules()*2)["status"],"ambiguous")
    def test_invalid_range(self):
        modules=self.modules();modules[0]["ModuleMemorySize"]="-1"
        with self.assertRaises(ValueError): resolve(4100,modules)
    def test_invalid_capture(self):
        with self.assertRaises(ValueError): analyze(dict(valid=False),self.modules())
    def test_provenance_does_not_identify_sr(self):
        report=analyze(dict(valid=True,origins={"1":{"1":[4100,0]}},job_identities=[dict(session="1",job=1,
            reset_origin=1,submit_origin=0,full_native_boundary=True)]),self.modules())
        self.assertEqual(report["full_native_boundary_jobs"],1)
        self.assertIsNone(report["actual_sr_gpu_ms"])
        self.assertIsNone(report["job_routes"][0]["submit_stack"])
        self.assertEqual(report["job_routes"][0]["reset_stack"][0]["candidates"][0]["module"],"game.exe")


if __name__=="__main__": unittest.main()
