"""No live traffic: these executables never link the scanner or traffic engine."""
import os
import pathlib
import pty
import re
import select
import subprocess
import sys
import termios
import time
import unittest

ROOT = pathlib.Path(sys.argv.pop(1)).resolve()
ENV = dict(os.environ, TERM="xterm", UBSAN_OPTIONS="halt_on_error=1")
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def run(name, data):
    return subprocess.run([str(ROOT / name)], input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=ENV, timeout=15)


class SafeTests(unittest.TestCase):
    def cli(self, data):
        result = run("nero-safe", data)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertNotIn(b"Sanitizer", result.stderr)
        return ANSI.sub(b"", result.stdout + result.stderr)

    def test_commands_and_notice(self):
        output = self.cli(b"help\nhelp -All\nversion\nhistory\nunknown\nexit\n")
        for expected in (b"LABORATORY USE ONLY", b"RESTRICTED TO LABORATORY USE", b"Available Commands",
                         b"version 1.0", b"Unknown command", b"Exiting"):
            self.assertIn(expected, output)
        self.assertIn(b"help\nhelp -All\nversion\nhistory", output)

    def test_disabled_network_command(self):
        symbols = subprocess.check_output(["nm", "-u", str(ROOT / "nero-safe")])
        self.assertIsNone(re.search(
            rb"\b_?(socket|connect|sendto|sendmsg|getaddrinfo|gethostbyname)(?:@[^\s]+)?$",
            symbols, re.M), symbols)
        self.assertIn(b"unsupported in this build", self.cli(b"192.0.2.1 -Syn\nexit\n"))

    def test_eof_and_incomplete_escape(self):
        for data in (b"", b"unfinished", b"\x1b", b"\x1b["):
            self.cli(data)

    def test_long_input_and_cursor_bounds(self):
        self.cli(b"x" * 4096 + b"\x1b[C" * 512 + b"\nexit\n")
        output = self.cli(b"versiox\x7fn\nexit\n")
        self.assertIn(b"version 1.0", output)

    def test_history_navigation(self):
        output = self.cli(b"version\nhelp\n\x1b[A\x1b[A\x1b[B\nexit\n")
        self.assertEqual(output.count(b"Available Commands"), 2)
        output = self.cli(b"version\n\x1b[A\nexit\n")
        self.assertEqual(output.count(b"version 1.0"), 2)

    def test_history_capacity_and_clear(self):
        self.cli(b"version\n" * 270 + b"history\nexit\n")
        self.cli(b"clear\n" * 100 + b"exit\n")

    def test_interface_selection(self):
        result = run("interfaces", b"1\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = ANSI.sub(b"", result.stdout)
        self.assertIn(b"Available Network Interfaces:", output)
        self.assertIn(b"Select an interface [1-", output)
        names = re.findall(rb"^\s*\d+\) (\S+)", output, re.M)
        self.assertTrue(names)
        self.assertEqual(len(names), len(set(names)))
        self.assertIn(b"SELECTED:" + names[0], output)
        for data in (b"0\n", b"-1\n", b"999\n", b"invalid\n", b""):
            invalid = run("interfaces", data)
            self.assertEqual(invalid.returncode, 1)
            self.assertIn(b"Invalid selection.", invalid.stderr)

    @unittest.skipUnless(sys.platform == "darwin", "Darwin adapter fixtures")
    def test_interface_fixtures(self):
        for mode, data, code, expected in (
            ("normal", b"1\n", 0, b"SELECTED:fixture0"),
            ("normal", b"2\n", 0, b"SELECTED:fixture5"),
            ("normal", b"3\n", 1, b""),
            ("empty", b"", 1, b""),
            ("error", b"", 1, b"getifaddrs"),
            ("limit", b"128\n", 0, b"SELECTED:fixture127"),
        ):
            result = subprocess.run([str(ROOT / "interfaces-mock")], input=data,
                                    capture_output=True, timeout=10,
                                    env=dict(ENV, INTERFACE_FIXTURE=mode))
            self.assertEqual(result.returncode, code, result.stderr)
            self.assertIn(expected, result.stdout + result.stderr)
            self.assertNotIn(b"Sanitizer", result.stderr)

    @unittest.skipUnless(sys.platform == "darwin", "Darwin adapter fixtures")
    def test_interface_selection_requires_complete_bounded_integer(self):
        for data in (b"1junk\n", b"1 2\n", b"4294967297\n",
                     b"9" * 100 + b"\n", b"1" + b" " * 200 + b"\n",
                     b"\n", b" \t\n"):
            with self.subTest(data=data):
                result = run("interfaces-mock", data)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"Invalid selection.", result.stderr)
                self.assertNotIn(b"SELECTED:", result.stdout)
        for data in (b"1\n", b" 1 \t\n", b"1"):
            with self.subTest(data=data):
                result = run("interfaces-mock", data)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(b"SELECTED:fixture0", result.stdout)

    def test_pty_restores_terminal(self):
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        process = subprocess.Popen([str(ROOT / "nero-safe")], stdin=slave,
                                   stdout=slave, stderr=slave, env=ENV)
        output = bytearray()
        try:
            deadline = time.monotonic() + 10
            while b"\xe2\x96\xba" not in output:
                self.assertLess(time.monotonic(), deadline, output)
                if select.select([master], [], [], 0.1)[0]:
                    output.extend(os.read(master, 65536))
            during = termios.tcgetattr(slave)
            self.assertFalse(during[3] & (termios.ECHO | termios.ICANON))
            for command in (b"version\n", b"clear\n"):
                os.write(master, command)
                response = bytearray()
                deadline = time.monotonic() + 10
                while b"\xe2\x96\xba" not in response:
                    self.assertLess(time.monotonic(), deadline, response)
                    if select.select([master], [], [], 0.1)[0]:
                        response.extend(os.read(master, 65536))
            os.write(master, b"exit\n")
            deadline = time.monotonic() + 10
            while process.poll() is None:
                self.assertLess(time.monotonic(), deadline)
                if select.select([master], [], [], 0.1)[0]:
                    output.extend(os.read(master, 65536))
            process.wait(timeout=1)
            self.assertEqual(process.returncode, 0)
            after = termios.tcgetattr(slave)
            # BSD may set the transient pending-input bit when ICANON is restored.
            after[3] &= ~termios.PENDIN
            before[3] &= ~termios.PENDIN
            self.assertEqual(after, before)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)


unittest.main()
