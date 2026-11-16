"""Check Diting's retained DT against the MC3/SH5 and Cape LLCC contract."""
import json
import os
from pathlib import Path
import subprocess
def validate_live_dtb(out):
    source = os.environ.get('DITING_LIVE_DTB')
    result = dict(source=source, hardware_tested=False, cpu_bcms=['MC3', 'SH5'],
                  ddr_max_khz=3196000, llcc_max_khz=1066000, llcc_gpu_slices=[11, 12])
    if source:
        def values(node, prop, kind='x'):
            text = subprocess.check_output(['fdtget', '-t', kind, source, node, prop], text=True, timeout=10).split()
            return [int(v, 16) for v in text] if kind == 'x' else text
        fp = '/soc/rsc@17a00000/qcom,dcvs-fp'
        if (values(fp, 'qcom,ddr-bcm-name', 's') != ['MC3'] or values(fp, 'qcom,llcc-bcm-name', 's') != ['SH5'] or
            max(values('/soc/ddr-freq-table', 'qcom,freq-tbl')) != 3196000 or
            max(values('/soc/llcc-freq-table', 'qcom,freq-tbl')) != 1066000 or
            values('/soc/cache-controller@19200000', 'compatible', 's') != ['qcom,cape-llcc', 'qcom,llcc-v21'] or
            values('/soc/cache-controller@19200000', 'reg') != [0x19200000, 0x580000, 0x19a00000, 0x80000] or
            values('/soc/qcom,bwmon-ddr@19091000', 'reg') != [0x19091000, 0x1000] or
            values('/soc/qcom,bwmon-llcc@190b6400', 'reg') != [0x190b6400, 0x300, 0x190b6300, 0x200]):
            raise ValueError('Selected DT does not match Diting memory hardware/frequency/BCM inputs')
        result['result'] = 'PASS'
    else:
        result.update(result='SKIP', reason='No live DT selected; recorded Diting constants only')
    Path(out).write_text(json.dumps(result, indent=2) + '\n')
    return result
