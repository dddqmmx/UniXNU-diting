import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('gpu_trace', Path(__file__).resolve().parents[1] / 'scripts/decode-gpu-trace.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def record(sequence, us, event, pid=0, *values):
    return struct.pack('<QQII12Q', sequence, us, event, pid, *(list(values) + [0] * (12 - len(values))))


def snapshot(records, issued=None):
    issued = issued if issued is not None else max((struct.unpack_from('<Q', r)[0] for r in records), default=0)
    return struct.pack('<IIIIQQ', 0x31545047, 1, 120, len(records), issued, 0) + b''.join(records)


class GpuTraceTests(unittest.TestCase):
    def test_120fps_budget_rejects_work_that_fits_a_60fps_frame(self):
        data = snapshot([record(1, 1100, 10, 43, 1, 0, 0, 0, 0, 0, 1, 0, 1000, 1),
                         record(2, 10500, 11, 43, 1, 0, 0, 1000, 19200, 192000, 1)])
        current = trace.analyze(data)
        previous = trace.analyze(data, refresh_hz=60)
        self.assertEqual(current['requested_refresh_hz'], 120)
        self.assertEqual(current['gpu']['cp_execution']['over_budget_count'], 1)
        self.assertEqual(previous['gpu']['cp_execution']['over_budget_count'], 0)
        self.assertNotEqual(current['performance_status'], 'PASS')

    def test_refresh_sends_do_not_certify_client_processing_or_physical_frames(self):
        data = snapshot([record(1, 1000, 25, 0, 1, 0, 19200, 120),
                         record(2, 9333, 25, 0, 2, 0xe00002c2, 179200, 120),
                         record(3, 1_009_333, 25, 0, 3, 0, 19379200, 120)])
        result = trace.analyze(data)
        refresh = result['display']['software_refresh']
        self.assertEqual(refresh['notifications'], 3)
        self.assertEqual(refresh['successful_sends'], 2)
        self.assertEqual(refresh['send_errors'], {str(0xe00002c2): 1})
        self.assertEqual(refresh['requested_rates_hz'], {'120': 3})
        self.assertEqual(refresh['cpu_observed_intervals']['longer_gaps_count'], 1)
        self.assertIsNone(result['display']['physical_presented_frame_count'])
        self.assertNotEqual(result['performance_status'], 'PASS')

    def test_bad_header_and_short_reads_do_not_become_empty_success(self):
        for data in (b'', bytes(32), snapshot([record(1, 100, 10)])[:-1]):
            with self.subTest(bytes=len(data)), self.assertRaises(ValueError):
                trace.analyze(data)
        result = trace.analyze(snapshot([]))
        self.assertEqual(result['performance_status'], 'INSUFFICIENT_GPU_DISPLAY_EVIDENCE')
        self.assertIsNone(result['gpu']['cp_execution']['p95_ms'])

    def test_metadata_duplicates_are_deduplicated_and_conflicts_are_rejected(self):
        meta = record(1, 10, 1)
        result = trace.analyze(snapshot([meta, meta]))
        self.assertEqual(result['header']['duplicate_records'], 1)
        self.assertEqual(result['event_counts']['1'], 1)
        with self.assertRaises(ValueError):
            trace.analyze(snapshot([meta, record(1, 20, 1)]))

    def test_cp_ticks_use_the_19_2_mhz_clock_and_cpu_retirement_is_separate(self):
        data = snapshot([record(1, 1200, 10, 43, 9, 2, 0, 1, 0, 0, 1, 0, 1000, 1),
                         record(2, 4000, 11, 43, 9, 2, 0, 1000, 50000, 69200, 1)])
        result = trace.analyze(data)['gpu']
        self.assertEqual(result['cp_execution']['p95_ms'], 1)
        self.assertEqual(result['submission_begin_to_cpu_retire']['p95_ms'], 3)
        self.assertEqual(result['ring_submit_to_cpu_retire']['p95_ms'], 2.8)
        self.assertEqual(result['paired_submissions'], 1)

    def test_pid_queue_and_submission_time_prevent_cross_client_fence_joins(self):
        data = snapshot([record(1, 1200, 10, 43, 9, 2, 0, 0, 0, 0, 1, 0, 1000, 1),
                         record(2, 4000, 11, 6, 9, 2, 0, 1000, 0, 0, 0),
                         record(3, 5000, 11, 43, 9, 3, 0, 1000, 0, 0, 0)])
        result = trace.analyze(data)['gpu']
        self.assertEqual(result['paired_submissions'], 0)
        self.assertEqual(result['retirements_without_retained_submit'], 2)
        self.assertEqual(result['cp_execution']['count'], 0)

    def test_copy_ids_have_their_own_namespace_and_zero_generation_is_not_success(self):
        data = snapshot([record(1, 1000, 20, 0, 9, 7, 10),
                         record(2, 2500, 20, 0, 9, 1, 193),
                         record(3, 4000, 23, 0, 9, 0, 0, 3),
                         record(4, 6000, 23, 0, 10, 0, 0, 0)])
        result = trace.analyze(data)['display']
        self.assertEqual(result['confirmed_gpu_copy_completions'], 1)
        self.assertEqual(result['unsuccessful_or_unconfirmed_latches'], 1)
        self.assertEqual(result['offer_to_copy_complete']['p95_ms'], 3)
        self.assertIsNone(result['physical_presented_frame_count'])

    def test_old_metadata_does_not_extend_coverage_and_long_gaps_are_preserved(self):
        data = snapshot([record(1, 100, 1), record(100000, 1_000_000, 23, 0, 1, 0, 0, 1),
                         record(100001, 1_016_000, 23, 0, 2, 0, 0, 1),
                         record(100002, 11_016_000, 23, 0, 3, 0, 0, 1)], issued=100002)
        result = trace.analyze(data)
        self.assertEqual(result['coverage']['start_us'], 1_000_000)
        self.assertEqual(result['coverage']['older_metadata_records'], 1)
        intervals = result['display']['copy_completion_intervals']
        self.assertEqual(intervals['all']['max_ms'], 10000)
        self.assertEqual(intervals['within_active_gap']['count'], 1)
        self.assertEqual(intervals['longer_gaps_count'], 1)
        self.assertNotEqual(result['performance_status'], 'PASS')

    def test_errors_and_missing_or_reversed_timestamps_are_not_fast_samples(self):
        data = snapshot([record(1, 1000, 11, 43, 1, 0, 0, 2000, 500, 100, 1),
                         record(2, 2000, 30, 43, 9, 10, 1, 0),
                         record(3, 3000, 30, 43, 9, 10, 3, 5)])
        result = trace.analyze(data)
        self.assertEqual(result['gpu']['invalid_total_timestamps'], 1)
        self.assertEqual(result['gpu']['cp_execution']['count'], 0)
        self.assertEqual(result['frontend']['error_completions'], 1)
        self.assertEqual(result['frontend']['completed_command_latency']['count'], 0)


if __name__ == '__main__':
    unittest.main()
