"""Diting's native Wi-Fi job wiring and fixed-destination boot archive.

The input is the installed 22H374 Version 7 cache, read without mounting it.
Only wifid and this board's existing native bootstrap are changed. Apple cache
contents, compiled executables and generated archives remain private inputs.
"""
from copy import deepcopy
import hashlib
import plistlib
import struct
import uuid
import zlib
from xml.parsers.expat import ExpatError

WIFID = '/System/Library/LaunchDaemons/com.apple.wifid.plist'
BOOTSTRAP_LABEL = 'dev.unixnu.diting.wifi-bootstrap'
BOOTSTRAP_JOB = '/System/Library/LaunchDaemons/' + BOOTSTRAP_LABEL + '.plist'
DESTINATIONS = ('/diting/wifi-bootstrap', BOOTSTRAP_JOB, WIFID,
                '/System/Library/xpc/launchd.plist')
HEADER_BYTES = 48
RAW_LIMITS = (1 << 20, 65536, 65536, 2 << 20, 2 << 20)
PACKED_LIMIT = 4 << 20
ENTITLEMENTS = {
    'platform-application': True,
    'com.apple.private.security.no-container': True,
    'com.apple.wifi.manager-access': True,
    'com.apple.private.corewifi.internal': True,
    'com.apple.SystemConfiguration.SCPreferences-write-access': ['preferences.plist'],
    'com.apple.SystemConfiguration.SCDynamicStore-write-access': True,
}


def native_jobs(cache_bytes, diagnostics=False):
    if not 8 <= len(cache_bytes) <= RAW_LIMITS[4] or not cache_bytes.startswith(b'bplist00'):
        raise ValueError('Bounded installed binary job cache required')
    original = plistlib.loads(cache_bytes)
    if not isinstance(original, dict) or original.get('VersionNumber') != 7 or \
            not isinstance(original.get('LaunchDaemons'), dict):
        raise ValueError('Native 22H374 Version 7 job cache required')
    cached = original['LaunchDaemons'].get(WIFID)
    if not isinstance(cached, dict) or cached.get('Label') != 'com.apple.wifid':
        raise ValueError('Actual native wifid job missing')
    program = cached.get('Program') or (cached.get('ProgramArguments') or [''])[0]
    if program != '/usr/sbin/wifid' or cached.get('Disabled') is True:
        raise ValueError('Actual enabled 22H374 wifid provider required')
    updated = deepcopy(original)
    jobs = updated['LaunchDaemons']
    services = jobs[WIFID].setdefault('MachServices', {})
    if not isinstance(services, dict):
        raise ValueError('Unexpected native wifid MachServices type')
    legacy = services.get('com.apple.wifi.manager')
    if legacy is not None and legacy is not True and not isinstance(legacy, dict):
        raise ValueError('Unexpected existing native MobileWiFi listener')
    declaration = deepcopy(legacy) if isinstance(legacy, dict) else {}
    declaration['HideUntilCheckIn'] = True
    services['com.apple.wifi.manager'] = declaration
    if diagnostics:
        environment = jobs[WIFID].setdefault('EnvironmentVariables', {})
        if not isinstance(environment, dict):
            raise ValueError('Unexpected native wifid environment type')
        environment['OS_ACTIVITY_DT_MODE'] = 'YES'
        jobs[WIFID]['StandardOutPath'] = jobs[WIFID]['StandardErrorPath'] = \
            '/private/var/logs/DitingService-wifid-wifi.log'
    bootstrap = dict(Label=BOOTSTRAP_LABEL,
        ProgramArguments=[DESTINATIONS[0]], UserName='root', RunAtLoad=True,
        LaunchOnlyOnce=True, ProcessType='Background', Nice=19, LowPriorityIO=True,
        StandardOutPath='/private/var/logs/diting-wifi-bootstrap.log',
        StandardErrorPath='/private/var/logs/diting-wifi-bootstrap.log')
    previous = jobs.get(BOOTSTRAP_JOB)
    if previous is not None and (not isinstance(previous, dict) or
        previous.get('Label') != BOOTSTRAP_LABEL or
        previous.get('ProgramArguments') != [DESTINATIONS[0]]):
        raise ValueError('Foreign bootstrap job at the fixed destination')
    jobs[BOOTSTRAP_JOB] = bootstrap
    # Check the complete cache object, including non-job tables. This protects
    # concurrent GPU/UI/USB/audio changes from an old whole-cache overlay.
    expected = deepcopy(original)
    expected['LaunchDaemons'][WIFID] = jobs[WIFID]
    expected['LaunchDaemons'][BOOTSTRAP_JOB] = bootstrap
    if updated != expected:
        raise ValueError('Non-Wi-Fi native cache mutation')
    encode = lambda value: plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=True)
    return encode(bootstrap), encode(jobs[WIFID]), encode(updated), dict(
        version=7, original_jobs=len(original['LaunchDaemons']), jobs=len(jobs),
        changed_jobs=[WIFID, BOOTSTRAP_JOB], protected_jobs=len(original['LaunchDaemons'])-1,
        protected_cache_tables=True, diagnostics=diagnostics)


