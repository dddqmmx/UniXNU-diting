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
Hardware(26): copy ID, status (0 stable, 1 unavailable, 2 off, 3 changed),
              INTF1/2 frame counters, raw panel/MDP timestamps, TE config,
              TE enable, frame-counter enable, timestamp stable bits,
              autorefresh config, resettable TE read-pointer frame/line counter
              (upper/lower 16 bits); its delta is not a lifetime TE pulse count.
A zero status with zero generation is not a completed GPU copy. None of these
events measures the panel's physical latch or input-to-photon latency.
Command(30): frontend ID, runtime ID, phase (1 commit, 2 scheduled, 3 finished),
             Metal status (4 completed, 5 error), pass count, commit ID, handle.
Runtime(39): runtime ID, stage, argument; stage definitions are in stage_trace.rs.
Host sync(80): frontend ID, phase (1 acquire, 2 publish), sync us, flush us.
Frontend frame(81): frontend ID, runtime ID, encode age us, schedule us,
                    callback us, finish us, status, passes, draws, pixels, priorities.
Frontend stages(82): frontend ID, runtime ID, nine consecutive stage deltas us,
                     then held mask. UINT64_MAX means that stage was unobserved.
Pipeline cost(83): kind (1 library, 2 function, 3 compute, 4 render), start us,
                   elapsed us, success. Costs below 1 ms are not sampled.
Render modes(84): frontend ID, selected policy (0 auto, 1 sysmem, 2 gmem,
                  3 ambiguous), flags (bit0 nocb), counters available, eight
                  cumulative process-local Turnip render-mode counters.
These are independent atomic counts of encoded passes, not GPU completion,
DDR traffic, scanout or physical frames. Retained samples bound each delta.
Test case(64): job, case, phase (1 begin, 2 end), worker start/end monotonic ns.
Legacy 61/62/63 timing remains readable. Legacy pipeline event 64 is accepted
only with a cost of at least 1 ms; test markers have a phase in that word.
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
TEST_CASE_EVENT = 64
HOST_SYNC_EVENT, FRONTEND_FRAME_EVENT, FRONTEND_STAGES_EVENT, PIPELINE_COST_EVENT = 80, 81, 82, 83
RENDER_MODE_EVENT = 84
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


def hardware_observations(records, active_gap_ms):
    rows = [r for r in records if r.event == 26]
    samples = [dict(sequence=r.sequence, us=r.us, copy_id=r.values[0], status=r.values[1],
                    intf_frames=list(r.values[2:4]), panel_timestamp_raw=r.values[4],
                    mdp_timestamp_raw=r.values[5], te_config=r.values[6], te_enable=r.values[7],
                    frame_counter_enable=r.values[8], timestamp_stable_bits=r.values[9],
                    autorefresh=r.values[10], te_counter_raw=r.values[11]) for r in rows]
    pairs = []
    for before, after in zip(rows, rows[1:]):
        elapsed = after.us - before.us
        if before.values[1] or after.values[1] or not 0 < elapsed <= active_gap_ms * 1000:
            continue
        item = dict(before_sequence=before.sequence, after_sequence=after.sequence, elapsed_us=elapsed)
        for name, word, bits, qualified in (
                ('intf_counter_rate_hz', 2, 32, lambda r: r.values[8] & 1),
                ('external_te_counter_rate_hz', 11, 16,
                 lambda r: (r.values[7] & 1) and (r.values[6] & 0x180000) == 0x180000)):
            if not qualified(before) or not qualified(after):
                continue
            a, b = before.values[word], after.values[word]
            if word == 11:
                a, b = a >> 16, b >> 16
            delta = (b - a) & ((1 << bits) - 1)
            # A discontinuity/reset is not billions of fast frames. This
            # generous bound is only a rejection guard, never a PASS target.
            if delta <= math.ceil(elapsed / 1000):
                item[name] = round(delta * 1_000_000 / elapsed, 6)
        if len(item) > 3:
            pairs.append(item)
    return dict(observations=len(rows), statuses={str(k): v for k, v in Counter(r.values[1] for r in rows).items()},
                samples=samples, counter_pairs=pairs, timestamp_units='RAW', physical_presented_frame_count=None,
                scope='existing enabled DPU/TE counters sampled after GPU copy; counters and TE pulses do not prove composed frames')


