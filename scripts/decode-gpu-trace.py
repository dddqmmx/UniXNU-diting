#!/usr/bin/env python3
"""Decode Diting GPU trace v1, retaining the limits of each snapshot.

Header: <IIIIQQ>, magic 0x31545047, version 1, record_bytes 120.
Record: <QQII12Q>, sequence, uptime us, event, pid, twelve event words.
Submit(10): fence, queue, unused, priority, role, unused, peak jobs,
            context, submission-begin uptime us, handle count.
Retire(11): fence, queue, unused, submission-begin uptime us,
            CP start ticks, CP end ticks, timestamps present, unused.
The CP clock is 19.2 MHz. Its ticks are NOT uptime us and cannot be subtracted
from CPU timestamps without calibration. Retire is CPU-observed completion.
Display(20) phase 7 is a GPU copy offer, phase 1 an ordinary framebuffer swap;
phase 4/8 is completed GPU/CPU copying. These have separate ID namespaces.
Latch(23): copy ID, status, unused, successful source generation, unused.
Refresh(25): callback counter, send result, scheduled absolute ticks, target Hz.
A zero status with zero generation is not a completed GPU copy. None of these
events measures the panel's physical latch or input-to-photon latency.
Command(30): frontend ID, runtime ID, phase (1 commit, 2 scheduled, 3 finished),
             Metal status (4 completed, 5 error), pass count, commit ID, handle.
Runtime(39): runtime ID, stage, argument; stage definitions are in stage_trace.rs.
Host sync(61): frontend ID, phase (1 acquire, 2 publish), sync us, flush us.
Process text(46) and build/debug text(47/58/59) pack UTF-8 in the twelve words.

Metadata survives ring wrap. Only the final 16384 sequence positions describe
the live time window; metadata timestamps do not extend measured coverage.
Repeated metadata is deduplicated. Snapshots are reported independently.
Long gaps remain in raw statistics and are also reported separately; they may
be idle, sleep, uncaptured work or a stall. This tool never certifies UI PASS.
"""
import argparse
from collections import Counter, defaultdict, namedtuple
import json
import math
from pathlib import Path
import struct

HEADER = struct.Struct('<IIIIQQ')
RECORD = struct.Struct('<QQII12Q')
CAPACITY = 16384
Record = namedtuple('Record', 'sequence us event pid values')


def parse(data):
    if not HEADER.size <= len(data) <= 16 << 20:
        raise ValueError('Trace header missing or snapshot exceeds its size bound')
    magic, version, size, count, issued, dropped = HEADER.unpack_from(data)
    if (magic, version, size) != (0x31545047, 1, RECORD.size):
        raise ValueError('Unsupported GPU trace magic, version or record size')
    end = HEADER.size + count * size
    if end > len(data) or len(data) - end >= 4096 or any(data[end:]):
        raise ValueError('Truncated records or unexpected trailing payload')
    records = {}
    duplicates = 0
    for i in range(count):
        words = RECORD.unpack_from(data, HEADER.size + i * size)
        item = Record(*words[:4], words[4:])
        if not 0 < item.sequence <= issued or not item.event:
            raise ValueError('Invalid trace sequence or event')
        previous = records.get(item.sequence)
        if previous is not None:
            if previous != item:
                raise ValueError('Conflicting records share one sequence')
            duplicates += 1
        records[item.sequence] = item
    return dict(issued=issued, dropped=dropped, stored_records=count,
                duplicate_records=duplicates), sorted(records.values())


def distribution(values, budget=None):
    values = sorted(values)
    result = dict(count=len(values), p50_ms=None, p95_ms=None, p99_ms=None, max_ms=None)
    if values:
        for key, p in [('p50_ms', .5), ('p95_ms', .95), ('p99_ms', .99)]:
            result[key] = round(values[max(0, math.ceil(len(values) * p) - 1)], 6)
        result['max_ms'] = round(values[-1], 6)
    if budget is not None:
        result['over_budget_count'] = sum(value > budget for value in values)
    return result


