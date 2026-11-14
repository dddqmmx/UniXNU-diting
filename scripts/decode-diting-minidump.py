#!/usr/bin/env python3
"""Decode the captured Diting OEM ten-slot minidump without image hashes.

Offsets in Raw_Dmp! section tables are absolute partition offsets. Console
DBGC cursors name the oldest byte; pmsg cursors name the next write position.
Preserve opaque TZ/HYP/CPU data and report OEM truncation explicitly.
"""
import argparse
import datetime
import json
import mmap
from pathlib import Path
import re
import struct

DBGC = 0x43474244
ZONE_BYTES = 2 * 1024 * 1024
SLOTS = 10
RAW_HEADER_BYTES = 88
SECTION_BYTES = 64


def bounded(data, offset, size, start, end):
    if offset < start or size < 0 or offset > end or size > end - offset or end > len(data):
        raise ValueError(f'Range {offset:#x}+{size:#x} is outside {start:#x}..{end:#x}')
    return bytes(data[offset:offset + size])


def decode_zone(raw, kind, zone_bytes=ZONE_BYTES):
    if len(raw) < 12 or zone_bytes <= 12:
        raise ValueError('Truncated DBGC header')
    magic, cursor, size = struct.unpack_from('<III', raw)
    capacity = zone_bytes - 12
    if magic != DBGC or cursor > capacity or size > capacity or len(raw) > zone_bytes:
        raise ValueError('Invalid DBGC signature, cursor, size or section capacity')
    if kind not in ('md_kmsg', 'md_pmsg'):
        raise ValueError('Unknown DBGC cursor semantics')
    payload = raw[12:]
    oldest = cursor % capacity if kind == 'md_kmsg' else (cursor - size) % capacity
    first = min(size, capacity - oldest)
    pieces = [(oldest, first), (0, size - first)]
    recovered, missing = [], 0
    for start, count in pieces:
        part = payload[start:start + count]
        recovered.append(part)
        missing += count - len(part)
    text = b''.join(recovered)
    return text, dict(magic=hex(magic), cursor=cursor, declared_size=size,
                      capacity=capacity, available_bytes=len(payload), oldest=oldest,
                      cursor_semantics='oldest byte' if kind == 'md_kmsg' else 'write cursor',
                      recovered_bytes=len(text), missing_bytes=missing,
                      truncated=bool(missing))


def decode_record(data, slot, span, raw_offset, provenance):
    slot_start = slot * span
    header_at = slot_start + raw_offset
    header = bounded(data, header_at, RAW_HEADER_BYTES, slot_start, min(slot_start + span, len(data)))
    if header[:8] != b'Raw_Dmp!':
        return None
    used = struct.unpack_from('<Q', header, 0x24)[0]
    count = struct.unpack_from('<I', header, 0x34)[0]
    if not 0 < count <= 256 or used < RAW_HEADER_BYTES + count * SECTION_BYTES:
        raise ValueError('Invalid Raw_Dmp! used length or section count')
    record_end = header_at + used
    if record_end > slot_start + span or record_end > len(data):
        raise ValueError('Raw_Dmp! record crosses its slot or captured partition')
    table_at = header_at + RAW_HEADER_BYTES
    table_end = table_at + count * SECTION_BYTES
    sections, intervals, names, text_parts, files = [], [], set(), [], {}
    for index in range(count):
        entry = bounded(data, table_at + index * SECTION_BYTES, SECTION_BYTES, table_at, table_end)
        fields = struct.unpack_from('<IIIQQQQ', entry)
        name = entry[44:64].split(b'\0', 1)[0].decode('ascii')
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', name) or name in names:
            raise ValueError('Invalid or duplicate section name')
        names.add(name)
        offset, size, physical, extra = fields[3:]
        raw = bounded(data, offset, size, table_end, record_end)
        if any(offset < end and begin < offset + size for begin, end in intervals):
            raise ValueError('Overlapping minidump sections')
        intervals.append((offset, offset + size))
        filename = name + '.bin'
        files[filename] = raw
        section = dict(name=name, table_index=index, fields_u32=list(fields[:3]),
                       partition_offset=offset, bytes=size, physical_address=hex(physical),
                       extra_u64=hex(extra), file=filename, text_decoded=False)
        if name in ('md_kmsg', 'md_pmsg'):
            try:
                text, zone = decode_zone(raw, name)
                section.update(zone=zone, text_decoded=True, text_file=name + '.log')
                files[name + '.log'] = text
                text_parts.append(text.decode('utf-8', errors='replace'))
            except ValueError as error:
                section['decode_error'] = str(error)
        else:
            # CPU/TZ/MDMP sections can be empty or encrypted. Neither random
            # text nor zero-filled registers establishes a panic/reset cause.
            section.update(opaque=True, all_zero=not any(raw))
        sections.append(section)
    text = '\n'.join(text_parts)
    epochs = sorted({int(v) for v in re.findall(r'\b(?:epoch=|boot-epoch\s+)(\d+)', text)})
    matched = provenance.get(str(epochs[0])) if len(epochs) == 1 else None
    record = dict(slot=slot, raw_header_offset=header_at, used_bytes=used,
                  section_count=count, raw_header_hex=header.hex(), sections=sections,
                  epochs_seen=epochs, payload_epoch=epochs[0] if len(epochs) == 1 else None,
                  provenance=matched,
                  provenance_policy='match unique payload epoch; kernel banner is insufficient',
                  cpu_first_boot=sorted({int(v) for v in re.findall(r'ArmCpu: CPU (\d+) first boot completed', text)}),
                  all_registered_cpus_available='ArmCpu: all registered CPUs available' in text,
                  data_mount_pass='DITING APFS: Data mount PASS' in text,
                  launchd_ios_lines=[line for line in text.splitlines() if '/sbin/launchd.ios' in line][:12],
                  panic_text_observed=bool(re.search(r'\bpanic\(cpu|\bpanic:', text, re.IGNORECASE)),
                  panic_absence_is_not_reset_evidence=True)
    return record, files


