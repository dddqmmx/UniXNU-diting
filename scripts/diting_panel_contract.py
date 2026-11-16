"""Keep Xiaomi DT panel selection separate from the panel's DCS replies."""
import importlib.util
import os
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
MODELS = {
    'qcom,mdss_dsi_l12_36_02_0b_dsc_cmd': (0x004C3132, 0x00360200),
    'qcom,mdss_dsi_l12_42_02_0a_dsc_cmd': (0x004C3132, 0x00420200),
}


def selection(nodes):
    chosen = nodes.get('/chosen', {}).get('bootargs', b'').rstrip(b'\0').decode('ascii')
    selectors = [arg.split('=', 1)[1].split(':', 1)[0] for arg in chosen.split()
                 if arg.startswith('msm_drm.dsi_display0=')]
    if len(selectors) != 1 or selectors[0] not in MODELS:
        raise ValueError('Require one retained Diting L12 DSI0 panel selection')
    name = selectors[0]
    candidates = [props for path, props in nodes.items() if path.rsplit('/', 1)[-1] == name]
    if len(candidates) != 1:
        raise ValueError('Selected L12 panel node is missing or ambiguous')
    panel = candidates[0]
    expected = MODELS[name]
    if panel.get('mi,panel-id') != struct.pack('>2I', *expected):
        raise ValueError('Xiaomi mi,panel-id does not match the selected L12 model')
    for key, value in {
        'qcom,mdss-dsi-panel-type': b'dsi_cmd_mode\0',
        'qcom,mdss-dsi-color-order': b'rgb_swap_rgb\0',
        'qcom,mdss-dsi-bpp': struct.pack('>I', 30),
        'qcom,dsi-ctrl-num': bytes(4),
        'qcom,dsi-phy-num': bytes(4),
        'qcom,mdss-dsi-stream': bytes(4),
        'qcom,mdss-dsi-virtual-channel-id': bytes(4),
    }.items():
        if panel.get(key) != value:
            raise ValueError('Selected L12 panel violates the Diting DSI0/RGB contract: ' + key)
    panel_path = next(path for path, props in nodes.items() if props is panel)
    prefix = panel_path + '/qcom,mdss-dsi-display-timings/timing@'
    timings = [props for path, props in nodes.items() if path.startswith(prefix)
               and '/' not in path[len(prefix):]]
    rates = []
    for timing in timings:
        if (timing.get('qcom,mdss-dsi-panel-width') != struct.pack('>I', 1220) or
                timing.get('qcom,mdss-dsi-panel-height') != struct.pack('>I', 2712)):
            raise ValueError('Selected L12 geometry differs from the retained scanout')
        rate = timing.get('qcom,mdss-dsi-panel-framerate', b'')
        if len(rate) != 4:
            raise ValueError('L12 timing lacks its frame rate')
        rates.append(int.from_bytes(rate, 'big'))
    if sorted(rates) != [60, 90, 120]:
        raise ValueError('Require the three retained L12 refresh timing nodes')
    return dict(name=name, model=list(expected), width=1220, height=2712, rgb=1, version=1)


def adt_panel_properties(source=None):
    source = source or os.environ.get('DITING_LIVE_DTB')
    if not source:
        # An optional panel mode probe may collect DCS evidence, but cannot
        # select a vendor timing stream without the retained board input.
        return []
    spec = importlib.util.spec_from_file_location('diting_panel_fdt', ROOT / 'scripts/prepare-diting-wifi.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    selected = selection(module.fdt_nodes(Path(source).read_bytes()))
    return [('diting-dsi-panel', struct.pack('<6I', selected['version'], *selected['model'],
                                           selected['width'], selected['height'], selected['rgb']))]
