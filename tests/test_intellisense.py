"""
IntelliSense interaction tests over a real PTY (POSIX). argv[1] is a
mock-linked nero executable (NERO_SAFE_TEST). These exercise ghost text,
Right-to-accept, the Tab completion menu, Escape, configuration switches and
the capability fallbacks, and assert that no command runs during suggestion.
"""
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
ENV = dict(os.environ, TERM="xterm", NERO_HISTORY_FILE=":memory:", NERO_LOG_FILE=":memory:",
           UBSAN_OPTIONS="halt_on_error=1")
CURSOR = b"\xe2\x96\xba "  # the unicode prompt glyph


class IntelliSense(unittest.TestCase):
    def session(self, steps, env=None, cols=80, rows=24, expect_signal=None):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        proc = subprocess.Popen([str(BINARY)], stdin=slave, stdout=slave, stderr=slave,
                                env=dict(ENV, **(env or {})))
        output = bytearray()

        def pump(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.05)[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        break

        try:
            cursor = b"> " if env and "NERO_ASCII" in env else CURSOR
            deadline = time.monotonic() + 5
            while cursor not in output:
                self.assertLess(time.monotonic(), deadline, output)
                pump(0.1)
            for data, wait in steps:
                os.write(master, data)
                pump(wait)
            if expect_signal is not None:
                os.kill(proc.pid, expect_signal)
            deadline = time.monotonic() + 5
            while proc.poll() is None and time.monotonic() < deadline:
                pump(0.1)
            if proc.poll() is None:
                proc.kill()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass
            self.assertNotIn(b"Sanitizer", bytes(output))
            if expect_signal is not None:
                self.assertEqual(proc.returncode, 128 + expect_signal, output)
            return bytes(output)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
            os.close(slave)

    def test_ghost_text_shown(self):
        out = self.session([(b"ver", 0.3), (b"\r", 0.2), (b"exit\n", 0.3)])
        # Gray ghost sequence for the remaining "sion" of "version".
        self.assertIn(b"\x1b[90msion", out)

    def test_right_accepts_ghost(self):
        out = self.session([(b"ver", 0.3), (b"\x1b[C", 0.2), (b"\r", 0.3), (b"exit\n", 0.3)])
        self.assertIn(b"version development", out)

    def test_tab_unique_completes(self):
        out = self.session([(b"hist", 0.3), (b"\t", 0.2), (b"\r", 0.3), (b"exit\n", 0.3)])
        # "hist" -> "history"; running it lists history (no unknown-command).
        self.assertNotIn(b"Unknown command", out)

    def test_tab_menu_accept_runs_selection(self):
        # "network " + Tab opens a menu; Enter accepts the first item into the
        # line (without running it); a second Enter submits it.
        out = self.session([(b"network ", 0.3), (b"\t", 0.3), (b"\r", 0.3),
                            (b"\r", 0.3), (b"exit\n", 0.3)])
        self.assertIn(b"network enable", out)   # a menu label was rendered
        self.assertIn(b"Network diagnostics: ON", out)  # the accepted command ran

    def test_escape_closes_menu_without_accepting(self):
        # Open the menu, press Escape, then type "disable" and run: proves the
        # Escape did not accept the highlighted "network status".
        out = self.session([(b"network ", 0.3), (b"\t", 0.3), (b"\x1b", 0.3),
                            (b"disable\r", 0.3), (b"exit\n", 0.3)])
        self.assertIn(b"Network diagnostics: OFF", out)

    def test_no_execution_during_suggestion(self):
        # Type a complete, valid command but never submit it; interrupt instead.
        out = self.session([(b"network status", 0.4)], expect_signal=signal.SIGINT)
        self.assertNotIn(b"Network diagnostics:", out)

    def test_no_color_suppresses_ghost(self):
        out = self.session([(b"ver", 0.3), (b"\r", 0.2), (b"exit\n", 0.3)],
                           env={"NO_COLOR": "1", "NERO_ASCII": "1"})
        self.assertNotIn(b"\x1b[90m", out)

    def test_disabled_switch_suppresses_ghost(self):
        out = self.session([(b"ver", 0.3), (b"\r", 0.2), (b"exit\n", 0.3)],
                           env={"NERO_INTELLISENSE": "off"})
        self.assertNotIn(b"\x1b[90m", out)
        # Completion is also inert when disabled: unique completion still works
        # on Tab, but ghost/menu are gone.
        self.assertIn(b"WARNING", out)  # "ver" ran as an unknown command

    def test_full_mode_shows_help_line(self):
        out = self.session([(b"wifi analyze", 0.4), (b"\r", 0.2), (b"exit\n", 0.3)],
                           env={"NERO_INTELLISENSE": "full"})
        # The live help line echoes the catalog description for the command.
        self.assertIn(b"Analyze a capture file offline", out)

    def test_redirected_output_has_no_ansi_but_completes(self):
        # Non-TTY stdout: ghost/menu rendering is suppressed, completion still
        # rewrites the buffer so the command dispatches.
        result = subprocess.run([str(BINARY)], input=b"vers\t\nexit\n",
                                capture_output=True, timeout=10, env=ENV)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"version development", result.stdout + result.stderr)
        self.assertNotIn(b"\x1b[90m", result.stdout)


unittest.main()
