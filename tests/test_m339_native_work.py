import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("native_work_analysis",ROOT/"tools/analyze_m339_native_work.py")
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture():
    return [
        "[MGPU][M337] kind=origin session=1 origin=1 pcs=1,2,3,4,5,6,7,8",
        "[MGPU][M339] kind=coverage session=1 mask=8191 extended=1 failures=0",
        "[MGPU][M339] kind=label session=1 label=1 object=ab encoding=2 bytes=6 data=500053004f00",
        "[MGPU][M337] kind=job session=1 job=1 queue=ABC type=0 engine_frame=2 frame_known=1 viewport=0 overflow=0 start_ms=100 frequency=1000 marks=4 reset_origin=1 submit_origin=1 native_start=1 native_end=1 fine_selected=1 data=0:0:0:0,2:0:0:1,0:0:0:4,12:0:0:5",
        "[MGPU][M339] kind=fine session=1 job=1 count=1 truncated=0 mixed=0 data=1:2:3:1:ab:0:1:1:1:1:0",
        "[MGPU][M337] kind=status session=1 reason=submitted_jobs_drained retired=1 native_boundary_mode=1",
        "[MGPU][M337] kind=aggregate session=1 queue=ABC jobs=1",
    ]


class NativeWorkTests(unittest.TestCase):
    def test_pair_and_unknown_remainder(self):
        result=module.analyze(fixture())
        self.assertTrue(result["valid"],result["errors"])
        job=result["fine_jobs"][0]
        self.assertEqual(job["operations"][0]["bracket_ms"],3)
        self.assertEqual(job["operations"][0]["pipeline_label_text"],"PSO")
        self.assertEqual(job["unclassified_remainder_ms"],2)
        self.assertIsNone(result["actual_sr_ms"])

    def test_missing_fine(self):
        self.assertIn("missing fine job",module.analyze([line for line in fixture() if "kind=fine " not in line])["errors"])

    def test_duplicate_fine(self):
        lines=fixture()
        self.assertIn("duplicate fine job",module.analyze(lines+[lines[4]])["errors"])

    def test_pair_out_of_range(self):
        lines=fixture(); lines[4]=lines[4].replace("data=1:2:","data=1:8:")
        self.assertFalse(module.analyze(lines)["valid"])

    def test_overlap_rejected(self):
        lines=fixture(); lines[4]=lines[4].replace("count=1","count=2")+",1:2:3:1:ab:0:1:1:1:1:0"
        self.assertFalse(module.analyze(lines)["valid"])

    def test_missing_origin(self):
        lines=fixture(); lines[4]=lines[4].replace("1:2:3:1:","1:2:3:2:")
        self.assertIn("missing operation origin",module.analyze(lines)["errors"])

    def test_count_mismatch(self):
        lines=fixture(); lines[4]=lines[4].replace("count=1","count=2")
        self.assertIn("fine count mismatch",module.analyze(lines)["errors"])

    def test_truncation_is_not_complete(self):
        lines=fixture(); lines[4]=lines[4].replace("truncated=0","truncated=1")
        result=module.analyze(lines)
        self.assertTrue(result["valid"],result["errors"])
        self.assertFalse(result["fine_jobs"][0]["complete_probe"])

    def test_mixed_is_not_complete(self):
        lines=fixture(); lines[4]=lines[4].replace("mixed=0","mixed=1")
        self.assertFalse(module.analyze(lines)["fine_jobs"][0]["complete_probe"])

    def test_label_pointer_mismatch(self):
        lines=fixture(); lines[2]=lines[2].replace("object=ab","object=ac")
        self.assertIn("missing or mismatched label",module.analyze(lines)["errors"])

    def test_fragmented_fine(self):
        lines=fixture(); fine=lines.pop(4)
        lines.extend(f"[MGPU][M339] kind=fine_part session=1 job=1 part={index} parts=2 payload={value}"
            for index,value in enumerate((fine[:60],fine[60:])))
        self.assertTrue(module.analyze(lines)["valid"])
        self.assertIn("missing fine fragment",module.analyze(lines[:-1])["errors"])

    def test_fragment_identity(self):
        lines=fixture(); fine=lines.pop(4)
        lines.extend(f"[MGPU][M339] kind=fine_part session=1 job=2 part={index} parts=2 payload={value}"
            for index,value in enumerate((fine[:60],fine[60:])))
        self.assertIn("fine fragment identity mismatch",module.analyze(lines)["errors"])

    def test_hook_failure(self):
        lines=fixture(); lines[1]=lines[1].replace("failures=0","failures=1")
        self.assertIn("native hook coverage failure",module.analyze(lines)["errors"])

    def test_base_failure_is_not_ignored(self):
        lines=fixture(); lines[3]=lines[3].replace("overflow=0","overflow=1")
        self.assertFalse(module.analyze(lines)["valid"])

    def test_reserved_meta_budget(self):
        lines=fixture(); lines[5]+=" fine_selected=24 fine_general=8 fine_meta=16 fine_selection_refused=0"
        result=module.analyze(lines)
        self.assertTrue(result["valid"],result["errors"])
        self.assertEqual(result["detail_budgets"][0]["meta"],16)
        lines[5]=lines[5].replace("fine_general=8","fine_general=24")
        self.assertIn("invalid detail budget",module.analyze(lines)["errors"])

    def test_budget_missing_field(self):
        lines=fixture(); lines[5]+=" fine_selected=24"
        self.assertIn("invalid detail budget",module.analyze(lines)["errors"])

    def candidate_fixture(self):
        lines=fixture()
        lines[3]=lines[3].replace("fine_selected=1","fine_selected=1 fine_candidate=1")
        lines.append("[MGPU][M339] kind=seen session=1 job=1 total=1 budget_skipped=0 scope_skipped=0 mixed=0 truncated=0 cutoff=0 counts=0,0,1,0,0,0,0,0,0,0,0,0,0")
        return lines

    def test_expanded_budget(self):
        lines=fixture(); lines[5]+=" fine_selected=1040 fine_general=1024 fine_meta=16 fine_selection_refused=0 fine_general_limit=1024 fine_meta_limit=16 fine_job_limit=1040"
        self.assertTrue(module.analyze(lines)["valid"])
        lines[5]=lines[5].replace("fine_general=1024","fine_general=1025")
        self.assertIn("invalid detail budget",module.analyze(lines)["errors"])

    def test_candidate_observation(self):
        result=module.analyze(self.candidate_fixture())
        self.assertTrue(result["valid"],result["errors"])
        self.assertEqual(result["observed_jobs"][0]["operation_counts"]["dispatch"],1)

    def test_missing_candidate_observation(self):
        self.assertIn("missing observed job",module.analyze(self.candidate_fixture()[:-1])["errors"])

    def test_counter_sum_rejected(self):
        lines=self.candidate_fixture(); lines[-1]=lines[-1].replace("total=1","total=2")
        self.assertIn("invalid observed operation counts",module.analyze(lines)["errors"])

    def test_counter_duplicates_rejected(self):
        lines=self.candidate_fixture()
        self.assertIn("duplicate observed job",module.analyze(lines+[lines[-1]])["errors"])

    def test_zero_calls_not_budget_skip(self):
        lines=self.candidate_fixture()
        lines[3]=lines[3].replace("fine_selected=1","fine_selected=0")
        lines.pop(4)
        lines[-1]=lines[-1].replace("total=1","total=0").replace("counts=0,0,1,","counts=0,0,0,")
        result=module.analyze(lines)
        self.assertTrue(result["valid"],result["errors"])
        self.assertEqual(result["observed_jobs"][0]["total"],0)
        self.assertTrue(result["observed_jobs"][0]["complete_hook_observation"])

    def test_skips_visible_without_fine_queries(self):
        lines=self.candidate_fixture()
        lines[3]=lines[3].replace("fine_selected=1","fine_selected=0")
        lines.pop(4)
        lines[-1]=lines[-1].replace("budget_skipped=0","budget_skipped=1")
        result=module.analyze(lines)
        self.assertTrue(result["valid"],result["errors"])
        self.assertEqual(result["observed_jobs"][0]["budget_skipped"],1)

    def test_cutoff_is_not_complete(self):
        lines=self.candidate_fixture(); lines[-1]=lines[-1].replace("cutoff=0","cutoff=1")
        self.assertFalse(module.analyze(lines)["observed_jobs"][0]["complete_hook_observation"])

    def expanded_operations(self):
        lines=fixture()
        points=["0:0:0:0"]
        operations=[]
        for index in range(160):
            begin=2*index+1; end=begin+1
            points.extend((f"2:0:0:{begin:x}",f"0:0:0:{end:x}"))
            operations.append(f"{begin}:{end}:3:1:ab:0:1:1:1:1:0")
        points.append("12:0:0:141")
        base=lines[3].replace("marks=4","marks=322").replace("fine_selected=1","fine_selected=1 fine_candidate=1").split(" data=",1)[0]+" data="+",".join(points)
        fine=lines[4].replace("count=1","count=160").split(" data=",1)[0]+" data="+",".join(operations)
        lines=lines[:3]+lines[5:]
        for marker,kind,text in (("M337","job",base),("M339","fine",fine)):
            parts=(len(text)+1299)//1300
            lines.extend(f"[MGPU][{marker}] kind={kind}_part session=1 job=1 part={part} parts={parts} payload={text[part*1300:(part+1)*1300]}" for part in range(parts))
        lines[3]+=" fine_selected=1 fine_general=1 fine_meta=0 fine_selection_refused=0 fine_general_limit=1024 fine_meta_limit=16 fine_job_limit=1040 fine_operation_limit=160"
        lines.append("[MGPU][M339] kind=seen session=1 job=1 total=160 budget_skipped=0 scope_skipped=0 mixed=0 truncated=0 cutoff=0 counts=0,0,160,0,0,0,0,0,0,0,0,0,0")
        return lines

    def test_160_operation_fragments(self):
        result=module.analyze(self.expanded_operations())
        self.assertTrue(result["valid"],result["errors"])
        self.assertEqual(len(result["fine_jobs"][0]["operations"]),160)

    def test_operation_limit_is_not_silently_raised(self):
        lines=[line.replace("fine_operation_limit=160","fine_operation_limit=16") for line in self.expanded_operations()]
        self.assertIn("fine count exceeds declared operation limit",module.analyze(lines)["errors"])

    def test_large_fragment_loss_invalidates(self):
        lines=self.expanded_operations()
        lines.remove(next(line for line in lines if "kind=fine_part" in line and " part=2 " in line))
        self.assertFalse(module.analyze(lines)["valid"])

    def independent_fixture(self):
        lines=self.candidate_fixture()
        lines[4]=lines[4].replace(" data="," timing=queries mixed_continues=1 data=").replace("data=1:2:","data=1:4:")
        lines[5]+=" fine_selected=1 fine_general=1 fine_meta=0 fine_selection_refused=0 fine_general_limit=1024 fine_meta_limit=16 fine_job_limit=1040 fine_operation_limit=160 fine_mixed_continues=1"
        lines[-1]=lines[-1].replace(" counts="," mixed_continues=1 counts=")
        return lines

    def test_independent_queries(self):
        result=module.analyze(self.independent_fixture())
        self.assertTrue(result["valid"],result["errors"])
        operation=result["fine_jobs"][0]["operations"][0]
        self.assertEqual(operation["bracket_ms"],3)
        self.assertEqual(operation["begin_ticks"],1)
        self.assertIsNone(operation["begin_index"])

    def test_mixed_continues_recording(self):
        lines=[line.replace("mixed=0","mixed=1") for line in self.independent_fixture()]
        result=module.analyze(lines)
        self.assertTrue(result["valid"],result["errors"])
        self.assertTrue(result["fine_jobs"][0]["complete_probe"])

    def test_independent_queries_cannot_escape_native_bounds(self):
        lines=self.independent_fixture(); lines[4]=lines[4].replace("data=1:4:","data=1:6:")
        self.assertIn("invalid or overlapping independent query pair",module.analyze(lines)["errors"])

    def test_semantic_scope_skip_is_incomplete(self):
        lines=self.independent_fixture(); lines[-1]=lines[-1].replace("scope_skipped=0","scope_skipped=1")
        self.assertFalse(module.analyze(lines)["fine_jobs"][0]["complete_probe"])

    def callback_fixture(self):
        lines=self.independent_fixture()
        lines[3]=lines[3].replace("fine_candidate=1","fine_candidate=1 callback_record=1")
        lines.append("[MGPU][M339] kind=callbacks session=1 job=1 total=3 counts=2,1,0,0 origins=1,1,0,0 points=1,2,4294967295,4294967295")
        return lines

    def test_callback_identities(self):
        result=module.analyze(self.callback_fixture())
        self.assertTrue(result["valid"],result["errors"])
        self.assertEqual(result["callback_jobs"][0]["callbacks"][0]["kind"],"acceleration_build")
        self.assertEqual(result["callback_jobs"][0]["callbacks"][0]["count"],2)

    def test_missing_callback_is_invalid(self):
        self.assertIn("missing callback record",module.analyze(self.callback_fixture()[:-1])["errors"])

    def test_bad_callback_point(self):
        lines=self.callback_fixture(); lines[-1]=lines[-1].replace("points=1,2,","points=1,8,")
        self.assertIn("invalid callback point",module.analyze(lines)["errors"])

    def test_new_clear_operation_kind(self):
        lines=self.independent_fixture(); lines[4]=lines[4].replace("1:4:3:","1:4:14:")
        result=module.analyze(lines)
        self.assertTrue(result["valid"],result["errors"])
        self.assertEqual(result["fine_jobs"][0]["operations"][0]["kind"],"uav_clear_uint")


if __name__=="__main__": unittest.main()