def decode(data, provenance=None):
    if len(data) < 32 or bytes(data[:8]) != b'cigamgol':
        raise ValueError('Unsupported Diting minidump ring header')
    cursor, word12, raw_offset, span = struct.unpack_from('<IIQQ', data, 8)
    # These are the measured Diting format constants, not generic Qualcomm
    # guesses. word12=10 is not a cumulative boot counter.
    if word12 != SLOTS or raw_offset != 0x400 or span != 0x600000 or cursor >= SLOTS:
        raise ValueError('Captured ring geometry/cursor differs from the measured Diting format')
    if len(data) < SLOTS * span:
        raise ValueError('Capture is shorter than the complete ten-slot ring')
    records, invalid, files = [], [], {}
    for slot in range(SLOTS):
        try:
            decoded = decode_record(data, slot, span, raw_offset, provenance or {})
            if decoded:
                record, outputs = decoded
                records.append(record)
                files[slot] = outputs
        except (ValueError, UnicodeError, struct.error) as error:
            invalid.append(dict(slot=slot, error=str(error)))
    order = list(range(cursor, SLOTS)) + list(range(cursor))
    records.sort(key=lambda record: order.index(record['slot']))
    if not records:
        raise ValueError('No valid minidump record found')
    result = dict(format='diting-oem-minidump-ring-v1', captured_bytes=len(data),
                  geometry=dict(slots=SLOTS, word12=word12, raw_header_offset=raw_offset,
                                slot_span=span, cursor_word8=cursor,
                                trailing_partition_bytes=len(data) - SLOTS * span),
                  order_policy='word8 interpreted as next write cursor from observed 0 -> 3 transition; epochs remain authoritative',
                  assumed_oldest_to_newest_slots=[record['slot'] for record in records],
                  invalid_records=invalid, records=records,
                  status='PARTIAL' if invalid or any('decode_error' in s for r in records for s in r['sections']) else 'PASS')
    return result, files


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--provenance', type=Path, help='JSON mapping of payload epochs to candidate/build records')
    args = parser.parse_args(argv)
    provenance = json.loads(args.provenance.read_text()) if args.provenance else {}
    with args.input.open('rb') as source:
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            result, files = decode(data, provenance)
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for slot, sections in files.items():
        target = out / f'slot{slot:02d}'
        target.mkdir()
        for name, raw in sections.items():
            (target / name).write_bytes(raw)
    result.update(input=str(args.input.resolve()), provenance_input=str(args.provenance.resolve()) if args.provenance else None,
                  decoded_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    (out / 'records.json').write_text(json.dumps(result, indent=2) + '\n')
    for record in result['records']:
        print(f"slot {record['slot']:02d} epoch={record['payload_epoch']} CPUs={record['cpu_first_boot']} Data={record['data_mount_pass']} panic_text={record['panic_text_observed']}")
    print(out)
    if result['status'] != 'PASS':
        raise SystemExit(2)


if __name__ == '__main__':
    main()
