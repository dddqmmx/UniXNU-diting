"""Production cursor ranges, UART framing, and accounting of missing events."""
import base64
import importlib.util
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from diting_usb_session import DitingUsbSession
spec=importlib.util.spec_from_file_location('capture_gpu_live',ROOT/'scripts/capture-diting-gpu-live.py')
capture=importlib.util.module_from_spec(spec);spec.loader.exec_module(capture)


class RangeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory=tempfile.TemporaryDirectory(prefix='diting-stream-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary=Path(cls.directory.name)/'stream'
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-Wall','-Wextra','-Werror',
                        '-pedantic','-pthread','-fsanitize=address,undefined','-g',
                        '-I'+str(ROOT/'drivers/DitingAdreno/submit'),'-I'+str(ROOT/'drivers/DitingUsb'),
                        str(ROOT/'tests/diting_gpu_stream.cpp'),'-o',str(cls.binary)],
                       capture_output=True,text=True,check=True)

    def test_ranges_and_protocol(self):
        for case in ['wrap','busy','bounds','concurrent','commands','base64']:
            with self.subTest(case=case):
                result=subprocess.run([str(self.binary),case],capture_output=True,text=True,check=True,timeout=10)
                self.assertEqual(result.stdout.strip(),'PASS '+case)


class HostTests(unittest.TestCase):
    def test_same_endpoint_resync_and_multiple_fragmented_replies(self):
        master,slave=pty.openpty();stop=threading.Event();seen=[]
        self.addCleanup(os.close,master);self.addCleanup(os.close,slave)
        # Pending bulk bytes survive a control-line transition in the real
        # device. This peer starts with the observed partial echoed line.
        def peer():
            line=bytearray(b' (user/501/com.astale');discard=False
            while not stop.is_set():
                ready,_,_=select.select([master],[],[],.05)
                if not ready:continue
                for c in os.read(master,1024):
                    if c in (10,13):
                        if line and not discard:
                            command=line.decode('ascii');seen.append(command)
                            reply=f'console fragmentDitingUSB BEGIN {command}\nOK\nDitingUSB END {command}\n'.encode()
                            for off in range(0,len(reply),7):
                                os.write(master,reply[off:off+7]);time.sleep(.001)
                        line.clear();discard=False
                    elif c<32 or c>126 or len(line)>=63:discard=True
                    elif not discard:line.append(c)
        thread=threading.Thread(target=peer);thread.start()
        try:
            # PTYs have no modem-control hardware. Only these two ioctls are
            # substituted; raw tty settings, locks and I/O stay real.
            with patch('diting_usb_session.fcntl.ioctl',return_value=0) as ioctl:
                with DitingUsbSession(os.ttyname(slave)) as session:
                    for command in ['status','help','status']:
                        self.assertEqual(session.command(command,1),
                                         f'DitingUSB BEGIN {command}\nOK\nDitingUSB END {command}\n'.encode())
                    self.assertEqual(ioctl.call_count,2)
            self.assertEqual(seen,['status','help','status'])
        finally:
            stop.set();thread.join(timeout=2)
        self.assertFalse(thread.is_alive())

    def test_chunk_validation_preserves_holes_and_rejects_corruption(self):
        rec=capture.RECORD.pack(2,100,10,0,*([0]*12))
        header=capture.CHUNK.pack(0x31545047,1,120,1,3,0,1,3,0,2)
        def reply(raw):
            return b'\nGPU trace bytes='+str(len(raw)).encode()+b'\nDATA '+base64.b64encode(raw)+b'\n'
        chunk,data=capture.decode_chunk(reply(header+rec),0,3)
        self.assertEqual((chunk['missing'],chunk['next'],data),(2,3,rec))
        with self.assertRaises(ValueError):capture.decode_chunk(reply(header+rec[:-1]),0,3)
        with self.assertRaises(ValueError):capture.decode_chunk(reply(header+rec),3,3)
        bad=capture.CHUNK.pack(0x31545047,1,120,1,3,0,1,3,0,0)
        with self.assertRaises(ValueError):capture.decode_chunk(reply(bad+rec),0,3)


if __name__=='__main__':unittest.main()
