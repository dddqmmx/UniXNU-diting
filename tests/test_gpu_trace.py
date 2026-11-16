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
    def test_case_markers_cannot_become_pipeline_costs(self):
        data = snapshot([record(1, 10000, 64, 16, 1, 4, 1, 0),
                         record(2, 19000, 64, 16, 1, 4, 2, 0),
                         record(3, 20000, 83, 43, 4, 11000, 9000, 1),
                         record(4, 21000, 81, 43, 2, 7, 3000, 1000, 4000, 500, 4),
                         record(5, 22000, 82, 43, 2, 7, 100, 200, 300, 400, 500, 600, 700, 800, 900)])
        frontend = trace.analyze(data)['frontend']
        self.assertNotIn('16', frontend['pipeline_costs_over_1ms'])
        self.assertEqual(frontend['pipeline_costs_over_1ms']['43']['4']['elapsed']['p95_ms'], 9)
        self.assertEqual(frontend['timing']['by_pid']['43']['encode_age']['p95_ms'], 3)
        self.assertEqual(frontend['runtime_stages']['43']['submit']['p95_ms'], .6)

    def test_frontend_timing_keeps_cpu_stages_cp_ticks_and_unobserved_times_distinct(self):
        missing = 2**64 - 1
        data = snapshot([record(1, 10000, 62, 43, 1, 9, 20000, missing, 15000, 4000, 4, 8, 32, 3300000),
                         record(2, 11000, 63, 43, 1, 9, 100, 2000, missing, 3000, 1000, 500, 100, 200, 300, 7),
                         record(3, 12000, 64, 299, 4, 9000, 9000, 1)])
        result = trace.analyze(data)
        frontend = result['frontend']
        self.assertEqual(frontend['timing']['by_pid']['43']['encode_age']['p95_ms'], 20)
        self.assertEqual(frontend['timing']['by_pid']['43']['scheduled']['count'], 0)
        self.assertEqual(frontend['timing']['by_pid']['43']['completion_work']['p95_ms'], 4)
        self.assertEqual(frontend['runtime_stages']['43']['prepare']['p95_ms'], 2)
        self.assertEqual(frontend['runtime_stages']['43']['handoff']['count'], 0)
        self.assertEqual(frontend['pipeline_costs_over_1ms']['299']['4']['elapsed']['p95_ms'], 9)
        self.assertEqual(result['gpu']['cp_execution']['count'], 0)
        self.assertNotEqual(result['performance_status'], 'PASS')

    def test_hardware_counters_retain_raw_units_and_do_not_certify_composed_fps(self):
        data = snapshot([record(1, 1000, 26, 0, 1, 0, 10, 0, 100000, 100100, 0x180000, 1, 1, 3, 0, 65535 << 16),
                         record(2, 17667, 26, 0, 2, 0, 12, 0, 200000, 200100, 0x180000, 1, 1, 3, 0, 1 << 16)])
        result = trace.analyze(data)
        hardware = result['display']['hardware_observations']
        self.assertEqual(hardware['observations'], 2)
        self.assertEqual(hardware['timestamp_units'], 'RAW')
        self.assertAlmostEqual(hardware['counter_pairs'][0]['external_te_counter_rate_hz'], 120, delta=.01)
        self.assertIsNone(hardware['physical_presented_frame_count'])
        self.assertNotEqual(result['performance_status'], 'PASS')

    def test_disabled_changed_or_reset_hardware_counters_do_not_become_fast_samples(self):
        for status, enable, last in ((2, 1, 12), (3, 1, 12), (0, 0, 12), (0, 1, 0)):
            data = snapshot([record(1, 1000, 26, 0, 1, 0, 100, 0, 0, 0, 0, 0, enable),
                             record(2, 9333, 26, 0, 2, status, last, 0, 0, 0, 0, 0, enable)])
            self.assertEqual(trace.analyze(data)['display']['hardware_observations']['counter_pairs'], [])

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

    def test_late_refresh_reports_latest_period_and_keeps_raw_deadline_evidence(self):
        data = snapshot([record(1, 20000, 25, 0, 1, 0, 339200, 120,
                                19200, 379200, 160000, 2, 499200),
                         record(2, 28000, 25, 0, 2, 0, 499200, 120,
                                499200, 503000, 160000, 0, 659200)])
        result = trace.analyze(data)
        clock = result['display']['software_refresh']['clock_evidence']
        self.assertEqual((clock['samples'], clock['invalid_samples']), (2, 0))
        self.assertEqual(clock['skipped_periods'], 2)
        self.assertEqual(clock['callbacks_skipping_periods'], 1)
        self.assertAlmostEqual(clock['scheduled_deadline_to_callback']['max_ms'], 18.75)
        self.assertAlmostEqual(clock['reported_period_to_callback']['max_ms'], 1000 / 480, places=6)
        self.assertIsNone(result['display']['physical_presented_frame_count'])

    def test_stale_refresh_timestamp_cannot_become_valid_callback_timing(self):
        data = snapshot([record(1, 20000, 25, 0, 1, 0, 19200, 120,
                                19200, 379200, 160000, 2, 499200)])
        clock = trace.analyze(data)['display']['software_refresh']['clock_evidence']
        self.assertEqual((clock['samples'], clock['invalid_samples']), (0, 1))
        self.assertEqual(clock['scheduled_deadline_to_callback']['count'], 0)

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
