#!/usr/bin/env python3
"""Decode complete bounded Diting display replies; never certify composed fps."""
import argparse
import json
from pathlib import Path


def fields(line):
    result = {}
    for word in line.split()[1:]:
        if '=' in word:
            key, value = word.split('=', 1)
            if key in result:
                raise ValueError('Duplicate display field: ' + key)
            result[key] = value
    return result


def decode(text):
    begin, end = 'DitingUSB BEGIN display', 'DitingUSB END display'
    lines = text.splitlines()
    starts = [i for i, line in enumerate(lines) if line == begin]
    if len(starts) != 1 or end not in lines[starts[0] + 1:]:
        raise ValueError('Missing or ambiguous display BEGIN/END frame')
    body = lines[starts[0] + 1:lines.index(end, starts[0] + 1)]
    heads = [fields(line) for line in body if line.startswith('DISPLAY ')]
    if len(heads) != 1 or heads[0].get('schema') != '1' or heads[0].get('physical_present') != 'UNQUALIFIED':
        raise ValueError('Unsupported or missing display schema')
    head = heads[0]
    report = dict(header=head, interfaces=[], pipes=[], controls=[], clocks={}, dpu={},
                  hardware_performance_pass=False, physical_presented_frame_count=None, timestamp_units='RAW')
    for line in body:
        prefix = line.split(' ', 1)[0]
        data = fields(line)
        if prefix == 'CLOCK': report['clocks'] = data
        elif prefix == 'DPU': report['dpu'] = data
        elif prefix == 'SSPP': report['pipes'].append(data)
        elif prefix == 'CTL': report['controls'].append(data)
        elif prefix == 'INTF':
            data['timestamp_available'] = int(data['TS_STABLE'])
            data['frame_counter_enabled'] = bool(int(data['COUNT_EN'], 16) & 1)
            data['tearcheck_enabled'] = bool(int(data['TE_EN'], 16) & 1)
            data['vsync_counter_enabled'] = bool(int(data['TE_CFG'], 16) & 0x80000)
            data['external_te_selected'] = bool(int(data['TE_CFG'], 16) & 0x100000)
            # NothingOSS sde_hw_intf_get_vsync_info: INT_COUNT_VAL is a
            # resettable read-pointer frame/line count, not a lifetime TE count.
            data['rd_ptr_frame_counter_raw'] = int(data['TE_COUNT'], 16) >> 16
            data['rd_ptr_line_counter_raw'] = int(data['TE_COUNT'], 16) & 0xffff
            data['autorefresh_enabled'] = bool(int(data['AUTOREFRESH'], 16) & 0x80000000)
            data['autorefresh_frame_count'] = int(data['AUTOREFRESH'], 16) & 0xffff
            report['interfaces'].append(data)
    if head.get('status') in ('PASS', 'CHANGED'):
        if not (len(report['interfaces']) == 2 and len(report['pipes']) == 8 and len(report['controls']) == 6):
            raise ValueError('Truncated complete display snapshot')
        if head.get('timestamp_units') != 'RAW' or int(head['reads']) > 256:
            raise ValueError('Unsupported units or unbounded reader')
        start, finish = int(head['cpu_start_us']), int(head['cpu_end_us'])
        if finish < start:
            raise ValueError('Reversed capture time')
        report['capture_duration_us'] = finish - start
    report['scope'] = 'read-only DPU configuration/counters; enabled timestamps retain raw units; no physical composed-frame proof'
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, nargs='+')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    reports = []
    for path in args.input:
        report = decode(path.read_text(errors='replace'))
        report['input'] = str(path.resolve())
        reports.append(report)
    encoded = json.dumps(dict(snapshots=reports, hardware_performance_pass=False), indent=2) + '\n'
    if args.output: args.output.write_text(encoded)
    else: print(encoded, end='')


if __name__ == '__main__':
    main()
