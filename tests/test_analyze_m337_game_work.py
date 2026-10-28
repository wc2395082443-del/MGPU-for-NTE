import unittest
from analyze_m337_game_work import analyze

def segment(**changes):
    row=dict(kind='segment',session='1',job='1',point='0',start_ms='1',end_ms='3',gpu_ms='2',phase='1',overflow='0')
    row.update({key:str(value) for key,value in changes.items()})
    return '[MGPU][M337] '+' '.join(f'{key}={value}' for key,value in row.items())

class StrictTests(unittest.TestCase):
    def test_complete_clean_window(self): self.assertTrue(analyze([segment()])['valid'])
    def test_status_rejection_invalidates(self): self.assertFalse(analyze([segment(),'[MGPU][M337] kind=status session=1 rejected=1 replayed=0 invalid=0 overflow_jobs=0'])['valid'])
    def test_budget_invalidates(self): self.assertIn('job budget exhausted',analyze([segment(),'[MGPU][M337] kind=status session=1 reason=record_budget_exhausted rejected=1 replayed=0 invalid=0 overflow_jobs=0'])['errors'])
    def test_overflow_invalidates(self): self.assertIn('marker overflow',analyze([segment(overflow=1)])['errors'])
    def test_sr_absent_unknown(self): self.assertIsNone(analyze([segment()])['gpu_sr_ms'])
    def test_ngx_not_sr(self): self.assertIsNone(analyze([segment(phase=10)])['gpu_sr_ms'])
    def test_invalid_duration(self): self.assertIn('duration mismatch',analyze([segment(end_ms=4)])['errors'])
    def test_duplicates_invalid(self): self.assertIn('duplicate segment',analyze([segment(),segment()])['errors'])
    def test_unknown_frame_not_invented(self): self.assertIsNone(analyze([segment()])['exclusive_game_render_ms'])
if __name__=='__main__': unittest.main()
