"""One locked, raw CDC session with replies delimited on the bulk endpoint."""
import fcntl
import os
import select
import struct
import termios
import time


class DitingUsbSession:
    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.received = bytearray()
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.ioctl(self.fd, termios.TIOCMBIC, struct.pack('I', termios.TIOCM_DTR))
            attributes = termios.tcgetattr(self.fd)
            attributes[0] = attributes[1] = attributes[3] = 0
            attributes[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            attributes[4] = attributes[5] = termios.B115200
            attributes[6][termios.VMIN] = attributes[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attributes)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
            time.sleep(.05)
            fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack('I', termios.TIOCM_DTR))
            time.sleep(.05)
            termios.tcflush(self.fd, termios.TCIFLUSH)
        except BaseException:
            os.close(self.fd)
            self.fd = None
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def command(self, command, timeout=8):
        if not command or any(ord(c) < 32 or ord(c) > 126 for c in command) or len(command) > 63:
            raise ValueError('one bounded ASCII command line required')
        if not 0 < timeout <= 60:
            raise ValueError('timeout must be 0..60 seconds')
        self.received = bytearray()
        begin = ('DitingUSB BEGIN ' + command + '\n').encode('ascii')
        end = ('DitingUSB END ' + command + '\n').encode('ascii')
        # DTR cannot order already queued bulk OUT bytes. Discard an echoed
        # prefix on the same endpoint as the real command. The invalid line
        # produces no reply and performs no device operation.
        payload = b'\x15\n' + (command + '\n').encode('ascii')
        written = 0
        deadline = time.monotonic() + timeout
        while written < len(payload):
            if time.monotonic() >= deadline:
                raise TimeoutError('CDC command write timeout')
            _, ready, _ = select.select([], [self.fd], [], .1)
            if ready:
                try:
                    written += os.write(self.fd, payload[written:])
                except BlockingIOError:
                    pass
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.fd], [], [], .1)
            if not ready:
                continue
            try:
                block = os.read(self.fd, 16384)
            except BlockingIOError:
                continue
            if not block:
                break
            self.received.extend(block)
            if len(self.received) > 2 * 1024 * 1024:
                raise RuntimeError('CDC response exceeded the capture bound')
            first = self.received.find(begin)
            last = self.received.find(end, first + len(begin)) if first >= 0 else -1
            if last >= 0:
                return bytes(self.received[first:last + len(end)])
        raise TimeoutError('device did not return the matching complete CDC response')

    def capture(self, seconds, max_bytes):
        if not 0 < seconds <= 60 or not 1 <= max_bytes <= 16 * 1048576:
            raise ValueError('capture duration or byte limit outside bounds')
        data = bytearray()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and len(data) < max_bytes:
            ready, _, _ = select.select([self.fd], [], [], min(1, max(0, deadline - time.monotonic())))
            if ready:
                try:
                    block = os.read(self.fd, min(16384, max_bytes - len(data)))
                except BlockingIOError:
                    continue
                if block:
                    data.extend(block)
                else:
                    time.sleep(.01)
        return bytes(data)
