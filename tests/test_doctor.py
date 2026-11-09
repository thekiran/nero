#!/usr/bin/env python3
"""Read-only doctor entry point: hostile environment, PATH and privacy checks."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
BINARY = Path(sys.argv.pop(1)).resolve()

class DoctorTests(unittest.TestCase):
    def test_no_storage_writes_or_sensitive_environment_echo(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = dict(os.environ, HOME=str(root), USERPROFILE=str(root),
                       NERO_HISTORY_FILE=str(root / 'history'),
                       NERO_LOG_FILE=str(root / 'private' / 'log'),
                       TOKEN='doctor-secret-1905', TERM='dumb', NO_COLOR='1',
                       HOSTNAME='192.168.31.51', PRIVATE_MAC='aa:bb:cc:dd:ee:ff')
            before = sorted(root.rglob('*'))
            r = subprocess.run([str(BINARY), '--doctor'], env=env, capture_output=True, timeout=5)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stderr, b'')
            text = r.stdout.decode()
            for secret in (str(root), 'doctor-secret-1905', '192.168.31.51', 'aa:bb:cc:dd:ee:ff', '\033'):
                self.assertNotIn(secret, text)
            self.assertEqual(before, sorted(root.rglob('*')))
            self.assertIn('redirected/plain', text)
            self.assertIn('Offline Doctor', text)

    def test_path_missing_oversized_matching_and_shadowed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            filename = 'nero.exe' if os.name == 'nt' else 'nero'
            first = root / 'first'; first.mkdir()
            second = root / 'second'; second.mkdir()
            executable = first / filename
            shutil.copy2(BINARY, executable)
            shutil.copy2(BINARY, second / filename)
            for value, expected in [('', 'nero not found'), ('x'*33000, 'not verifiable'),
                                    (str(first), 'resolves to this executable'),
                                    (str(second), 'another nero precedes')]:
                # Windows has a 32767-character environment block limit.
                if os.name == 'nt' and len(value) > 32000:
                    continue
                env = dict(os.environ, PATH=value)
                r = subprocess.run([str(executable), '--doctor'], env=env, capture_output=True, timeout=5)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn(expected, r.stdout.decode())
                self.assertNotIn(directory, r.stdout.decode())

    def test_storage_root_unavailable(self):
        env = dict(os.environ)
        env.pop('HOME', None); env.pop('USERPROFILE', None)
        r = subprocess.run([str(BINARY), '--doctor'], env=env, capture_output=True, timeout=5)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b'not configured', r.stdout)

if __name__ == '__main__':
    unittest.main()