def intervals(records, budget, active_gap_ms):
    records = sorted(records, key=lambda item: item.us)
    values = [(b.us - a.us) / 1000 for a, b in zip(records, records[1:])]
    return dict(all=distribution(values, budget),
                within_active_gap=distribution([v for v in values if v <= active_gap_ms], budget),
                gap_threshold_ms=active_gap_ms,
                longer_gaps_count=sum(v > active_gap_ms for v in values),
                longer_gaps_ms=[round(v, 6) for v in values if v > active_gap_ms],
                gap_classification='idle/sleep/stall/uncaptured; not inferred dropped frames')


def text_words(values):
    return struct.pack('<12Q', *values).split(b'\0', 1)[0].decode('utf-8', errors='replace')


def analyze(data, refresh_hz=120, active_gap_ms=250, stream=False):
    if not math.isfinite(refresh_hz) or refresh_hz <= 0 or not math.isfinite(active_gap_ms) or active_gap_ms <= 0:
        raise ValueError('Positive finite refresh target and gap threshold required')
    header, records = parse(data)
    budget = 1000 / refresh_hz
    first = min((r.sequence for r in records), default=header['issued'] + 1) if stream else max(1, header['issued'] - CAPACITY + 1)
    live = [r for r in records if r.sequence >= first]
    expected = header['issued'] - first + 1
    event_counts = Counter(r.event for r in live)
    processes = {str(r.pid): text_words(r.values) for r in records if r.event == 46}
    result = dict(format='diting-gpu-trace-analysis-v1', header=header,
                  capture_transport='cdc-cursor-stream' if stream else 'retained-kernel-snapshot',
                  coverage=dict(first_sequence=first, last_sequence=header['issued'],
                                retained_live_records=len(live), missing_live_sequence_positions=expected - len(live),
                                older_metadata_records=len(records) - len(live),
                                start_us=min((r.us for r in live), default=None),
                                end_us=max((r.us for r in live), default=None)),
                  event_counts={str(k): v for k, v in sorted(event_counts.items())}, processes=processes,
                  build_debug_text=[dict(pid=r.pid, event=r.event, text=text_words(r.values))
                                    for r in records if r.event in (47, 58, 59)],
                  requested_refresh_hz=refresh_hz, frame_budget_ms=budget,
                  physical_panel_timestamps_available=False,
                  performance_status='REQUIRES_UI_VALIDATION')
    submits = {(r.pid, r.values[1], r.values[0], r.values[8]): r for r in live if r.event == 10}
    retired = set()
    total, after, gpu = [], [], []
    by_pid = defaultdict(lambda: dict(total=[], gpu=[]))
    unpaired, invalid_total, missing_gpu = 0, 0, 0
    jobs = []
    for r in live:
        if r.event != 11:
            continue
        fence, queue, _, begin, start, end, present = r.values[:7]
        key = (r.pid, queue, fence, begin)
        submit = submits.get(key)
        if submit is None:
            unpaired += 1
        else:
            retired.add(key)
            if r.us >= submit.us:
                after.append((r.us - submit.us) / 1000)
        latency = (r.us - begin) / 1000 if begin and r.us >= begin else None
        execution = (end - start) / 19200 if present and start and end >= start else None
        if latency is None:
            invalid_total += 1
        else:
            total.append(latency)
            by_pid[r.pid]['total'].append(latency)
        if execution is None:
            missing_gpu += 1
        else:
            gpu.append(execution)
            by_pid[r.pid]['gpu'].append(execution)
        jobs.append(dict(sequence=r.sequence, us=r.us, pid=r.pid, fence=fence, queue=queue,
                         submission_begin_us=begin, cpu_observed_total_ms=latency, gpu_execution_ms=execution))
    result['gpu'] = dict(submissions=len(submits), retirements=event_counts[11],
                         paired_submissions=len(retired), retained_submissions_without_retire=len(submits) - len(retired),
                         retirements_without_retained_submit=unpaired, invalid_total_timestamps=invalid_total,
                         unavailable_gpu_timestamps=missing_gpu,
                         submission_begin_to_cpu_retire=distribution(total, budget),
                         ring_submit_to_cpu_retire=distribution(after, budget),
                         cp_execution=distribution(gpu, budget),
                         by_pid={str(pid): dict(name=processes.get(str(pid)),
                                               total=distribution(rows['total'], budget),
                                               cp_execution=distribution(rows['gpu'], budget))
                                 for pid, rows in by_pid.items()}, jobs=jobs)
    offers = {(r.pid, r.values[0]): r for r in live if r.event == 20 and r.values[1] == 7}
    latches = [r for r in live if r.event == 23]
    completed = [r for r in latches if r.values[1] == 0 and r.values[3] != 0]
    copy_times, copies = [], []
    unpaired_latches = 0
    for r in completed:
        offer = offers.get((r.pid, r.values[0]))
        latency = (r.us - offer.us) / 1000 if offer is not None and r.us >= offer.us else None
        if latency is None:
            unpaired_latches += 1
        else:
            copy_times.append(latency)
        copies.append(dict(sequence=r.sequence, us=r.us, id=r.values[0], offer_us=offer.us if offer else None,
                           copy_latency_ms=latency))
    result['display'] = dict(gpu_copy_offers=len(offers), confirmed_gpu_copy_completions=len(completed),
                             unsuccessful_or_unconfirmed_latches=len(latches) - len(completed),
                             completions_without_retained_offer=unpaired_latches,
                             offer_to_copy_complete=distribution(copy_times, budget),
                             copy_completion_intervals=intervals(completed, budget, active_gap_ms),
                             cpu_copy_events=sum(r.event == 20 and r.values[1] == 8 for r in live),
                             physical_presented_frame_count=None, copies=copies)
    refreshes = [r for r in live if r.event == 25]
    result['display']['software_refresh'] = dict(
        notifications=len(refreshes), successful_sends=sum(r.values[1] == 0 for r in refreshes),
        send_errors={str(code): n for code, n in Counter(r.values[1] for r in refreshes if r.values[1]).items()},
        requested_rates_hz={str(rate): n for rate, n in Counter(r.values[3] for r in refreshes).items()},
        cpu_observed_intervals=intervals(refreshes, budget, active_gap_ms),
        scope='software notification delivery attempts; not client processing or physical panel refresh')
    frontend = defaultdict(dict)
    for r in live:
        if r.event == 30 and r.values[2] in (1, 2, 3):
            frontend[(r.pid, r.values[0])][r.values[2]] = r
    command_times, failed = [], 0
    for phases in frontend.values():
        begin, end = phases.get(1), phases.get(3)
        if end is not None and end.values[3] == 5:
            failed += 1
        if begin is not None and end is not None and end.values[3] == 4 and end.us >= begin.us:
            command_times.append((end.us - begin.us) / 1000)
    result['frontend'] = dict(observed_commands=len(frontend), completed_command_latency=distribution(command_times, budget),
                              error_completions=failed,
                              host_sync={str(phase): dict(sync=distribution([r.values[2] / 1000 for r in live if r.event == 61 and r.values[1] == phase]),
                                                         flush=distribution([r.values[3] / 1000 for r in live if r.event == 61 and r.values[1] == phase]))
                                         for phase in (1, 2)})
    result['recovery_events'] = event_counts[13]
    result['client_reported_dropped_events'] = sum(r.values[0] for r in live if r.event == 41)
    if not completed or not total:
        result['performance_status'] = 'INSUFFICIENT_GPU_DISPLAY_EVIDENCE'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, nargs='+')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--refresh-hz', type=float, default=120, help='comparison target; not a measured panel rate')
    parser.add_argument('--active-gap-ms', type=float, default=250, help='separate long gaps while preserving raw statistics')
    parser.add_argument('--stream', action='store_true', help='analyze a CDC cursor stream; inspect its capture.json for sequence losses')
    args = parser.parse_args()
    reports = []
    for path in args.input:
        report = analyze(path.read_bytes(), args.refresh_hz, args.active_gap_ms, args.stream)
        report['input'] = str(path.resolve())
        reports.append(report)
    payload = dict(snapshots=reports, hardware_performance_pass=False,
                   scope='retained GPU/copy events; no physical scanout or complete input latency measurement')
    encoded = json.dumps(payload, indent=2) + '\n'
    if args.output:
        args.output.write_text(encoded)
        for r in reports:
            print(Path(r['input']).name, 'retire p95 ms', r['gpu']['submission_begin_to_cpu_retire']['p95_ms'],
                  'GPU p95 ms', r['gpu']['cp_execution']['p95_ms'],
                  'copy p95 ms', r['display']['offer_to_copy_complete']['p95_ms'],
                  'live records', r['coverage']['retained_live_records'])
    else:
        print(encoded, end='')


if __name__ == '__main__':
    main()
