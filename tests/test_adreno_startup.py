"""Execute the production AArch64 HFI/power waiter and retained log contract."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
DRIVER=ROOT/'drivers/DitingAdreno'
class Startup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ('aarch64-linux-gnu-gcc','qemu-aarch64','c++'):
            if not shutil.which(tool):raise RuntimeError('Required real-source test tool missing: '+tool)
        cls.tmp=tempfile.TemporaryDirectory(prefix='gmu-ack-',dir=os.environ.get('DITING_STARTUP_TEST_DIR'))
        out=Path(cls.tmp.name);cls.out=out
        source=(DRIVER/'DitingAdrenoGmu.c').read_text();profile=(DRIVER/'DitingAdrenoCpProfile.h').read_text()
        lines=[]
        for name in sorted(set(re.findall(r'\bGEN7_[A-Z0-9_]+\b',source))):
            hit=re.search(r'^#define\s+'+name+r'\s+(0x[0-9a-fA-F]+)[uU]?\s*$',profile,re.M)
            if not hit:raise RuntimeError('Missing SM8475 register: '+name)
            lines.append('#define '+name+' '+hit[1]+'u')
        (out/'DitingAdrenoRegisters.h').write_text('\n'.join(lines)+'\n')
        subprocess.run(['aarch64-linux-gnu-gcc','-static','-std=c11','-O2','-Wall','-Wextra','-Werror',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(out),'-I'+str(DRIVER),
            str(ROOT/'tests/adreno_startup_ack.c'),str(DRIVER/'DitingAdrenoGmu.c'),str(DRIVER/'DitingAdrenoHfi.c'),
            '-o',str(out/'ack')],check=True,capture_output=True,text=True)
        subprocess.run(['c++','-std=c++17','-O2','-pthread','-Wall','-Wextra','-Werror','-I'+str(DRIVER),
            str(ROOT/'tests/adreno_startup_log.cpp'),'-o',str(out/'history')],check=True,capture_output=True,text=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def scenario(self,index):
        result=subprocess.run(['qemu-aarch64',str(self.out/'ack'),str(index)],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS real GMU/HFI scenario '+str(index),result.stdout)
    def test_immediate_responses_and_irq_ownership(self):self.scenario(1)
    def test_preempted_boundary_has_matching_success(self):self.scenario(2)
    def test_missing_reply_still_times_out(self):self.scenario(3)
    def test_firmware_error_is_not_success(self):self.scenario(4)
    def test_firmware_reset_rejects_queued_ack(self):self.scenario(5)
    def test_frozen_clock_has_iteration_bound(self):self.scenario(6)
    def test_stale_sequence_is_not_success(self):self.scenario(7)
    def test_uncertain_notification_retains_memory(self):self.scenario(8)
    def test_irq_write_failure_is_not_success(self):self.scenario(9)
    def test_unrelated_interrupt_is_preserved(self):self.scenario(10)
    def test_backwards_clock_fails(self):self.scenario(11)
    def test_wait_failure_is_not_success(self):self.scenario(12)
    def test_oob_and_start_ack_use_separate_bits(self):self.scenario(13)
    def test_corrupt_queue_is_not_success(self):self.scenario(14)
    def test_immutable_bounded_history(self):
        result=subprocess.run([str(self.out/'history')],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS bounded startup history',result.stdout)