def render_mode_observations(records):
    names = ('gmem_passes', 'gmem_kpixels', 'autotuned_sysmem_passes',
             'autotuned_sysmem_kpixels', 'other_sysmem_passes',
             'other_sysmem_kpixels', 'unpadded_gmem_passes', 'reserved')
    by_pid = {}
    for pid in sorted({r.pid for r in records if r.event == RENDER_MODE_EVENT}):
        rows = sorted((r for r in records if r.event == RENDER_MODE_EVENT and r.pid == pid),
                      key=lambda r: r.sequence)
        valid, skipped, invalid, pairs, previous = [], 0, 0, 0, None
        totals = [0] * 8
        for row in rows:
            policy, flags, available = row.values[1:4]
            if policy > 3 or flags & ~1 or available not in (0, 1) or row.values[11]:
                invalid += 1
                previous = None
                continue
            if not available:
                skipped += 1
                previous = None
                continue
            counts = row.values[4:12]
            valid.append(dict(sequence=row.sequence, us=row.us, frontend_id=row.values[0],
                              policy=policy, nocb=bool(flags & 1), counters=dict(zip(names, counts))))
            if previous is not None:
                if row.us < previous.us or any(a < b for a, b in zip(counts, previous.values[4:12])):
                    invalid += 1
                else:
                    pairs += 1
                    for i, (a, b) in enumerate(zip(counts, previous.values[4:12])):
                        totals[i] += a - b
            previous = row
        by_pid[str(pid)] = dict(status='CHANGED_OR_INVALID' if invalid else 'OBSERVED' if pairs else 'INSUFFICIENT',
            samples=valid, skipped_samples=skipped, invalid_pairs_or_samples=invalid,
            valid_pairs=pairs, observed_delta=dict(zip(names, totals)),
            selected_policies=sorted({row['policy'] for row in valid}))
    return dict(schema=1, by_pid=by_pid,
        scope='Encoded process-local render-pass counters between retained samples; no per-command attribution, bandwidth or physical FPS proof')


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
    refresh_delays, refresh_residuals, refresh_skips = [], [], []
    invalid_refresh_clock = 0
    for r in refreshes:
        reported, hz, due, fired, period, skipped, armed = r.values[2:9]
        if not period:  # Legacy notification records have no callback clock evidence.
            continue
        if (not due or not hz or fired < due or skipped != (fired - due) // period or
            reported != due + skipped * period or armed <= fired or (armed - due) % period):
            invalid_refresh_clock += 1
            continue
        # Use the recorded period, rather than assume a CPU counter frequency.
        scale = 1000 / (hz * period)
        refresh_delays.append((fired - due) * scale)
        refresh_residuals.append((fired - reported) * scale)
        refresh_skips.append(skipped)
    result['display']['software_refresh'] = dict(
        notifications=len(refreshes), successful_sends=sum(r.values[1] == 0 for r in refreshes),
        send_errors={str(code): n for code, n in Counter(r.values[1] for r in refreshes if r.values[1]).items()},
        requested_rates_hz={str(rate): n for rate, n in Counter(r.values[3] for r in refreshes).items()},
        cpu_observed_intervals=intervals(refreshes, budget, active_gap_ms),
        clock_evidence=dict(samples=len(refresh_delays), invalid_samples=invalid_refresh_clock,
            scheduled_deadline_to_callback=distribution(refresh_delays, budget),
            reported_period_to_callback=distribution(refresh_residuals, budget),
            skipped_periods=sum(refresh_skips), max_skipped_periods=max(refresh_skips, default=0),
            callbacks_skipping_periods=sum(n > 0 for n in refresh_skips),
            timestamp_units='MACH_ABSOLUTE_TICKS; milliseconds scaled by each recorded requested period'),
        scope='software notification delivery attempts; not client processing or physical panel refresh')
    result['display']['hardware_observations'] = hardware_observations(live, active_gap_ms)
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
                              host_sync={str(phase): dict(sync=distribution([r.values[2] / 1000 for r in live if r.event in (61, HOST_SYNC_EVENT) and r.values[1] == phase]),
                                                         flush=distribution([r.values[3] / 1000 for r in live if r.event in (61, HOST_SYNC_EVENT) and r.values[1] == phase]))
                                         for phase in (1, 2)})
    timed = [r for r in live if r.event in (62, FRONTEND_FRAME_EVENT)]
    def timing(records):
        return {name: distribution([r.values[index] / 1000 for r in records if r.values[index] != 2**64 - 1], budget)
                for name, index in [('encode_age', 2), ('scheduled', 3), ('callback', 4), ('completion_work', 5)]}
    stages = [r for r in live if r.event in (63, FRONTEND_STAGES_EVENT)]
    result['frontend']['timing'] = dict(total=timing(timed),
        by_pid={str(pid): timing([r for r in timed if r.pid == pid]) for pid in sorted({r.pid for r in timed})},
        scope='command creation to commit includes caller idle time; CPU completion work and runtime stages are distinct from CP execution')
    stage_names = ['run', 'prepare', 'handoff', 'inbox', 'held', 'submit', 'wait_begin', 'poll', 'seen']
    result['frontend']['runtime_stages'] = {
        str(pid): {name: distribution([r.values[i + 2] / 1000 for r in stages if r.pid == pid and r.values[i + 2] != 2**64 - 1], budget)
                   for i, name in enumerate(stage_names)} for pid in sorted({r.pid for r in stages})}
    result['frontend']['render_modes'] = render_mode_observations(live)
    costs = [r for r in live if r.event in (TEST_CASE_EVENT, PIPELINE_COST_EVENT) and
             1 <= r.values[0] <= 4 and r.values[2] >= 1000 and r.values[3] in (0, 1)]
    result['frontend']['pipeline_costs_over_1ms'] = {
        str(pid): {str(kind): dict(elapsed=distribution([r.values[2] / 1000 for r in costs if r.pid == pid and r.values[0] == kind], budget),
                                  failures=sum(not r.values[3] for r in costs if r.pid == pid and r.values[0] == kind))
                   for kind in sorted({r.values[0] for r in costs if r.pid == pid})} for pid in sorted({r.pid for r in costs})}
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