def pack_runtime(executable, cache_bytes, identifier=None, diagnostics=False):
    signature = verify_bootstrap_signature(executable)
    job, wifid, updated_cache, record = native_jobs(cache_bytes, diagnostics)
    raw = (bytes(executable), job, wifid, updated_cache, bytes(cache_bytes))
    identifier = identifier or uuid.uuid4().bytes
    if len(identifier) != 16 or not any(identifier):
        raise ValueError('Nonzero 16-byte installation identity required')
    body = bytearray()
    for payload, limit in zip(raw, RAW_LIMITS):
        if not 8 <= len(payload) <= limit:
            raise ValueError('Wi-Fi runtime file exceeds its fixed bound')
        packed = zlib.compress(payload, 6)
        body.extend(struct.pack('<QQ', len(payload), len(packed)))
        body.extend(packed)
        body.extend(b'\0' * (-len(packed) % 8))
    total = HEADER_BYTES + len(body)
    if total > PACKED_LIMIT:
        raise ValueError('Wi-Fi runtime archive exceeds 4 MiB')
    header = b'DTWIFI01' + struct.pack('<4IQ16s', 1, 0x8475, 5, 0, total, identifier)
    record.update(install_id=str(uuid.UUID(bytes=identifier)), raw_bytes=list(map(len, raw)),
        packed_bytes=total, destinations=list(DESTINATIONS), compare_installed_cache=True,
        preimage_cache_bytes=len(cache_bytes), password_or_network_preferences_included=False,
        code_signature=signature, signature_page_bytes=4096, native_manager_entitlements=True)
    return header + body, record


def _bootstrap_layout(data):
    if len(data) < 32 or struct.unpack_from('<2I', data) != (0xfeedfacf, 0x100000c) or \
            struct.unpack_from('<I', data, 12)[0] != 2:
        raise ValueError('Thin arm64 bootstrap executable required')
    count, command_bytes = struct.unpack_from('<2I', data, 16)
    if count > 128 or 32 + command_bytes > len(data):
        raise ValueError('Invalid bootstrap load commands')
    cursor, signature, linkedit, text = 32, None, None, None
    for _ in range(count):
        if cursor + 8 > 32 + command_bytes:
            raise ValueError('Truncated bootstrap command')
        kind, size = struct.unpack_from('<2I', data, cursor)
        if size < 8 or size % 8 or cursor + size > 32 + command_bytes:
            raise ValueError('Invalid bootstrap command length')
        if kind == 0x1d:
            if size != 16 or signature is not None:
                raise ValueError('Unique signature command required')
            signature = cursor
        elif kind == 0x19 and size >= 72:
            name = bytes(data[cursor+8:cursor+24]).split(b'\0')[0]
            if name == b'__LINKEDIT':
                if linkedit is not None: raise ValueError('Duplicate linkedit segment')
                linkedit = cursor
            if name == b'__TEXT':
                if text is not None: raise ValueError('Duplicate text segment')
                text = cursor
        cursor += size
    if cursor != 32 + command_bytes or signature is None or linkedit is None or text is None:
        raise ValueError('Linker signature, text and linkedit required')
    code_limit, old_bytes = struct.unpack_from('<2I', data, signature + 8)
    link_offset = struct.unpack_from('<Q', data, linkedit + 40)[0]
    if not 32 + command_bytes <= link_offset <= code_limit or code_limit + old_bytes != len(data):
        raise ValueError('Signature must be the final linkedit contents')
    return signature, linkedit, text, code_limit


