import unittest
from analyze_m337b_jobs import analyze

def fixture():
    return ["[M337] kind=job session=1 job=1 queue=a type=0 engine_frame=9 frame_known=1 overflow=0 start_ms=100 frequency=1000000 marks=3 data=1:2560:1440:0,8:0:0:3e8,12:0:0:bb8",
            "[M337] kind=status session=1 reason=submitted_jobs_drained retired=1 rejected=0 invalid=0",
            "[M337] kind=aggregate session=1 jobs=1"]

class CompactTests(unittest.TestCase):
    def test_valid(self):
        row=analyze(fixture());self.assertTrue(row["valid"]);self.assertEqual(row["gpu_sr_ms"],2)
    def test_log_loss(self):
        self.assertFalse(analyze(fixture()+["log queue: 10240 line(s) dropped so far"])['valid'])
    def test_missing_final(self): self.assertFalse(analyze(fixture()[:1])['valid'])
    def test_missing_job(self): self.assertFalse(analyze(fixture()[1:])['valid'])
    def test_missing_aggregate(self): self.assertFalse(analyze(fixture()[:2])['valid'])
    def test_duplicate(self): self.assertFalse(analyze(fixture()+fixture()[:1])['valid'])
    def test_overflow(self): self.assertFalse(analyze([s.replace('overflow=0','overflow=1') for s in fixture()])['valid'])
    def test_unknown_frame(self):
        report=analyze([s.replace('frame_known=1','frame_known=0') for s in fixture()]);self.assertIsNone(report['segments'][0]['engine_frame'])
    def test_encoding_failure(self): self.assertFalse(analyze([s.replace('invalid=0','invalid=0 encoding_errors=1') for s in fixture()])['valid'])
    def test_nonmonotonic(self): self.assertFalse(analyze([s.replace(':bb8',':1') for s in fixture()])['valid'])
    def test_incomplete_tail(self): self.assertFalse(analyze([s.replace('12:0:0','1:0:0') for s in fixture()])['valid'])
    def test_invalid_sr_stays_null(self): self.assertIsNone(analyze(fixture()+['log queue: 1 line(s) dropped'])['gpu_sr_ms'])
    def test_known_ngx_sr_identity(self):
        rows=[line.replace('data=','ngx_handle=99 ngx_feature=1 ngx_known=1 data=').replace(',8:0:0:',',10:0:0:') for line in fixture()]
        self.assertEqual(analyze(rows)['gpu_sr_ms'],2)
    def test_known_non_sr_not_called_sr(self):
        rows=[line.replace('data=','ngx_handle=99 ngx_feature=11 ngx_known=1 data=').replace(',8:0:0:',',10:0:0:') for line in fixture()]
        self.assertIsNone(analyze(rows)['gpu_sr_ms'])
        self.assertEqual(analyze(rows)['ngx_feature_interval_ms']['11'],2)
    def test_unknown_handle_not_guessed(self):
        rows=[line.replace('data=','ngx_handle=99 ngx_feature=1 ngx_known=0 data=').replace(',8:0:0:',',10:0:0:') for line in fixture()]
        self.assertIsNone(analyze(rows)['gpu_sr_ms'])
    def test_conflicting_handles_not_guessed(self):
        rows=[line.replace('data=','ngx_handle=99 ngx_feature=1 ngx_known=2 data=').replace(',8:0:0:',',10:0:0:') for line in fixture()]
        self.assertIsNone(analyze(rows)['gpu_sr_ms'])
    def test_native_boundary_and_distinct_provenance(self):
        rows=fixture()
        rows[0]=rows[0].replace('data=','native_start=1 native_end=1 reset_origin=1 submit_origin=2 command=abcd data=')
        rows.extend('[M337] kind=origin session=1 origin=%d pcs=10,20,0,0,0,0,0,0'%index for index in (1,2))
        report=analyze(rows)
        self.assertTrue(report['valid'])
        self.assertTrue(report['job_identities'][0]['full_native_boundary'])
        self.assertEqual(report['job_identities'][0]['submit_origin'],2)
        self.assertEqual(report['origins']['1'][1][:2],[16,32])
    def test_old_job_boundary_is_unknown(self):
        self.assertIsNone(analyze(fixture())['job_identities'][0]['native_start'])
    def fragments(self):
        rows=fixture();line=rows.pop(0);split=len(line)//2
        return rows+['[M337] kind=job_part session=1 job=1 part=%d parts=2 payload=%s'%(index,payload)
                     for index,payload in enumerate((line[:split],line[split:]))]
    def test_fragments_reassemble(self): self.assertTrue(analyze(self.fragments())['valid'])
    def test_missing_fragment(self): self.assertFalse(analyze(self.fragments()[:-1])['valid'])
    def test_duplicate_fragment(self): self.assertFalse(analyze(self.fragments()+self.fragments()[-1:])['valid'])
    def test_conflicting_fragment_count(self):
        rows=self.fragments();rows[-1]=rows[-1].replace('parts=2','parts=3')
        self.assertFalse(analyze(rows)['valid'])
    def test_fragment_identity_mismatch(self):
        rows=[line.replace('kind=job_part session=1 job=1','kind=job_part session=1 job=2') for line in self.fragments()]
        self.assertFalse(analyze(rows)['valid'])
    def test_native_partial_not_full(self):
        rows=fixture();rows[0]=rows[0].replace('data=','native_start=0 native_end=1 data=')
        self.assertFalse(analyze(rows)['job_identities'][0]['full_native_boundary'])
    def test_origin_duplicate_rejected(self):
        line='[M337] kind=origin session=1 origin=1 pcs=1,2,0,0,0,0,0,0'
        self.assertFalse(analyze(fixture()+[line,line])['valid'])
    def test_missing_origin_rejected(self):
        rows=fixture();rows[0]=rows[0].replace('data=','reset_origin=1 data=')
        self.assertFalse(analyze(rows)['valid'])
    def test_native_unsubmitted_pool_not_executed(self):
        rows=fixture();rows[1]+=' native_boundary_mode=1 recording=2 closed_unsubmitted=3'
        self.assertTrue(analyze(rows)['valid'])
    def test_pending_submitted_native_still_rejected(self):
        rows=fixture();rows[1]+=' native_boundary_mode=1 submitted_pending=1'
        self.assertFalse(analyze(rows)['valid'])
    def test_pending_semantic_scope_still_rejected(self):
        rows=fixture();rows[1]+=' native_boundary_mode=1 scope_pending=1'
        self.assertFalse(analyze(rows)['valid'])

if __name__=='__main__':unittest.main()
