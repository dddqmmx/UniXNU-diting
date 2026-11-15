"""Fixed-path, bounded boot payload for the first 22H374 GPU installation."""
import struct
import uuid
import zlib
from pathlib import Path

import gpu_runtime18

FILES = ('Info.plist', *gpu_runtime18.BINARIES, *gpu_runtime18.POLICY,
         'gpu-capture18', 'dev.unixnu.diting.gpu-capture18.plist')
PACKED_MAX = 64 << 20
RAW_MAX = 128 << 20


def pack(runtime, capture, job, install_id):
    runtime = Path(runtime).resolve()
    gpu_runtime18.load_runtime(runtime)
    paths = [*(runtime / gpu_runtime18.BUNDLE / name for name in FILES[:4]),
             *(runtime / name for name in FILES[4:6]), Path(capture), Path(job)]
    body = bytearray()
    total = 0
    for path in paths:
        if path.is_symlink() or not path.is_file():
            raise RuntimeError('ordinary GPU stage input required: ' + str(path))
        size = path.stat().st_size
        if not 0 < size <= PACKED_MAX or total + size > RAW_MAX:
            raise RuntimeError('GPU stage exceeds the bounded kernel input')
        data = path.read_bytes()
        if len(data) != size:
            raise RuntimeError('GPU stage input changed during packaging')
        compressed = zlib.compress(data, 9)
        body += struct.pack('<QQ', size, len(compressed))
        body += compressed + bytes(-len(compressed) % 8)
        total += size
    packed_bytes = 48 + len(body)
    if packed_bytes > PACKED_MAX:
        raise RuntimeError('compressed GPU stage exceeds 64 MiB')
    header = struct.pack('<8sIIIIQ', b'DTGPU001', 1, 0x07030002, len(paths), 0, packed_bytes)
    return header + uuid.UUID(str(install_id)).bytes + body