def sign_bootstrap(executable, identifier=BOOTSTRAP_LABEL):
    """Build a real 4 KiB ad-hoc CodeDirectory with native manager entitlements.

    Linux's codesign shim signs no files. Update our own thin executable's
    signature instead; the stock daemon, restore binaries and signing policy
    are not modified. SHA-256 below is code signing, never an image audit.
    """
    data = bytearray(executable)
    signature, linkedit, text, code_limit = _bootstrap_layout(data)
    link_offset = struct.unpack_from('<Q', data, linkedit + 40)[0]
    name = identifier.encode('ascii') + b'\0'
    if not 2 <= len(name) <= 256 or b'\0' in name[:-1]:
        raise ValueError('Bounded code-signing identifier required')
    xml = plistlib.dumps(ENTITLEMENTS, fmt=plistlib.FMT_XML, sort_keys=True)
    entitlement = struct.pack('>2I', 0xfade7171, 8+len(xml)) + xml
    slots = (code_limit + 4095) // 4096
    hash_offset = 88 + len(name) + 5*32
    cd_size = hash_offset + slots*32
    sig_size = 28 + cd_size + len(entitlement)
    struct.pack_into('<I', data, signature + 12, sig_size)
    link_bytes = code_limit + sig_size - link_offset
    struct.pack_into('<Q', data, linkedit + 48, link_bytes)
    struct.pack_into('<Q', data, linkedit + 32, (link_bytes+0x3fff) & ~0x3fff)
    text_offset, text_bytes = struct.unpack_from('<2Q', data, text + 40)
    directory = bytearray(struct.pack('>9I4B4I4Q',
        0xfade0c02, cd_size, 0x20400, 2, hash_offset, 88, 5, slots, code_limit,
        32, 2, 0, 12, 0, 0, 0, 0, 0, text_offset, text_bytes, 1))
    directory.extend(name)
    # Special slots are indexed backwards from hashOffset. Slot 5 contains
    # the actual XML entitlement blob consumed by SecTask/MobileWiFi.
    directory.extend(hashlib.sha256(entitlement).digest())
    directory.extend(b'\0' * (4*32))
    for offset in range(0, code_limit, 4096):
        directory.extend(hashlib.sha256(data[offset:min(code_limit,offset+4096)]).digest())
    superblob = struct.pack('>3I4I',0xfade0cc0,sig_size,2,0,28,5,28+cd_size)
    return bytes(data[:code_limit]) + superblob + directory + entitlement


def verify_bootstrap_signature(executable, identifier=BOOTSTRAP_LABEL):
    """Check our bootstrap's signature and entitlement slots before embedding.

    This checks code signing only. It neither hashes a boot image nor changes
    native signature policy, and cannot establish hardware Wi-Fi operation.
    """
    command, linkedit, text, limit = _bootstrap_layout(executable)
    signature_bytes = struct.unpack_from('<I', executable, command + 12)[0]
    blob = executable[limit:]
    if len(blob) < 28 or struct.unpack_from('>7I', blob)[:5] != \
            (0xfade0cc0, signature_bytes, 2, 0, 28):
        raise ValueError('Native bootstrap signature superblob required')
    kind, ent_offset = struct.unpack_from('>2I', blob, 20)
    if kind != 5 or not 116 <= ent_offset <= len(blob)-8:
        raise ValueError('Bounded XML entitlement signature slot required')
    directory, entitlement = blob[28:ent_offset], blob[ent_offset:]
    if len(directory) < 88 or len(entitlement) < 8:
        raise ValueError('Truncated CodeDirectory or entitlement blob')
    fields = struct.unpack_from('>9I4B4I4Q', directory)
    magic, size, version, flags, hashes, name_at, special, slots, code_limit = fields[:9]
    name = identifier.encode('ascii') + b'\0'
    if (magic, size, version, flags, name_at, special, slots, code_limit) != \
            (0xfade0c02, len(directory), 0x20400, 2, 88, 5, (limit+4095)//4096, limit) or \
            fields[9:17] != (32, 2, 0, 12, 0, 0, 0, 0) or fields[17] != 0 or \
            fields[18:21] != (*struct.unpack_from('<2Q', executable, text+40), 1) or \
            hashes != 88 + len(name) + 5*32 or len(directory) != hashes + slots*32 or \
            directory[88:88+len(name)] != name:
        raise ValueError('Native 4 KiB ad-hoc CodeDirectory metadata mismatch')
    link_offset, link_bytes = struct.unpack_from('<2Q', executable, linkedit+40)
    if link_offset + link_bytes != len(executable) or \
            struct.unpack_from('<Q', executable, linkedit+32)[0] != (link_bytes+0x3fff) & ~0x3fff:
        raise ValueError('Signature exceeds the declared linkedit segment')
    try:
        native_entitlements = plistlib.loads(entitlement[8:])
    except (plistlib.InvalidFileException, ExpatError, ValueError, TypeError) as error:
        raise ValueError('Malformed native Wi-Fi entitlement XML') from error
    if struct.unpack_from('>2I', entitlement) != (0xfade7171, len(entitlement)) or \
            native_entitlements != ENTITLEMENTS:
        raise ValueError('Native Wi-Fi manager entitlements missing or changed')
    if directory[hashes-5*32:hashes-4*32] != hashlib.sha256(entitlement).digest() or \
            directory[hashes-4*32:hashes] != b'\0' * (4*32):
        raise ValueError('CodeDirectory entitlement hash mismatch')
    for slot, offset in enumerate(range(0, limit, 4096)):
        if directory[hashes+slot*32:hashes+(slot+1)*32] != \
                hashlib.sha256(executable[offset:min(limit,offset+4096)]).digest():
            raise ValueError('Bootstrap code-signing page hash mismatch')
    return dict(identifier=identifier, page_bytes=4096, code_slots=slots,
                entitlement_slot=5, code_signing_checked=True, ad_hoc=True)
