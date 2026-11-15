import unittest

from analyze_fg_gpu_audit import Clock, input_epochs


class AuditTests(unittest.TestCase):
    def test_exact_fence_identity_required(self):
        signal = dict(result=0, is_wait=0, caller_sequence=8, caller_source=100,
                      fence=11, value=3, queue=10, cpu_begin_qpc=200)
        wait = dict(signal, is_wait=1, caller_sequence=0, caller_source=0, queue=20, cpu_begin_qpc=190)
        self.assertEqual(input_epochs([signal, wait])[20][0]['source'], 100)
        self.assertFalse(input_epochs([signal, dict(wait, fence=12)]))
        self.assertFalse(input_epochs([signal, dict(signal, caller_source=101), wait]))

    def test_clock_preserves_large_gpu_integer_offsets(self):
        base = 1794749329750000000
        samples = [dict(gpu=base + index * 1000000, cpu=9000000000000 + index * 10000)
                   for index in range(4)]
        clock = Clock(samples, 10000000)
        self.assertTrue(clock.valid)
        self.assertAlmostEqual(clock.project(base + 1500000), 9000000015000)
        self.assertIsNone(clock.project(base - 1))
        self.assertIsNone(clock.project(base + 3000001))

    def test_bad_clock_or_insufficient_samples_rejected(self):
        self.assertFalse(Clock([dict(gpu=0, cpu=0), dict(gpu=1, cpu=1)], 10000000).valid)
        clock = Clock([dict(gpu=0, cpu=0), dict(gpu=1000000, cpu=90000), dict(gpu=2000000, cpu=20000)], 10000000)
        self.assertFalse(clock.valid)
        self.assertIsNone(clock.project(1000000))

    def test_long_calibration_call_rejected_without_changing_residual_threshold(self):
        samples=[dict(gpu=index*1000000,cpu=index*10000,before=index*10000,after=index*10000+20)
                 for index in range(5)]
        samples[2]=dict(gpu=2000000,cpu=23000,before=20000,after=23020)
        clock=Clock(samples,10000000)
        self.assertTrue(clock.valid)
        self.assertEqual(len(clock.rejected_long_call_samples),1)
        self.assertEqual(len(clock.samples),4)
        self.assertAlmostEqual(clock.project(2000000),20000)


if __name__ == '__main__':
    unittest.main()
