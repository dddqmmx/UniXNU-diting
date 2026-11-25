"""USB GPU job transport, independent result checks, and reproducible reports."""
import base64
import copy
from datetime import datetime, timezone
import importlib.util
import json
import math
from pathlib import Path
import re
import struct
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CATALOG = json.loads((ROOT / 'configs/diting-gpu-tests.json').read_text())
PROFILES = dict(qualification=1, performance=2, all=3, ui=4)
POLICIES = dict(auto=0, sysmem=1, gmem=2)
STATES = {0: 'IDLE', 1: 'PENDING', 2: 'RUNNING', 3: 'PASSED', 4: 'FAILED',
          5: 'CANCELLED', 6: 'TIMED_OUT', 7: 'OVERFLOW'}
RESULT_LIMIT = 1048576
TRACE_HEADER = struct.Struct('<4I2Q')
TRACE_RECORD = struct.Struct('<2Q2I12Q')
UI_FRAME = struct.Struct('<IIQ')
UI_INPUT = struct.Struct('<I')


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


STREAM = module('diting_gpu_live_decoder', 'capture-diting-gpu-live.py')
DISPLAY = module('diting_gpu_display_decoder', 'decode-diting-display.py')
TRACE = module('diting_gpu_trace_decoder', 'decode-gpu-trace.py')


def body(response, command):
    begin = ('DitingUSB BEGIN ' + command + '\n').encode()
    end = ('DitingUSB END ' + command + '\n').encode()
    if (len(response) > 4096 or not response.startswith(begin) or not response.endswith(end) or
            response.count(begin) != 1 or response.count(end) != 1):
        raise ValueError('missing, duplicate or oversized matching USB frame')
    return response[len(begin):-len(end)]


def fields(line):
    result = {}
    for word in line.split()[1:]:
        if '=' not in word:
            raise ValueError('malformed GPUTEST field')
        key, value = word.split('=', 1)
        if key in result:
            raise ValueError('duplicate GPUTEST field: ' + key)
        result[key] = value
    return result


def number(value, maximum=(1 << 64) - 1):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9]{1,20}', value):
        raise ValueError('unsigned decimal field required')
    result = int(value)
    if result > maximum:
        raise ValueError('field exceeds integer bound')
    return result


def status(response, command):
    lines = body(response, command).decode('ascii').splitlines()
    if len(lines) != 1 or not lines[0].startswith('GPUTEST '):
        raise ValueError('one GPU job status required')
    data = fields(lines[0])
    if data.get('schema') != '1':
        raise ValueError('unsupported GPU job schema')
    if data.get('available') != '1':
        raise RuntimeError('GPU automation unavailable: ' + data.get('reason', 'missing broker'))
    required = ('error', 'daemon', 'heartbeat_age_us', 'job', 'state', 'profile', 'policy', 'bytes',
                'case', 'stage', 'worker_error', 'created_us', 'completed_us', 'timeout_ms', 'quiescing')
    parsed = {key: number(data[key]) for key in required}
    if parsed['state'] not in STATES or parsed['bytes'] > RESULT_LIMIT or parsed['quiescing'] > 1:
        raise ValueError('invalid GPU state/result bound')
    return parsed


def result_chunk(response, command, job, offset, requested, total):
    lines = body(response, command).splitlines()
    if len(lines) != 2 or not lines[0].startswith(b'GPUTEST ') or not lines[1].startswith(b'DATA '):
        raise ValueError('one bounded GPU result chunk required')
    header = fields(lines[0].decode('ascii'))
    if header.get('schema') != '1':
        raise ValueError('unsupported GPU result schema')
    actual = {key: number(header[key]) for key in ('error', 'job', 'offset', 'bytes', 'total')}
    if (not 0 <= offset <= total or actual['error'] or actual['job'] != job or actual['offset'] != offset or actual['total'] != total or
            actual['bytes'] != min(requested, total-offset) or not 0 < requested <= 1024 or total > RESULT_LIMIT):
        raise ValueError('stale, rejected or inconsistent GPU result chunk')
    encoded = lines[1][5:]
    if len(encoded) > 1368:
        raise ValueError('GPU result chunk exceeds base64 bound')
    raw = base64.b64decode(encoded, validate=True)
    if len(raw) != actual['bytes']:
        raise ValueError('GPU result data length mismatch')
    return raw


def expected_cases(profile, policy):
    return {item['id']: item for item in CATALOG['cases']
            if profile in item['profiles'] or profile == 3 and policy == 0 and item.get('all_auto')}


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def percentile(values, fraction=.95):
    if not values:
        return None
    values = sorted(values)
    return values[math.ceil(len(values)*fraction)-1]


def performance(case):
    if any(case.get(key) != CATALOG[key] for key in ('width', 'height', 'frames', 'warmup', 'target_fps')):
        raise ValueError('GPU workload geometry/count/target differs from the qualification catalog')
    columns = case.get('sample_columns')
    samples = case.get('samples')
    expected_columns = (['encode_us', 'submit_us', 'wait_us', 'total_us', 'gpu_us']
                        if case['name'].startswith('vk-') else
                        ['encode_us', 'commit_and_complete_us', 'total_us'])
    if (columns != expected_columns or case.get('layers') != (0, 8, 32)[(case['id']-40) % 3] or
            not isinstance(samples, list) or len(samples) != CATALOG['frames']):
        raise ValueError('missing/duplicate sample columns or incomplete GPU workload')
    totals, gpu = [], []
    for row in samples:
        if not isinstance(row, list) or len(row) != len(columns):
            raise ValueError('incomplete GPU sample row')
        measured = dict(zip(columns, row))
        if any(not finite(value) for key, value in measured.items() if key != 'gpu_us'):
            raise ValueError('negative or nonfinite GPU sample')
        if measured['total_us'] <= 0:
            raise ValueError('nonpositive workload duration')
        parts = [value for key, value in measured.items() if key not in ('gpu_us', 'total_us')]
        if sum(parts) > measured['total_us'] + 2:
            raise ValueError('GPU sample parts exceed measured total')
        totals.append(measured['total_us'])
        if measured.get('gpu_us') is not None:
            if not finite(measured['gpu_us']):
                raise ValueError('nonfinite GPU timestamp duration')
            gpu.append(measured['gpu_us'])
    budget = 1e6 / CATALOG['target_fps']
    if case['end_ns']-case['start_ns'] < sum(totals)*1000:
        raise ValueError('sequential GPU samples exceed the case time window')
    tail, gpu_tail = percentile(totals), percentile(gpu)
    actual_pass = tail <= budget and (not case['name'].startswith('vk-') or
                                      len(gpu) == len(samples) and gpu_tail <= budget)
    return dict(pass_=bool(actual_pass and case['pass']), p50_ms=percentile(totals,.5)/1000,
                p95_ms=tail/1000, p99_ms=percentile(totals,.99)/1000, max_ms=max(totals)/1000,
                gpu_p95_ms=None if gpu_tail is None else gpu_tail/1000, sample_count=len(samples))


