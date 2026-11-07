"""Exercise the native Windows CLI and adapter without network operations."""
import os
from pathlib import Path
import subprocess
import sys
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()
INTERFACES = Path(sys.argv.pop(1)).resolve()
ENV = dict(os.environ, NO_COLOR='1', NERO_ASCII='1', TERM='dumb')


def run(data):
    return subprocess.run([str(BINARY)], input=data, capture_output=True,
                          timeout=15, env=ENV)


class NativeWindowsCLI(unittest.TestCase):
    def test_commands_and_history(self):
        result = run(b'version\nhelp\nhistory\nexit\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'version 1.0', result.stdout)
        self.assertIn(b'Available Commands:', result.stdout)
        self.assertIn(b'version\nhelp', result.stdout.replace(b'\r\n', b'\n'))
        self.assertIn(b'Exiting...', result.stdout)

    def test_eof(self):
        result = run(b'')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_and_bounded_input(self):
        result = run(b'unknown-command\n' + b'x' * 4096 + b'\nversion\nexit\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'Unknown command', result.stdout + result.stderr)
        self.assertIn(b'Command too long', result.stdout + result.stderr)
        self.assertIn(b'version 1.0', result.stdout)

    def test_raw_engine_is_explicitly_unavailable(self):
        result = run(b'192.0.2.1 -Syn\nexit\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'-Syn: unsupported in this build', result.stdout + result.stderr)
        self.assertIn(b'no network traffic is generated', result.stdout + result.stderr)

    def test_interface_selection(self):
        result = subprocess.run([str(INTERFACES)], input=b'1\n', capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'SELECTED:', result.stdout)

    def test_interface_rejects_invalid_selection(self):
        for value in (b'0\n', b'-1\n', b'1junk\n', b'999999\n', b''):
            with self.subTest(value=value):
                result = subprocess.run([str(INTERFACES)], input=value, capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertNotIn(b'SELECTED:', result.stdout)


unittest.main()
