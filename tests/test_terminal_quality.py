"""POSIX terminal regression tests. argv[1] is a mock-linked executable."""
import errno
import fcntl
import os
import pathlib
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time
import unittest

BINARY = pathlib.Path(sys.argv.pop(1)).resolve()
ENV = dict(os.environ, TERM="xterm", UBSAN_OPTIONS="halt_on_error=1")


def run(data, **extra):
    return subprocess.run([str(BINARY)], input=data, capture_output=True,
                          timeout=10, env=dict(ENV, **extra))


class TerminalQuality(unittest.TestCase):
    def test_catalog_completion_and_help(self):
        output = self.output(b"vers\t\nhistory -c\t\nhelp\nexit\n")
        self.assertIn(b"version 1.0", output)
        self.assertIn(b"History cleared.", output)
        self.assertNotIn(b"Unknown command", output)
        for usage in (b"help -All", b"history -clear", b"exit -All",
                      b"<network IP> -Syn", b"<target> -Sc", b"-mode on", b"-mode off"):
            self.assertIn(usage, output)

    def output(self, data):
        result = run(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"Sanitizer", result.stderr)
        return result.stdout + result.stderr

    def test_blank_and_exact_commands(self):
        output = self.output(b"\n \t \nhelper\npreexit\nversionSuffix\nhelp\nhelp -All\nversion\nexit\n")
        self.assertEqual(output.count(b"Unknown command"), 3)
        self.assertEqual(output.count(b"Available Commands:"), 2)
        self.assertEqual(output.count(b"version 1.0"), 1)
        self.assertEqual(output.count(b"Exiting..."), 1)

    def test_invalid_parameters_and_dispatch(self):
        data = (b"help wrong\nversion extra\nclear extra\nexit extra\nhistory extra\n"
                b"192.0.2.1 -Syn extra\nnot-an-ip -Syn\n192.0.2.1;evil -Syn\n"
                b"  192.0.2.1   -Syn  \n198.51.100.1\t-Syn\nexit\n")
        output = self.output(data)
        self.assertEqual(output.count(b"Invalid command parameters"), 8)
        self.assertEqual(output.count(b"MOCK_SYN:"), 2)
        self.assertIn(b"MOCK_SYN:192.0.2.1", output)
        self.assertIn(b"MOCK_SYN:198.51.100.1", output)

    def test_oversize_line_is_not_dispatched(self):
        output = self.output(b"192.0.2.1 -Syn " + b" " * 4096 + b"\nversion\nexit\n")
        self.assertIn(b"Command too long", output)
        self.assertNotIn(b"MOCK_SYN:", output)
        self.assertIn(b"version 1.0", output)

    def test_cursor_insert_delete_backspace(self):
        for edit in (b"verion\x1b[D\x1b[D\x1b[Ds",
                     b"versxion\x1b[D\x1b[D\x1b[D\x7f",
                     b"versxion\x1b[D\x1b[D\x1b[D\x1b[D\x1b[3~",
                     b"version\x1b[D\x1b[D\x1b[C",
                     b"versxion\x1b[D\x1b[D\x1b[D\x1b[D\x04"):
            with self.subTest(edit=edit):
                output = self.output(edit + b"\nexit\n")
                self.assertIn(b"version 1.0", output)
                self.assertNotIn(b"Unknown command", output)

    def test_history_draft_and_clear(self):
        output = self.output(b"help\nversion\x1b[A\x1b[B\nhistory -clear\nafter-clear\nhistory\nexit\n")
        self.assertIn(b"version 1.0", output)
        self.assertIn(b"History cleared.", output)
        # Only entries typed after the clear are listed.
        self.assertIn(b"\nafter-clear\nhistory", output)
        self.assertNotIn(b"\nhelp\nversion", output)

    def test_history_eviction(self):
        output = self.output(b"first-entry\n" + b"version\n" * 256 + b"history\nexit\n")
        # The first entry appears once as input, never again in history.
        self.assertEqual(output.count(b"first-entry"), 1)
        self.assertEqual(output.count(b"version 1.0"), 256)

    def test_escape_and_controls(self):
        output = self.output(b"\x1b[99~\x1bOP\x00version\nexit\n")
        self.assertIn(b"version 1.0", output)
        self.assertNotIn(b"Unknown command", output)
        for data in (b"", b"\x1b", b"\x1b[", b"\x1b[123", b"\x04", b"unfinished"):
            self.output(data)
        output = self.output(b"\x1b[" + b"1" * 40 + b"version\nexit\n")
        self.assertNotIn(b"version 1.0", output)
        self.assertIn(b"Command too long", output)

    def test_offline_legacy_commands(self):
        output = self.output(b"192.0.2.1 -Sc\n-mode\n192.0.2.1 -mode\nexit -All\n")
        self.assertNotIn(b"MOCK_SYN:", output)
        self.assertNotIn(b"Unknown command", output)
        self.assertEqual(output.count(b"-Sc: simulation for 192.0.2.1; packets sent: 0"), 1)
        self.assertEqual(output.count(b"Network diagnostics: ON"), 2)

    def test_repeated_clear_and_fallback(self):
        result = run(b"clear\n" * 200 + b"help\nexit\n", TERM="dumb", NERO_ASCII="1", NO_COLOR="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"\x1b", result.stdout)
        self.assertIn(b"N E R O", result.stdout)
        self.assertIn(b"LABORATORY USE ONLY", result.stdout)

    def test_closed_stdin_error(self):
        try:
            result = subprocess.run([str(BINARY)], capture_output=True, timeout=10, env=ENV,
                                    preexec_fn=lambda: os.close(0))
        except subprocess.TimeoutExpired as error:
            self.fail(f"closed-stdin run hung: partial stdout={error.stdout!r} "
                      f"stderr={error.stderr!r}")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b"terminal read", result.stderr)

    def test_devnull_stdin_is_eof_not_error(self):
        with open(os.devnull, "rb") as devnull:
            result = subprocess.run([str(BINARY)], stdin=devnull, capture_output=True,
                                    timeout=10, env=ENV)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"\xe2\x96\xba", result.stdout)

    def test_control_c_pipe(self):
        result = run(b"\x03")
        self.assertEqual(result.returncode, 128 + signal.SIGINT)

    def pty_session(self, ending, sig=None, env=None, sequence=None, binary=BINARY):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 20, 0, 0))
        before = termios.tcgetattr(slave)
        proc = subprocess.Popen([str(binary)], stdin=slave, stdout=slave, stderr=slave,
                                env=dict(ENV, **(env or {})))
        output = bytearray()

        def wait_prompt():
            start = len(output)
            deadline = time.monotonic() + 5
            cursor = b"> " if env and "NERO_ASCII" in env else b"\xe2\x96\xba "
            while cursor not in output[start:]:
                self.assertLess(time.monotonic(), deadline, output)
                if select.select([master], [], [], 0.05)[0]:
                    output.extend(os.read(master, 65536))

        try:
            wait_prompt()
            if sequence:
                os.write(master, sequence)
                wait_prompt()
            if sig:
                os.kill(proc.pid, sig)
            else:
                os.write(master, ending)
            deadline = time.monotonic() + 5
            while proc.poll() is None:
                self.assertLess(time.monotonic(), deadline, output)
                if select.select([master], [], [], 0.05)[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
            after = termios.tcgetattr(slave)
            before[3] &= ~termios.PENDIN
            after[3] &= ~termios.PENDIN
            self.assertEqual(after, before)
            self.assertEqual(proc.returncode, 128 + sig if sig else 0, output)
            self.assertNotIn(b"Sanitizer", output)
            return bytes(output)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)

    def test_signal_cleanup(self):
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            with self.subTest(sig=sig):
                self.pty_session(b"", sig=sig)

    def test_tty_ctrl_d_and_clear_cleanup(self):
        self.pty_session(b"\x04")
        self.pty_session(b"exit\n", sequence=b"clear\n")

    def test_no_color_tty(self):
        output = self.pty_session(b"exit\n", env={"NO_COLOR": "1", "NERO_ASCII": "1"})
        self.assertNotIn(b"\x1b[0;35m", output)
        self.assertIn(b"N E R O", output)

    def test_narrow_terminal_viewport(self):
        import re
        output = self.pty_session(b"exit\n", sequence=b"x" * 100 + b"\x7f" * 100 + b"version\n")
        redraws = output.split(b"\r\x1b[K")[1:]
        self.assertTrue(redraws)
        for redraw in redraws:
            first_line = redraw.split(b"\r")[0].split(b"\n")[0]
            plain = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", first_line)
            # The viewport uses at most 19 cells in the 20-column test PTY.
            if b"version 1.0" not in plain:
                self.assertLessEqual(len(plain.decode("utf-8", errors="replace")), 19, plain)

    def test_terminal_hangup_does_not_spin(self):
        master, slave = pty.openpty()
        proc = subprocess.Popen([str(BINARY)], stdin=slave, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=ENV)
        try:
            self.assertTrue(select.select([proc.stdout], [], [], 5)[0])
            os.close(master)
            master = None
            output, error = proc.communicate(timeout=5)
            self.assertIn(proc.returncode, (0, 1), error)
            self.assertNotIn(b"Sanitizer", output + error)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if master is not None:
                os.close(master)
            os.close(slave)

    def test_termios_setup_failures(self):
        for fault in ("getattr", "setattr"):
            master, slave = pty.openpty()
            before = termios.tcgetattr(slave)
            try:
                result = subprocess.run([str(BINARY.with_name("nero-faults"))], stdin=slave,
                                        capture_output=True, timeout=5,
                                        env=dict(ENV, NERO_TEST_FAULT=fault))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"tc" + fault.encode(), result.stderr)
                self.assertEqual(termios.tcgetattr(slave), before)
            finally:
                os.close(master)
                os.close(slave)

    def test_restore_retries_eintr(self):
        self.pty_session(b"exit\n", env={"NERO_TEST_FAULT": "restore-eintr"},
                         binary=BINARY.with_name("nero-faults"))

    def test_standalone_escape_timeout(self):
        master, slave = pty.openpty()
        proc = subprocess.Popen([str(BINARY)], stdin=slave, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=ENV)
        try:
            # Pipes avoid output backpressure; wait for the prompt before input.
            output = bytearray()
            deadline = time.monotonic() + 5
            while b"\xe2\x96\xba " not in output:
                self.assertLess(time.monotonic(), deadline)
                if select.select([proc.stdout], [], [], 0.05)[0]:
                    output.extend(os.read(proc.stdout.fileno(), 65536))
            os.write(master, b"\x1b")
            time.sleep(0.2)
            os.write(master, b"version\nexit\n")
            remaining, error = proc.communicate(timeout=5)
            self.assertEqual(proc.returncode, 0, error)
            self.assertIn(b"version 1.0", remaining)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)


unittest.main()