def ui_animation(case):
    """Assess paced gesture content separately from physical presentation.

    A changed endpoint is insufficient. Each gesture retains all 43 samples,
    plus the time spent reading the uncached framebuffer, so a late observer
    cannot turn missing observations into an animation failure or a FPS pass.
    """
    expected_motions, expected_events = {60:(8,354),61:(2,92),62:(1,50)}[case['id']]
    if (case.get('frame_sample_format') != 'le-u32-elapsed-us,u32-read-us,u64-hash' or
            case.get('frame_sample_hz') != 120 or case.get('motion_steps') != 42 or
            case.get('motion_warmup_steps') != 6 or case.get('accepted_events') != expected_events or
            type(case.get('frame_sample_count')) is not int or not 1 <= case['frame_sample_count'] <= 1536 or
            type(case.get('frame_sample_start_ns')) is not int or case['frame_sample_start_ns'] < case['start_ns'] or
            not isinstance(case.get('frame_samples_base64'),str) or len(case['frame_samples_base64']) > 32768):
        raise ValueError('UI observation/input identity differs from the fixed scenario')
    raw = base64.b64decode(case['frame_samples_base64'],validate=True)
    if len(raw) != UI_FRAME.size*case['frame_sample_count']:
        raise ValueError('incomplete UI framebuffer observation stream')
    frames = list(UI_FRAME.iter_unpack(raw))
    if (any(b[0] <= a[0] for a,b in zip(frames,frames[1:])) or
            any(case['frame_sample_start_ns']+(at+read)*1000 > case['end_ns'] for at,read,_ in frames)):
        raise ValueError('nonmonotonic or out-of-window UI framebuffer observation')
    ranges = case.get('motion_sample_ranges')
    if not isinstance(ranges,list) or len(ranges) != expected_motions:
        raise ValueError('UI motion coverage differs from the fixed scenario')
    independent = case.get('observation_schema') == 2
    inputs = []
    if independent:
        if (case.get('observer_independent') is not True or
                case.get('observer_policy_passes') != expected_motions or
                case.get('input_pacing') != 'absolute-monotonic-no-catchup' or
                case.get('input_sample_count') != expected_motions*43 or
                not isinstance(case.get('input_samples_base64'),str) or
                len(case['input_samples_base64']) > 4096):
            raise ValueError('missing independent observer or fixed input pacing identity')
        input_raw = base64.b64decode(case['input_samples_base64'],validate=True)
        if len(input_raw) != UI_INPUT.size*expected_motions*43:
            raise ValueError('incomplete UI input timing stream')
        inputs = [at for at, in UI_INPUT.iter_unpack(input_raw)]
        if (any(b <= a for a,b in zip(inputs,inputs[1:])) or
                case['frame_sample_start_ns']+inputs[-1]*1000 > case['end_ns']):
            raise ValueError('nonmonotonic or out-of-window UI input timing')
    budget = 1e6/120
    motions, previous_end = [], -1
    for motion_index,pair in enumerate(ranges):
        if (not isinstance(pair,list) or len(pair)!=2 or any(type(i) is not int for i in pair) or
                not previous_end < pair[0] <= pair[1] < len(frames) or pair[1]-pair[0] != 42):
            raise ValueError('overlapping, missing or short UI motion sample range')
        previous_end = pair[1]
        rows = frames[pair[0]:pair[1]+1]
        paced = rows[6:]  # fixed 50 ms motion warmup; retain the original samples
        intervals = [b[0]-a[0] for a,b in zip(paced,paced[1:])]
        reads = [row[1] for row in paced]
        changed = [row[0] for i,row in enumerate(paced) if i and row[2] != paced[i-1][2]]
        gaps = [b-a for a,b in zip([paced[0][0]]+changed,changed+[paced[-1][0]])]
        elapsed = paced[-1][0]-paced[0][0]
        if not 200000 <= rows[-1][0]-rows[0][0] <= 10000000:
            raise ValueError('UI motion does not cover the requested 350 ms gesture')
        observer_late = percentile(intervals) > budget*1.25 or percentile(reads) > budget/4
        input_metrics = {}
        if independent:
            stamps = inputs[motion_index*43:(motion_index+1)*43]
            input_intervals = [b-a for a,b in zip(stamps,stamps[1:])]
            start_offset,end_offset = stamps[0]-rows[0][0],stamps[-1]-rows[-1][0]
            input_metrics = dict(input_interval_p95_ms=percentile(input_intervals)/1000,
                                 input_interval_min_ms=min(input_intervals)/1000,
                                 input_duration_ms=(stamps[-1]-stamps[0])/1000,
                                 observer_start_offset_ms=start_offset/1000,
                                 observer_end_offset_ms=end_offset/1000)
            observer_late = (observer_late or percentile(input_intervals)>budget*1.25 or
                             min(input_intervals)<budget*.9 or abs(start_offset)>budget*1.25 or
                             abs(end_offset)>budget*2)
        fraction = len(changed)/(len(paced)-1)
        continuous = len(set(row[2] for row in rows)) >= 3 and fraction >= .9 and max(gaps) < 4*budget
        motions.append(dict(sample_first=pair[0],sample_last=pair[1],observations=len(rows),
                            observer_late=observer_late,observer_interval_p95_ms=percentile(intervals)/1000,
                            observer_read_p95_ms=percentile(reads)/1000,
                            distinct_content=len(set(row[2] for row in rows)),content_changes=len(changed),
                            change_fraction=fraction,observed_change_hz=len(changed)*1e6/elapsed,
                            longest_unchanged_ms=max(gaps)/1000,
                            continuous_content_observed=continuous,**input_metrics))
    state = ('INCOMPLETE' if not independent or any(row['observer_late'] for row in motions) else
             'FAIL' if any(not row['continuous_content_observed'] for row in motions) else 'OBSERVED')
    return dict(status=state,target_fps=120,sample_count=len(frames),motions=motions,
                observer_independent=independent,
                scope='separate timed observer and acknowledged paced input; pre-DPU non-atomic thumbnail, no physical FPS proof')


