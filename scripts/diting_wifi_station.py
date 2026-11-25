"""Apply Diting station changes to the pinned public source in build output."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def prepare_station(source: Path, output: Path) -> Path:
    output.mkdir(parents=True, exist_ok=True)
    target = output / 'nw_sta_diting.c'
    if target.is_symlink():
        target.unlink()
    target.write_bytes(source.read_bytes())
    patch = ROOT / 'drivers/DitingWifi/ath11k/station.patch'
    result = subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)],
                            cwd=output, capture_output=True, text=True)
    (output / 'station-patch.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError('Diting station source differs from the pinned ABI: '
                           + result.stdout + result.stderr)
    return target
