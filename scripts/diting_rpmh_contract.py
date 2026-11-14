"""Check the selected live Diting DT against the shared APSS RPMh owner.

TCS types are allocated in property order, not in numeric type order.
This check reads only a local DT; it neither sends votes nor audits images.
"""
import json
import os
from pathlib import Path
import subprocess


def validate_live_dtb(out):
    source = os.environ.get('DITING_LIVE_DTB')
    result = {'controller': 'SM8475 APSS DRV2', 'base': '0x17a20000',
              'tcs_offset': '0xd00', 'active': [0, 1, 2], 'sleep': [3, 4],
              'wake': [5, 6], 'source': source, 'hardware_tested': False}
    if source:
        node = '/soc/rsc@17a00000'

        def cells(name):
            text = subprocess.check_output(['fdtget', '-t', 'x', source, node, name],
                                           text=True, timeout=10)
            return [int(v, 16) for v in text.split()]

        drv = cells('qcom,drv-id')
        reg = cells('reg')
        offset = cells('qcom,tcs-offset')
        layout = cells('qcom,tcs-config')
        if (drv != [2] or len(reg) < 6 or reg[4:6] != [0x17a20000, 0x10000]
                or offset != [0xd00] or layout != [2, 3, 0, 2, 1, 2, 3, 0, 4, 1]):
            raise ValueError('Selected live DT does not match the SM8475 APSS RPMh/TCS contract')
        result['result'] = 'PASS'
        result['tcs_config'] = layout
    else:
        result['result'] = 'SKIP'
        result['reason'] = 'No live DT selected; using the recorded Diting board constants'
    Path(out).write_text(json.dumps(result, indent=2) + '\n')
    return result
