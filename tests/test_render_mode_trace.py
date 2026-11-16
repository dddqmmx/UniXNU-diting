"""Reject unavailable/reset telemetry and preserve process and time boundaries."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / 'scripts/decode-gpu-trace.py'
spec = importlib.util.spec_from_file_location('render_trace', path)
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)

def row(sequence, us, pid, counts, policy=2, flags=1, available=1):
    return trace.Record(sequence, us, 84, pid, (sequence, policy, flags, available, *counts))

class RenderModeTraceTests(unittest.TestCase):
    def test_process_local_counts_remain_separate(self):
        result = trace.render_mode_observations([
            row(1, 1000, 43, (100, 300, 0, 0, 4, 8, 70, 0)),
            row(2, 2000, 101, (7, 20, 8, 20, 0, 0, 3, 0), policy=0),
            row(3, 3000, 43, (105, 315, 0, 0, 5, 10, 74, 0))])['by_pid']
        self.assertEqual(result['43']['observed_delta']['gmem_passes'], 5)
        self.assertEqual(result['43']['observed_delta']['other_sysmem_passes'], 1)
        self.assertEqual(result['43']['selected_policies'], [2])
        self.assertEqual(result['101']['status'], 'INSUFFICIENT')

    def test_unavailable_counter_is_not_zero_rendering(self):
        result = trace.render_mode_observations([row(1, 1, 43, (0,)*8, available=0)])['by_pid']['43']
        self.assertEqual(result['skipped_samples'], 1)
        self.assertEqual(result['status'], 'INSUFFICIENT')
        self.assertEqual(result['samples'], [])

    def test_counter_reset_does_not_underflow(self):
        result = trace.render_mode_observations([
            row(1, 1, 43, (100, 100, 0, 0, 0, 0, 10, 0)),
            row(2, 2, 43, (1, 1, 0, 0, 0, 0, 1, 0))])['by_pid']['43']
        self.assertEqual(result['status'], 'CHANGED_OR_INVALID')
        self.assertEqual(result['observed_delta']['gmem_passes'], 0)

    def test_unknown_flags_or_reserved_data_are_rejected(self):
        for kwargs, counts in [({'flags':2}, (0,)*8), ({'policy':4}, (0,)*8),
                               ({'available':2}, (0,)*8), ({}, (0,)*7+(1,))]:
            with self.subTest(kwargs=kwargs, counts=counts):
                result = trace.render_mode_observations([row(1, 1, 43, counts, **kwargs)])['by_pid']['43']
                self.assertEqual(result['invalid_pairs_or_samples'], 1)
                self.assertEqual(result['samples'], [])

    def test_missing_snapshot_breaks_delta_boundary(self):
        result = trace.render_mode_observations([
            row(1, 1, 43, (0,)*8), row(2, 2, 43, (0,)*8, available=0),
            row(3, 3, 43, (100, 300, 0, 0, 0, 0, 50, 0))])['by_pid']['43']
        self.assertEqual(result['valid_pairs'], 0)

    def test_time_reversal_is_not_a_valid_interval(self):
        result = trace.render_mode_observations([
            row(1, 2, 43, (0,)*8), row(2, 1, 43, (1, 3, 0, 0, 0, 0, 1, 0))])['by_pid']['43']
        self.assertEqual(result['valid_pairs'], 0)
        self.assertEqual(result['status'], 'CHANGED_OR_INVALID')

if __name__ == '__main__':
    unittest.main()
