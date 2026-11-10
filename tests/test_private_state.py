"""Real log/history boundaries; only temporary local files, no network I/O."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class PrivateStateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nero-state-',
            dir=os.environ.get('LOCALAPPDATA') if os.name == 'nt' else None)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def cli(self, log, history, commands=b'version\nlogs clear\nhistory clear\nexit\n', **extra):
        result = subprocess.run([str(BINARY)], input=commands, capture_output=True,
            env=dict(os.environ, NERO_LOG_FILE=str(log), NERO_HISTORY_FILE=str(history),
                     NO_COLOR='1', TERM='dumb', **extra), timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout + result.stderr

    def test_absolute_custom_paths_with_spaces(self):
        parent = self.root / 'custom state'; parent.mkdir()
        log, history = parent / 'my log', parent / 'my history'
        self.cli(log, history, b'version\nexit\n')
        self.assertIn('session', log.read_text())
        self.assertEqual(history.read_bytes(), b'version\n')

    def test_traversal_rejected_before_creating_any_parent(self):
        for bad in ('uncreated/../victim', 'uncreated/./victim', 'uncreated/victim/'):
            with self.subTest(path=bad):
                out = self.cli(str(self.root) + '/' + bad, str(self.root) + '/' + bad)
                self.assertIn(b'History storage unavailable', out)
                self.assertFalse((self.root / 'uncreated').exists())
                self.assertFalse((self.root / 'victim').exists())

    def test_overlong_paths_do_not_create_partial_directories(self):
        bad = str(self.root) + '/uncreated/' + 'a' * 260
        self.cli(bad, bad)
        self.assertFalse((self.root / 'uncreated').exists())

    def symlink(self, target, link, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except OSError as error:
            if os.name == 'nt': self.skipTest('Windows symlink privilege unavailable: ' + str(error))
            raise

    def test_parent_symlinks_never_create_or_import_target_state(self):
        victim = self.root / 'victim'; victim.mkdir()
        alias = self.root / 'alias'; self.symlink(victim, alias, True)
        marker = '[2000-01-01T00:00:00Z] [ERROR] [fixture] PRIVATE_OUTSIDE_MARKER\n'
        (victim / 'state').write_text(marker)
        before = (victim / 'state').read_bytes()
        out = self.cli(alias / 'state', alias / 'history')
        self.assertNotIn(b'PRIVATE_OUTSIDE_MARKER', out)
        self.assertEqual((victim / 'state').read_bytes(), before)
        self.assertFalse((victim / 'history').exists())

    def test_leaf_and_backup_symlinks_preserve_victims(self):
        victim = self.root / 'victim'; victim.write_text('PRIVATE_OUTSIDE_MARKER')
        for name in ('log', 'log.1', 'history'):
            with self.subTest(name=name):
                link = self.root / name; link.unlink(missing_ok=True); self.symlink(victim, link)
                out = self.cli(self.root / 'log', self.root / 'history')
                self.assertNotIn(b'PRIVATE_OUTSIDE_MARKER', out)
                self.assertEqual(victim.read_text(), 'PRIVATE_OUTSIDE_MARKER')
                link.unlink()

    def test_hardlinks_preserve_victims(self):
        victim = self.root / 'victim'; victim.write_text('PRIVATE_OUTSIDE_MARKER')
        if os.name != 'nt': victim.chmod(0o600)
        for name in ('log', 'log.1', 'history'):
            with self.subTest(name=name):
                link = self.root / name
                link.unlink(missing_ok=True)
                os.link(victim, link)
                out = self.cli(self.root / 'log', self.root / 'history')
                self.assertNotIn(b'PRIVATE_OUTSIDE_MARKER', out)
                self.assertEqual(victim.read_text(), 'PRIVATE_OUTSIDE_MARKER')
                link.unlink()

    @unittest.skipUnless(os.name == 'posix', 'POSIX permissions and pinned directory descriptors')
    def test_shared_parent_is_rejected(self):
        shared = self.root / 'shared'; shared.mkdir(); shared.chmod(0o777)
        self.cli(shared / 'log', shared / 'history')
        self.assertEqual(list(shared.iterdir()), [])

    @unittest.skipUnless(os.name == 'posix', 'POSIX deterministic parent swap')
    def test_parent_swap_cannot_redirect_write_rotation_or_clear(self):
        parent = self.root / 'parent'; parent.mkdir()
        victim = self.root / 'victim'; victim.mkdir()
        for name in ('log', 'log.1', 'history'):
            (victim / name).write_text('PRIVATE_OUTSIDE_MARKER')
        env = dict(os.environ, NERO_LOG_FILE=str(parent / 'log'),
                   NERO_HISTORY_FILE=str(parent / 'history'), TERM='dumb')
        process = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        try:
            process.stdin.write(b'version\n'); process.stdin.flush()
            deadline = time.monotonic() + 5
            while not (parent / 'history').exists() or (parent / 'history').read_bytes() != b'version\n':
                if time.monotonic() > deadline: self.fail('CLI did not persist initial history')
                time.sleep(0.01)
            saved = self.root / 'saved'; parent.rename(saved)
            parent.symlink_to(victim, target_is_directory=True)
            # Force the rotation threshold before the next log event.
            with (saved / 'log').open('a') as stream: stream.write('x' * (1024 * 1024) + '\n')
            out, err = process.communicate(b'wrong-command\nhelp\nlogs clear\nhistory clear\nexit\n', timeout=10)
            self.assertEqual(process.returncode, 0, err)
            self.assertNotIn(b'PRIVATE_OUTSIDE_MARKER', out + err)
            for name in ('log', 'log.1', 'history'):
                self.assertEqual((victim / name).read_text(), 'PRIVATE_OUTSIDE_MARKER')
            self.assertEqual((saved / 'history').read_bytes(), b'')
            self.assertFalse((saved / 'log.1').exists())
            self.assertEqual(list(saved.glob('.nero-tmp-*')), [])
        finally:
            if process.poll() is None: process.kill(); process.communicate()

    @unittest.skipUnless(os.name == 'posix' and hasattr(os, 'geteuid'), 'POSIX elevation')
    def test_elevated_environment_uses_memory_only(self):
        if os.geteuid() != 0: self.skipTest('Fixture requires an already-root runner; never invokes sudo')
        log, history = self.root / 'log', self.root / 'history'
        self.cli(log, history, SUDO_UID='1000')
        self.assertFalse(log.exists()); self.assertFalse(history.exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows path namespace and alternate streams')
    def test_windows_device_unc_and_alternate_streams_rejected(self):
        for name in ('NUL', r'\\.\NUL', r'\\server\share\file', str(self.root / 'file') + ':stream'):
            self.cli(name, name)
        self.assertEqual(list(self.root.iterdir()), [])

    @unittest.skipUnless(os.name == 'nt', 'Windows directory ACL boundary')
    def test_windows_shared_acl_is_rejected(self):
        shared = self.root / 'shared'; shared.mkdir()
        result = subprocess.run(['icacls', str(shared), '/grant', '*S-1-1-0:(OI)(CI)F'],
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.cli(shared / 'log', shared / 'history')
        self.assertEqual(list(shared.iterdir()), [])


if __name__ == '__main__': unittest.main()