def decode_result(raw, job, profile, policy, *, allow_partial=False):
    if not 0 < len(raw) <= RESULT_LIMIT:
        raise ValueError('empty or oversized GPU NDJSON')
    tail = 0
    if not raw.endswith(b'\n'):
        if not allow_partial:
            raise ValueError('truncated GPU NDJSON')
        last = raw.rfind(b'\n') + 1
        tail, raw = len(raw)-last, raw[:last]
    def reject_constant(value):
        raise ValueError('nonfinite JSON constant: ' + value)
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate GPU JSON field: ' + key)
            result[key] = value
        return result
    records = [json.loads(line, parse_constant=reject_constant, object_pairs_hook=unique_object)
               for line in raw.decode('utf-8').splitlines()]
    if any(not isinstance(row, dict) for row in records) or not records:
        raise ValueError('GPU results must be JSON objects')
    if records[0].get('kind') != 'manifest':
        raise ValueError('GPU manifest required')
    manifest = records[0]
    summary = records[-1] if records[-1].get('kind') == 'summary' else None
    if not summary and not allow_partial:
        raise ValueError('terminal GPU summary required')
    if sum(row.get('kind') == 'manifest' for row in records) != 1 or sum(row.get('kind') == 'summary' for row in records) != bool(summary):
        raise ValueError('duplicate GPU manifest/summary')
    if (manifest.get('schema'), manifest.get('job'), manifest.get('profile'), manifest.get('policy')) != (1, job, profile, policy):
        raise ValueError('GPU job/profile/policy ABI mismatch')
    if any(manifest.get(key) != CATALOG[key] for key in ('width','height','frames','warmup','target_fps')):
        raise ValueError('GPU manifest does not match the fixed workload')
    if manifest.get('host_supplied_shader') is not False or type(manifest.get('pid')) is not int or manifest['pid'] <= 0:
        raise ValueError('GPU manifest worker/shader identity missing')
    if summary and (summary.get('schema'),summary.get('job'),summary.get('hardware_performance_pass'),summary.get('physical_present')) != (1,job,False,'UNQUALIFIED'):
        raise ValueError('invalid GPU summary or unsupported physical-fps claim')
    expected = expected_cases(profile, policy)
    cases, begins, ends, fatals, devices = {}, {}, {}, [], []
    active = ended = None
    for row in records[1:-1] if summary else records[1:]:
        kind = row.get('kind')
        if kind == 'device':
            devices.append(row)
        elif kind == 'fatal':
            if row.get('job') != job or not isinstance(row.get('stage'), str):
                raise ValueError('invalid GPU fatal record')
            fatals.append(row)
        elif kind in ('case','begin','end'):
            case_id = row.get('id')
            if type(case_id) is not int or case_id not in expected:
                raise ValueError('unexpected GPU test case')
            target = cases if kind == 'case' else begins if kind == 'begin' else ends
            if case_id in target:
                raise ValueError('duplicate GPU case/marker')
            if kind == 'begin':
                if active is not None or ended is not None:
                    raise ValueError('overlapping GPU case markers')
                active = case_id
            elif kind == 'end':
                if active != case_id:
                    raise ValueError('out-of-order GPU end marker')
                active, ended = None, case_id
            elif ended != case_id:
                raise ValueError('out-of-order GPU case result')
            else:
                ended = None
            target[case_id] = row
        else:
            raise ValueError('unknown GPU result record')
    device_failed = sum(not row.get('pass', False) for row in cases.values())
    if summary and (summary.get('cases') != len(cases) or type(summary.get('failures')) is not int or
            summary['failures'] != device_failed + len(fatals) or type(summary.get('pass')) is not bool or
            summary['pass'] != (summary['failures'] == 0 and len(cases)>0)):
        raise ValueError('GPU summary/counts inconsistent with original device results')
    evaluated_cases = copy.deepcopy(cases)
    for case_id, case in evaluated_cases.items():
        spec = expected[case_id]
        if (case.get('name'),case.get('category'),case.get('required')) != (spec['name'],spec['category'],True) or type(case.get('pass')) is not bool:
            raise ValueError('GPU case identity or boolean result differs from the catalog')
        if (case_id not in begins or case_id not in ends or
                case.get('start_ns') != begins[case_id].get('start_ns') or
                case.get('start_ns') != ends[case_id].get('start_ns') or
                case.get('end_ns') != ends[case_id].get('end_ns') or
                not finite(case.get('start_ns')) or not finite(case.get('end_ns')) or
                case['end_ns'] < case['start_ns']):
            raise ValueError('missing or inconsistent GPU case time markers')
        case['device_pass'] = case['pass']
        if case_id in (15,16,17,18,19,31,32,33,34,35,36,37,46,47,48) and case['pass']:
            modes, sides, events = case.get('render_mode_delta'), case.get('gmem_side_delta'), case.get('gmem_event_delta')
            partial = case_id in (34,35,36,37,46,47,48)
            imported = case_id in (46,47,48)
            minimum = 7 if partial else 2 if case_id in (15,16,19) else 4 if case_id >= 31 else 1
            surface = case_id in (31,32,33)
            linear, fetch = None if surface else case_id not in (18,37), case_id in (17,18)
            target = 'metal-iosurface' if surface else 'vulkan-linear-image' if linear else 'vulkan-optimal-image'
            if (case.get('path_verified') is not True or case.get('linear_tiling') is not linear or
                    case.get('render_target_kind') != target or
                    case.get('input_fetch') is not fetch or case.get('minimum_render_passes') != minimum or
                    not isinstance(modes,list) or len(modes) != 8 or not isinstance(sides,list) or len(sides) != 16 or
                    not isinstance(events,list) or len(events) != 8 or
                    any(type(value) is not int or value < 0 for value in modes+sides+events)):
                raise ValueError('render colour PASS without actual path counters')
            linear_observed = bool(sum(sides[3::4])+sum(events[3::4]))
            transfers = sum(sides[index] for index in (0,4,8))+events[0]+events[4]
            if case.get('linear_transfer_observed') is not linear_observed:
                raise ValueError('linear transfer claim differs from actual counters')
            if (modes[0]+modes[2]+modes[4] < minimum or
                    policy == 1 and (modes[0] or modes[2]+modes[4] < minimum) or
                    policy == 2 and (modes[0] < minimum or linear and not linear_observed or
                                     fetch and not sides[12] or surface and not transfers)):
                raise ValueError('requested render path did not execute')
            if case_id in (15,16,19) or partial:
                seed = case.get('seed_pixels')
                if (not isinstance(seed,dict) or seed.get('pass') is not True or seed.get('pixels') != 257*129 or
                        seed.get('mismatches') != 0 or seed.get('guard_errors') != 0 or seed.get('first_mismatches') != [] or
                        not finite(seed.get('max_channel_error')) or seed['max_channel_error'] > 2):
                    raise ValueError('linear render PASS without independent store-only seed pixels')
            if surface:
                cycles = case.get('cycle_pixels')
                tolerance = 0.002 if case_id == 33 else 2
                if (case.get('reuse_cycles') != 4 or case.get('readback_source') !=
                        'IOSurface CPU mapping after GPU completion and IOSurfaceLock' or
                        not isinstance(cycles,list) or len(cycles) != 4 or
                        any(row.get('pass') is not True or row.get('pixels') != 257*129 or
                            row.get('mismatches') != 0 or row.get('guard_errors') != 0 or
                            not finite(row.get('max_channel_error')) or row['max_channel_error'] > tolerance or
                            row.get('first_mismatches') != [] for row in cycles)):
                    raise ValueError('IOSurface reuse PASS without all retired CPU pixel checks')
            if partial:
                cycles, raw = case.get('cycle_pixels'), case.get('linear_storage_pixels')
                destinations = case.get('gmem_destination_delta')
                expected_source = ('original imported host pointer plus independent transfer-buffer oracle' if imported else
                                   'raw mapped linear image plus independent transfer-buffer oracle' if linear else 'Vulkan transfer buffer')
                if (case.get('partial_render_area') is not True or case.get('reuse_cycles') != 6 or
                        case.get('imported_host_pointer') is not imported or case.get('draw_load_required') is not imported or
                        case.get('draw_transfer_verified') is not True or case.get('readback_source') != expected_source or
                        not isinstance(cycles,list) or len(cycles) != 6 or not isinstance(raw,list) or
                        len(raw) != (7 if linear else 0) or not isinstance(destinations,list) or len(destinations) != 12 or
                        any(type(value) is not int or value < 0 for value in destinations) or
                        any(not isinstance(row,dict) or row.get('pass') is not True or row.get('pixels') != 257*129 or
                            row.get('mismatches') != 0 or row.get('guard_errors') != 0 or row.get('first_mismatches') != [] or
                            not finite(row.get('max_channel_error')) or row['max_channel_error'] > 2 for row in cycles+raw)):
                    raise ValueError('partial render PASS without every independent pixel check')
                if imported and (case.get('external_handle_type') != 0x80 or
                        type(case.get('host_pointer_alignment')) is not int or
                        not 4096 <= case['host_pointer_alignment'] <= 1048576 or
                        case['host_pointer_alignment'] & (case['host_pointer_alignment'] - 1) or
                        type(case.get('host_allocation_bytes')) is not int or
                        not 257*129*4 <= case['host_allocation_bytes'] <= 17*1048576 or
                        case['host_allocation_bytes'] % case['host_pointer_alignment']):
                    raise ValueError('external-host render PASS without imported storage metadata')
                if policy == 2 and (destinations[0]+destinations[4] < 6 or imported and
                                    (destinations[8] < 6 or sides[4] < 6 or sides[7] != sides[4])):
                    raise ValueError('partial GMEM render PASS without every required draw load/store')
        if case['category'] == 'performance':
            if 'samples' in case:
                case['evaluated'] = performance(case)
                case['pass'] = case['evaluated']['pass_']
            elif case['pass']:
                raise ValueError('performance PASS without complete measured samples')
        if case['category'] == 'colour' and case['pass']:
            if (case.get('pixels') != 257*129 or case.get('mismatches') != 0 or case.get('guard_errors') != 0 or
                    not finite(case.get('max_channel_error')) or
                    case['max_channel_error'] > (0.002 if 'fp16' in case['name'] else 2) or
                    not isinstance(case.get('first_mismatches'),list) or case['first_mismatches']):
                raise ValueError('colour PASS without complete pixels/guards')
        if case['category'] == 'synchronization' and case['pass'] and (case.get('words'), case.get('mismatches')) != (4096, 0):
            raise ValueError('synchronization PASS without complete CPU acquire')
        if case['category'] == 'lifetime' and case['pass'] and (case.get('cycles'), case.get('mismatches')) != (64, 0):
            raise ValueError('lifetime PASS without 64 retired map/release cycles')
        if case['category'] == 'ui' and case['pass']:
            hashes = case.get('content_hashes')
            minimum = 2 if case_id == 62 else 3
            if (not isinstance(hashes, list) or not 1 <= len(hashes) <= 256 or
                    any(type(value) is not int or not 0 <= value < 1 << 64 for value in hashes) or
                    case.get('distinct_content') != len(set(hashes)) or len(set(hashes)) < minimum or
                    case.get('minimum_distinct_content') != minimum or
                    type(case.get('accepted_events')) is not int or case['accepted_events'] <= 0 or
                    case.get('refused_events') != 0 or case_id == 61 and case.get('foreground_verified') is not True):
                raise ValueError('UI PASS without acknowledged input and verified changed content')
            case['animation_observation'] = ui_animation(case)
    if (set(begins) != set(cases) or set(ends) != set(cases)) and not allow_partial:
        raise ValueError('unfinished GPU case')
    expected_debug = ('nocb', 'sysmem,nocb', 'gmem,nocb')[policy]
    seen_apis = set()
    for device in devices:
        api = device.get('api')
        if api not in ('vulkan', 'metal') or api in seen_apis or device.get('tu_debug') != expected_debug:
            raise ValueError('duplicate/foreign GPU backend or test policy was not applied')
        seen_apis.add(api)
        if api == 'vulkan' and (device.get('vendor') != 0x5143 or device.get('chip_id') != 0x07030002 or device.get('software_renderer') is not False):
            raise ValueError('Vulkan result did not use the actual Adreno 730v3')
        if api == 'metal' and ('DitingMetalBridge' not in device.get('implementation', '') or
                device.get('class') != 'NativeBridgeMetalDevice' or device.get('single_sample_override') != '0'):
            raise ValueError('Metal result did not use the actual Diting frontend')
    for case_id, api in ((14, 'vulkan'), (30, 'metal')):
        if cases.get(case_id, {}).get('pass') and api not in seen_apis:
            raise ValueError('GPU prerequisite PASS without backend identity')
    missing = sorted(set(expected)-set(cases))
    complete = not missing and not fatals and summary is not None and not tail and active is None and ended is None
    return dict(manifest=manifest, summary=summary, cases=list(evaluated_cases.values()),
                expected_cases=len(expected), missing_cases=missing, fatal_errors=fatals, devices=devices,
                unfinished_cases=sorted(set(begins)-set(cases)), truncated_tail_bytes=tail,
                functional_complete=complete, device_failures=device_failed,
                pass_=complete and not any(not case['pass'] for case in evaluated_cases.values()) and summary['pass'])


