"""PTY regression checks for operation layout; no network traffic is generated."""
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time


def check(binary, plain=False):
    master, slave = pty.openpty()

    def resize(rows, columns):
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    resize(24, 80)
    before = termios.tcgetattr(slave)
    env = dict(os.environ, TERM="xterm")
    env.pop("NO_COLOR", None)
    if plain:
        env["NO_COLOR"] = "1"
    process = subprocess.Popen([binary], stdin=slave, stdout=slave, stderr=slave, env=env)

    def collect(seconds=0.3):
        output = bytearray()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if select.select([master], [], [], 0.03)[0]:
                output.extend(os.read(master, 65536))
        return bytes(output)

    try:
        logo = "░▒▓███████▓▒░".encode()
        initial = collect()
        # Process startup can exceed one frame interval on a busy machine.
        deadline = time.monotonic() + 3
        while logo not in initial and time.monotonic() < deadline:
            initial += collect()
        assert logo in initial
        assert b"Latest probe  TCP / 8197" in initial
        assert b"TCP   " in initial and b"OPEN" in initial
        assert (b"\x1b[35m" in initial) == (not plain)
        os.write(master, b"\x1b[B" * 80)
        moved = collect(0.6)
        assert b"> 1080" in moved and b"81 / 100" in moved
        assert logo not in moved, "List navigation repainted the logo"
        resize(10, 40)
        assert b"Enlarge terminal" in collect()
        resize(30, 100)
        enlarged = collect()
        assert logo in enlarged and b"> 1080" in enlarged
        os.write(master, b"\x1b")
        ended = collect()
        process.wait(timeout=2)
        assert process.returncode == 0 and b"\x1b[?1049l" in ended
        after = termios.tcgetattr(slave)
        pending = getattr(termios, "PENDIN", 0)
        before[3] &= ~pending
        after[3] &= ~pending
        assert before == after
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)
        os.close(slave)


for plain_mode in (False, True):
    check(sys.argv[1], plain_mode)
print("PASS operation layout: scrolling, resize, logo, NO_COLOR, Escape and termios")
