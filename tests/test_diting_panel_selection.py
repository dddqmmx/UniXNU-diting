"""Reject incorrect retained panel inputs before selecting vendor commands."""
import copy
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from diting_panel_contract import MODELS, adt_panel_properties, selection


def captured_shape(name):
    path = '/soc/qcom,mdss_mdp@ae00000/' + name
    panel = {
        'mi,panel-id': struct.pack('>2I', *MODELS[name]),
        'qcom,mdss-dsi-panel-type': b'dsi_cmd_mode\0',
        'qcom,mdss-dsi-color-order': b'rgb_swap_rgb\0',
        'qcom,mdss-dsi-bpp': struct.pack('>I', 30),
        'qcom,dsi-ctrl-num': bytes(4), 'qcom,dsi-phy-num': bytes(4),
        'qcom,mdss-dsi-stream': bytes(4), 'qcom,mdss-dsi-virtual-channel-id': bytes(4),
    }
    nodes = {'/chosen': {'bootargs': ('console=x msm_drm.dsi_display0=' + name + ': debug=y\0').encode()},
             path: panel}
    for index, hz in enumerate((60, 90, 120)):
        nodes[path + '/qcom,mdss-dsi-display-timings/timing@' + str(index)] = {
            'qcom,mdss-dsi-panel-width': struct.pack('>I', 1220),
            'qcom,mdss-dsi-panel-height': struct.pack('>I', 2712),
            'qcom,mdss-dsi-panel-framerate': struct.pack('>I', hz),
        }
    return nodes, path


class PanelSelectionTests(unittest.TestCase):
    def test_metadata_models(self):
        for name, expected in MODELS.items():
            with self.subTest(name=name):
                nodes, _ = captured_shape(name)
                result = selection(nodes)
                self.assertEqual(result['model'], list(expected))
                self.assertEqual((result['width'], result['height'], result['rgb']), (1220, 2712, 1))

    def test_absent_or_ambiguous_selection(self):
        name = next(iter(MODELS))
        for fault in ('chosen', 'duplicate', 'node', 'ambiguous', 'unsupported'):
            with self.subTest(fault=fault):
                nodes, path = captured_shape(name)
                if fault == 'chosen': del nodes['/chosen']
                if fault == 'duplicate': nodes['/chosen']['bootargs'] += (' msm_drm.dsi_display0=' + name).encode()
                if fault == 'node': del nodes[path]
                if fault == 'ambiguous': nodes['/other/' + name] = copy.deepcopy(nodes[path])
                if fault == 'unsupported': nodes['/chosen']['bootargs'] = b'msm_drm.dsi_display0=unknown:\0'
                with self.assertRaises(ValueError): selection(nodes)

    def test_hardware_and_geometry_mismatch(self):
        name = next(iter(MODELS))
        for field in ('mi,panel-id', 'qcom,mdss-dsi-panel-type', 'qcom,mdss-dsi-color-order',
                      'qcom,mdss-dsi-bpp', 'qcom,dsi-ctrl-num', 'qcom,dsi-phy-num',
                      'qcom,mdss-dsi-stream', 'qcom,mdss-dsi-virtual-channel-id', 'width', 'height', 'hz'):
            with self.subTest(field=field):
                nodes, path = captured_shape(name)
                if field in ('width', 'height', 'hz'):
                    timing = nodes[path + '/qcom,mdss-dsi-display-timings/timing@0']
                    key = {'width': 'qcom,mdss-dsi-panel-width', 'height': 'qcom,mdss-dsi-panel-height',
                           'hz': 'qcom,mdss-dsi-panel-framerate'}[field]
                    timing[key] = struct.pack('>I', 1)
                else: nodes[path][field] = b'bad'
                with self.assertRaises(ValueError): selection(nodes)

    def test_missing_live_input_has_no_selection_property(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(adt_panel_properties(), [])


if __name__ == '__main__':
    unittest.main()