def display_rates(snapshots):
    # INT_COUNT_VAL wraps/resets during autorefresh and kickoff. Subtracting
    # its high word, especially across short snapshots, does not measure TE Hz.
    counters = []
    for snapshot in snapshots:
        if snapshot['header'].get('status')!='PASS':continue
        for one in snapshot['interfaces']:
            counters.append(dict(interface=one['index'],rd_ptr_frame_counter_raw=one['rd_ptr_frame_counter_raw'],
                                 autorefresh_enabled=one['autorefresh_enabled'],
                                 autorefresh_frame_count=one['autorefresh_frame_count'],
                                 external_te_selected=one['external_te_selected']))
    return dict(status='UNAVAILABLE',external_te_median_hz=None,read_pointer_counters=counters,
                physical_composed_frames=None,source='uncalibrated counters',
                scope='resettable read-pointer counters retained as raw configuration; no TE Hz or composed-frame proof')


def display_timestamp_cadence(records):
    """Calibrate the enabled panel VSYNC timestamp against a long CPU window.

    Only adjacent reads less than one 120 Hz period apart may contribute a
    timestamp edge. Sparse/reset read-pointer counts never qualify cadence.
    Timestamp cadence remains separate from new composed-frame presentation.
    """
    result=dict(status='UNAVAILABLE',source='panel_vsync_timestamp_calibrated',
                panel_timestamp_median_hz=None,physical_composed_frames=None,
                scope='enabled panel VSYNC timestamp edges with empirically calibrated raw units; no composed-frame proof')
    points=[(r.us,r.values[4]) for r in records if r.event==26 and r.values[1]==0 and
            r.values[7]&1 and r.values[9]&1 and r.values[4]]
    segments=[[]]
    for point in points:
        if segments[-1] and (point[0]<=segments[-1][-1][0] or point[1]<segments[-1][-1][1]):segments.append([])
        segments[-1].append(point)
    result['timestamp_resets']=len(segments)-1
    longest=max(segments,key=lambda rows:rows[-1][0]-rows[0][0] if rows else 0)
    if len(longest)<32 or longest[-1][0]-longest[0][0]<2000000:
        result['reason']='insufficient stable timestamp calibration window';return result
    mean_t=sum(p[0] for p in longest)/len(longest);mean_v=sum(p[1] for p in longest)/len(longest)
    denominator=sum((t-mean_t)**2 for t,_ in longest)
    slope=sum((t-mean_t)*(v-mean_v) for t,v in longest)/denominator
    if not 1<=slope<=1000:
        result['reason']='timestamp clock unavailable or outside measurable range';return result
    residuals=[abs((v-mean_v)/slope-(t-mean_t)) for t,v in longest]
    scales=[]
    for part in (longest[:len(longest)//2],longest[len(longest)//2:]):
        elapsed=part[-1][0]-part[0][0]
        if elapsed<1000000:
            result['reason']='timestamp calibration halves too short';return result
        scales.append((part[-1][1]-part[0][1])/elapsed)
    result['calibration']=dict(samples=len(longest),window_start_us=longest[0][0],window_end_us=longest[-1][0],
                               observed_raw_clock_hz=slope*1e6,residual_p95_us=percentile(residuals),
                               half_window_clock_hz=[value*1e6 for value in scales])
    if percentile(residuals)>20000 or max(scales)-min(scales)>slope*.02:
        result['reason']='timestamp calibration drift or jitter exceeds the bound';return result
    edges=[dict(cpu_before_us=t1,cpu_after_us=t2,raw_delta=v2-v1,period_ms=(v2-v1)/slope/1000)
           for (t1,v1),(t2,v2) in zip(longest,longest[1:]) if 0<t2-t1<=1e6/120 and v2>v1]
    result['edges']=edges
    if len(edges)<32 or edges[-1]['cpu_after_us']-edges[0]['cpu_before_us']<2000000:
        result['reason']='insufficient closely sampled timestamp edges';return result
    periods=[edge['period_ms'] for edge in edges]
    median=percentile(periods,.5);rate=1000/median
    result.update(status='BELOW_TARGET' if rate<110 else 'TARGET_RANGE' if rate<=130 else 'OUT_OF_RANGE',
                  panel_timestamp_median_hz=rate,period_p50_ms=median,period_p95_ms=percentile(periods),
                  edge_count=len(edges))
    return result


def correlate_trace(output, report):
    """Join fixed worker windows to real kernel work; never use CP ticks as CPU time."""
    output = Path(output)
    raw = (output/'trace.bin').read_bytes()
    decoded = TRACE.analyze(raw,120,stream=True)
    (output/'decoded-trace.json').write_text(json.dumps(decoded,indent=2,allow_nan=False)+'\n')
    _, records = TRACE.parse(raw)
    report.setdefault('display',{})['timestamp_cadence']=display_timestamp_cadence(records)
    report['display'].update({key:report['display']['timestamp_cadence'][key]
                             for key in ('status','source','panel_timestamp_median_hz')})
    report['trace_analysis_file'] = 'decoded-trace.json'
    report['gpu_recoveries'] = sum(row.event==13 for row in records)
    report['scenario_trace_incomplete'] = []
    processes = decoded['processes']
    for job in report.get('jobs',[]):
        result = job.get('assessment')
        if not result:
            continue
        for case in result['cases']:
            marker = [row for row in records if row.event==TRACE.TEST_CASE_EVENT and row.values[0]==job['job'] and row.values[1]==case['id'] and row.values[2] in (1,2)]
            by_phase = {phase:[row for row in marker if row.values[2]==phase] for phase in (1,2)}
            marker_ok = len(marker)==2 and all(len(rows)==1 for rows in by_phase.values())
            if marker_ok:
                begin,end = by_phase[1][0],by_phase[2][0]
                marker_ok = (begin.pid==end.pid==job['terminal']['daemon'] and begin.sequence<end.sequence and
                             begin.values[3:5]==(case['start_ns'],0) and
                             end.values[3:5]==(case['start_ns'],case['end_ns']) and
                             all(0 <= row.us-stamp//1000 <= 5000000 for row,stamp in
                                 ((begin,case['start_ns']),(end,case['end_ns']))))
            first,last = case['start_ns']/1000,case['end_ns']/1000
            pid = result['manifest']['pid']
            events = [row for row in records if first <= row.us <= last and
                      (case['category']=='ui' or row.pid==pid)]
            submits = [row for row in events if row.event==10 and row.values[8]>=first]
            retires = [row for row in events if row.event==11 and first<=row.values[3]<=last]
            per_pid = {}
            for row in retires:
                entry = per_pid.setdefault(row.pid,dict(total=[],gpu=[]))
                entry['total'].append((row.us-row.values[3])/1000)
                if row.values[6] and row.values[4] and row.values[5]>=row.values[4]:
                    entry['gpu'].append((row.values[5]-row.values[4])/19200)
            copies = ([copy for copy in decoded['display']['copies'] if first<=copy['us']<=last]
                      if case['category']=='ui' else [])
            case['kernel_trace'] = dict(
                marker_status='PASS' if marker_ok else 'INCOMPLETE',cpu_clock='mach absolute nanoseconds / 1000',
                window_start_us=first,window_end_us=last,worker_pid=pid,
                submissions=len(submits),retirements=len(retires),
                by_pid={str(pid):dict(name=processes.get(str(pid)),
                                     submission_to_cpu_retire=TRACE.distribution(rows['total'],1000/120),
                                     cp_execution=TRACE.distribution(rows['gpu'],1000/120))
                        for pid,rows in per_pid.items()},
                completed_gpu_copies=len(copies),
                copy_completion_intervals=TRACE.distribution([(b['us']-a['us'])/1000 for a,b in zip(copies,copies[1:])],1000/120),
                offer_to_copy_complete=TRACE.distribution([copy['copy_latency_ms'] for copy in copies
                                                          if copy['copy_latency_ms'] is not None and
                                                          copy['offer_us']>=first],1000/120),
                scope='UI includes interleaved system GPU clients; other cases select the actual worker PID; no panel latch')
            if not marker_ok:
                report['scenario_trace_incomplete'].append(f"{job['policy']}/{case['name']}: kernel case markers unavailable or inconsistent")
            if case['pass'] and case['category'] in ('colour','synchronization','lifetime','performance') and not retires:
                report['scenario_trace_incomplete'].append(f"{job['policy']}/{case['name']}: no retired GPU workload retained")
            if 'animation_observation' in case:
                name=f"ui-frames-{job['job']}-{case['id']}.csv"
                frames=UI_FRAME.iter_unpack(base64.b64decode(case['frame_samples_base64'],validate=True))
                lines=['uptime_ns,read_us,content_hash']+[f"{case['frame_sample_start_ns']+at*1000},{read},{hash_:016x}" for at,read,hash_ in frames]
                (output/name).write_text('\n'.join(lines)+'\n')
                case['animation_observation']['samples_file']=name
                if case.get('observation_schema')==2:
                    name=f"ui-inputs-{job['job']}-{case['id']}.csv"
                    stamps=[at for at, in UI_INPUT.iter_unpack(base64.b64decode(case['input_samples_base64'],validate=True))]
                    (output/name).write_text('motion,step,uptime_ns\n'+'\n'.join(
                        f"{i//43},{i%43},{case['frame_sample_start_ns']+at*1000}" for i,at in enumerate(stamps))+'\n')
                    case['animation_observation']['inputs_file']=name
    return report


class Collector:
    def __init__(self, session, output, command_timeout=8):
        self.session, self.output, self.command_timeout = session, Path(output), command_timeout
        self.raw = (self.output/'usb.raw.log').open('xb')
        try:
            self.trace = (self.output/'trace.bin').open('xb')
        except BaseException:
            self.raw.close()
            raise
        self.trace.write(bytes(TRACE_HEADER.size))
        self.cursor = self.records = self.skipped = self.missing = self.dropped = self.commands = self.raw_bytes = 0
        self.display = []
        self.jobs = []
        self.initial_dropped = 0
        self.issued = 0

    def command(self, command):
        try:
            response = self.session.command(command,self.command_timeout)
            return response
        finally:
            received=bytes(self.session.received)
            if self.raw_bytes+len(received)>64*1048576:
                raise RuntimeError('USB raw log bound reached; capture incomplete')
            self.raw.write(received);self.raw_bytes+=len(received);self.commands+=1

    def trace_chunk(self):
        response=self.command(f'trace {self.cursor} 20')
        chunk,records=STREAM.decode_chunk(response,self.cursor,20)
        if self.records+chunk['count']>131072:
            raise RuntimeError('GPU trace bound reached; capture incomplete')
        self.trace.write(records);self.cursor=chunk['next'];self.records+=chunk['count']
        self.skipped+=chunk['skipped'];self.missing+=chunk['missing'];self.dropped=chunk['dropped']
        self.issued=chunk['issued']
        return chunk['next']==chunk['issued']

    def drain_trace(self):
        self.trace_chunk()
        watermark=self.issued;deadline=time.monotonic()+30
        while self.cursor<watermark:
            if time.monotonic()>=deadline:
                raise TimeoutError('GPU trace did not reach the terminal workload watermark')
            self.trace_chunk()

    def sample_display(self):
        response=self.command('display')
        decoded=DISPLAY.decode(response.decode('ascii'))
        self.display.append(decoded)
        (self.output/f'display-{len(self.display):03}.raw.log').write_bytes(response)

    def run_job(self, profile, policy):
        before=status(self.command('gputest status'),'gputest status')
        if before['state'] in (1,2) or before['quiescing']:
            raise RuntimeError('another GPU job is active; preserving its result')
        if not before['daemon'] or before['heartbeat_age_us']>=5000000:
            raise RuntimeError('native GPU automation daemon is not ready')
        command=f'gputest run {profile} {policy}'
        running=status(self.command(command),command)
        if running['error'] or running['state'] not in (1,2) or not running['job'] or running['job']==before['job']:
            raise RuntimeError('device refused or did not create a fresh GPU job: '+str(running))
        if (running['profile'], running['policy'], running['timeout_ms']) != (PROFILES[profile], POLICIES[policy], CATALOG['timeout_ms']):
            raise RuntimeError('device started an unexpected GPU profile/policy/deadline')
        job=running['job'];deadline=time.monotonic()+135
        receipt=dict(job=job,profile=profile,policy=policy,start=running,state='RUNNING',result_bytes=0)
        self.jobs.append(receipt)
        next_status=next_display=time.monotonic()
        try:
            while time.monotonic()<deadline:
                caught_up=self.trace_chunk()
                now=time.monotonic()
                if now>=next_status:
                    previous=running;running=status(self.command('gputest status'),'gputest status')
                    if (running['job']!=job or running['bytes']<previous['bytes'] or
                            any(running[key]!=previous[key] for key in ('profile','policy','created_us','timeout_ms'))):
                        raise RuntimeError('GPU job changed during collection')
                    next_status=now+.2
                    if running['state'] not in (1,2) and not running['quiescing']:
                        if running['state'] == 0:raise RuntimeError('active GPU job returned to IDLE without a terminal result')
                        break
                if now>=next_display:
                    self.sample_display();next_display=now+1
                if caught_up:
                    time.sleep(.005)
            else:
                raise TimeoutError('GPU job did not finish within the host deadline')
            receipt.update(state=STATES[running['state']],terminal=running)
            raw=bytearray()
            path=self.output/f'{policy}-{profile}-{job}.ndjson'
            receipt['result_file']=path.name
            with path.open('xb') as stream:
                while len(raw)<running['bytes']:
                    count=min(1024,running['bytes']-len(raw))
                    request=f'gputest result {job} {len(raw)} {count}'
                    block=result_chunk(self.command(request),request,job,len(raw),count,running['bytes'])
                    raw.extend(block);stream.write(block);receipt['result_bytes']=len(raw)
                    self.trace_chunk()
            if raw:
                receipt['assessment']=decode_result(raw,job,PROFILES[profile],POLICIES[policy],
                                                    allow_partial=running['state'] != 3)
                if not receipt['assessment']['functional_complete']:
                    receipt['error']='worker result incomplete; complete records retained'
            else:
                receipt['error']='worker returned no structured result'
            self.sample_display()
            response=self.command('gpu')
            filename=f'{policy}-{profile}-{job}-gpu.raw.log'
            (self.output/filename).write_bytes(response);receipt['gpu_snapshot_file']=filename
            return receipt
        except BaseException as error:
            receipt.update(state='ERROR',error=f'{type(error).__name__}: {error}')
            # Only cancel the job created by this invocation. A busy preflight
            # never changes somebody else's job.
            try:
                cancel=f'gputest cancel {job}'
                receipt['cancel_response']=body(self.command(cancel),cancel).decode('ascii')
                until=time.monotonic()+15
                while time.monotonic()<until:
                    final=status(self.command('gputest status'),'gputest status')
                    if final['job'] != job:
                        raise RuntimeError('GPU job replaced before cancellation was confirmed')
                    if final['state'] not in (1,2) and not final['quiescing']:
                        receipt['cancel_terminal']=final
                        break
                    time.sleep(.1)
                else:
                    receipt['cancel_error']='GPU child exit remains unconfirmed; new work refused'
            except Exception as cancel_error:
                receipt['cancel_error']=str(cancel_error)
            raise

    def close(self):
        self.trace.seek(0)
        self.trace.write(TRACE_HEADER.pack(0x31545047,1,TRACE_RECORD.size,self.records,self.cursor,self.dropped))
        self.trace.close();self.raw.close()

    def report(self):
        return dict(commands=self.commands,raw_bytes=self.raw_bytes,records=self.records,
                    skipped_positions=self.skipped,missing_positions=self.missing,
                    kernel_dropped=max(0,self.dropped-self.initial_dropped), initial_kernel_dropped=self.initial_dropped,
                    last_inspected_sequence=self.cursor,display=display_rates(self.display),
                    jobs=self.jobs,physical_present='UNQUALIFIED',hardware_performance_pass=False)


def assess(report):
    failed,incomplete=[],[]
    for job in report.get('jobs',[]):
        label=job['policy']+'/'+job['profile']
        if job.get('error'):incomplete.append(label+': '+job['error'])
        if job.get('terminal',{}).get('state') in (4,5,6,7):failed.append(label+': '+job['state'])
        result=job.get('assessment',{})
        if result:
            if result['missing_cases']:incomplete.append(label+': missing cases '+str(result['missing_cases']))
            if result.get('summary') is None:incomplete.append(label+': no terminal worker summary')
            if result.get('truncated_tail_bytes'):incomplete.append(label+': truncated final worker record')
            for case in result['cases']:
                if not case['pass']:failed.append(label+': '+case['name'])
            for fatal in result['fatal_errors']:failed.append(label+': '+str(fatal.get('stage','fatal')))
    if report.get('missing_positions') or report.get('skipped_positions') or report.get('kernel_dropped'):
        incomplete.append('GPU trace coverage lost')
    incomplete.extend(report.get('scenario_trace_incomplete',[]))
    if report.get('gpu_recoveries'):
        failed.append('GPU recovery occurred during automated workloads')
    for job in report.get('jobs',[]):
        for case in job.get('assessment',{}).get('cases',[]):
            animation=case.get('animation_observation',{})
            if animation.get('status')=='FAIL':
                failed.append(job['policy']+'/'+case['name']+': continuous 120 Hz motion content not observed')
            if animation.get('status')=='INCOMPLETE':
                incomplete.append(job['policy']+'/'+case['name']+': framebuffer observer could not maintain the requested sampling rate')
    performance_required=any(job['profile'] in ('all','performance','ui') for job in report.get('jobs',[]))
    if performance_required:
        if report.get('display',{}).get('status')=='BELOW_TARGET':
            failed.append('calibrated panel VSYNC timestamp cadence below 120 Hz target')
        incomplete.append('physical composed-frame presentation remains unqualified')
    if not report.get('jobs'):incomplete.append('no GPU job executed')
    if report.get('infrastructure_error'):failed.append('test infrastructure failed')
    report.update(assessment='FAIL' if failed else 'INCOMPLETE' if incomplete else 'PASS',
                  failures=failed,incomplete=incomplete,hardware_performance_pass=False)
    return report


def write_reports(output, report):
    output=Path(output)
    (output/'report.json').write_text(json.dumps(report,indent=2,ensure_ascii=False,allow_nan=False)+'\n')
    lines=['# Diting USB GPU automated result','',f"Assessment: **{report['assessment']}**; target **120 fps / 8.333 ms**.",
           '',f"Trace: {report.get('records',0)} records, missing {report.get('missing_positions',0)}, skipped {report.get('skipped_positions',0)}, kernel drops {report.get('kernel_dropped',0)}.",
           '', '| Policy/profile | Passed cases | Missing cases | Worker |','| --- | ---: | ---: | --- |']
    suite=ET.Element('testsuite',name='diting-usb-gpu')
    tests=failures=errors=skipped=0
    for job in report.get('jobs',[]):
        result=job.get('assessment',{});cases=result.get('cases',[])
        lines.append(f"| {job['policy']}/{job['profile']} | {sum(case['pass'] for case in cases)}/{result.get('expected_cases',0)} | {len(result.get('missing_cases',[]))} | {job['state']} |")
        for case in cases:
            tests+=1
            node=ET.SubElement(suite,'testcase',name=case['name'],classname='gpu.'+job['policy'],
                               time=str(case.get('duration_us',0)/1e6))
            if not case['pass']:
                failures+=1;ET.SubElement(node,'failure',message=case.get('stage','GPU test failed')).text=json.dumps(case,ensure_ascii=False)
        for case_id in result.get('missing_cases',[]):
            tests+=1;skipped+=1;node=ET.SubElement(suite,'testcase',name='missing-case-'+str(case_id),classname='gpu.'+job['policy'])
            ET.SubElement(node,'skipped',message='not executed; prerequisite or worker failure')
        if job.get('error'):
            tests+=1;failures+=1;node=ET.SubElement(suite,'testcase',name='job-result',classname='gpu.'+job['policy'])
            ET.SubElement(node,'failure',message=job['error'],type='IncompleteWorkerResult')
    lines+=['','| Policy | Workload | CPU completion p95 | GPU p95 | Result |','| --- | --- | ---: | ---: | --- |']
    for job in report.get('jobs',[]):
        for case in job.get('assessment',{}).get('cases',[]):
            timing=case.get('evaluated')
            if timing:
                gpu='unavailable' if timing['gpu_p95_ms'] is None else f"{timing['gpu_p95_ms']:.3f} ms"
                lines.append(f"| {job['policy']} | {case['name']} | {timing['p95_ms']:.3f} ms | {gpu} | {'PASS' if case['pass'] else 'FAIL'} |")
    display=report.get('display',{});rate=display.get('panel_timestamp_median_hz')
    lines+=['','| UI scene | Animation observations | Lowest observed change rate | Longest unchanged motion |','| --- | --- | ---: | ---: |']
    for job in report.get('jobs',[]):
        for case in job.get('assessment',{}).get('cases',[]):
            animation=case.get('animation_observation')
            if animation:
                motions=animation['motions']
                lines.append(f"| {case['name']} | {animation['status']} | {min(row['observed_change_hz'] for row in motions):.1f} Hz | {max(row['longest_unchanged_ms'] for row in motions):.3f} ms |")
    lines+=['',f"Calibrated panel VSYNC timestamp cadence: {'unavailable' if rate is None else f'{rate:.3f} Hz'} ({display.get('status','UNAVAILABLE')}).",
            '', 'Colour checks compare GPU readback pixels with independent CPU values. UI checks use acknowledged HID and changed pre-DPU framebuffer content.',
            'UI motion records every input step at 120 Hz; late sampling is incomplete evidence. Content change rates are observations, not physical FPS.',
            'GPU completion, software refresh and TE counts do not certify physical composed-frame fps.']
    if report.get('failures'):
        lines+=['','Failures:','']+['- '+item for item in report['failures']]
    if report.get('incomplete'):
        lines+=['','Incomplete evidence:','']+['- '+item for item in report['incomplete']]
    if report.get('infrastructure_error'):
        message=report['infrastructure_error'];lines+=['','Infrastructure error: '+message]
        tests+=1;errors+=1;node=ET.SubElement(suite,'testcase',name='infrastructure',classname='gpu.transport')
        ET.SubElement(node,'error',message=message,type='InfrastructureError')
    # CI must also see report-level failures/incomplete evidence.
    for label in ('failures','incomplete'):
        for message in report.get(label,[]):
            tests+=1;node=ET.SubElement(suite,'testcase',name=message,classname='gpu.assessment')
            failures+=1;ET.SubElement(node,'failure',message=message,
                                     type='TestFailure' if label=='failures' else 'IncompleteEvidence')
    suite.set('tests',str(tests));suite.set('failures',str(failures));suite.set('errors',str(errors));suite.set('skipped',str(skipped))
    ET.indent(suite);ET.ElementTree(suite).write(output/'junit.xml',encoding='utf-8',xml_declaration=True)
    (output/'report.md').write_text('\n'.join(lines)+'\n')
