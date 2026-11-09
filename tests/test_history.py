"""Persistence contracts against a real CLI; no remote network operations."""
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class HistoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nero-history-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "history"
        self.env = dict(os.environ, NERO_HISTORY_FILE=str(self.path),
                        NERO_LOG_FILE=":memory:", NO_COLOR="1", NERO_ASCII="1")

    def cli(self, data, **env):
        result = subprocess.run([str(BINARY)], input=data, capture_output=True,
                                env=dict(self.env, **env), timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"Sanitizer", result.stderr)
        return result.stdout + result.stderr

    def test_restart_and_clear(self):
        self.cli(b"version\nexit\n")
        self.assertEqual(self.path.read_bytes(), b"version\n")
        output = self.cli(b"history\nexit\n")
        self.assertIn(b"\nversion\nhistory", output.replace(b"\r\n", b"\n"))
        self.cli(b"history -clear\nexit\n")
        self.assertEqual(self.path.read_bytes(), b"")
        self.assertNotIn(b"\nversion\nhistory", self.cli(b"history\nexit\n"))

    def test_allowlist_excludes_secrets_and_unknown_arguments(self):
        self.cli(b"-mode off\nhelp password=secret\nunknown-token-123\nversion api_key=abc\n"
                 b"log -path bearer secret\n192.0.2.1 -Sc secret\n"
                 b"192.0.2.1-Sc\n001.2.3.4 -Sc\n+1.2.3.4 -Sc\n"
                 b"192.0.2.1 -Sc\nversion\nexit\n")
        self.assertEqual(self.path.read_bytes(), b"-mode off\n192.0.2.1 -Sc\nversion\n")

    def test_readable_aliases_preserve_privacy_allowlist_and_clear(self):
        self.cli(b"network disable\nhelp --all\nlogs path\nscan 192.0.2.1\n"
                 b"scan 001.2.3.4\nscan 192.0.2.1 secret\nlogs password=secret\nexit\n")
        self.assertEqual(self.path.read_bytes(),
                         b"network disable\nhelp --all\nlogs path\nscan 192.0.2.1\n")
        self.cli(b"history clear\nexit\n")
        self.assertEqual(self.path.read_bytes(), b"")

    def test_bounded_history_and_malformed_import(self):
        self.path.write_bytes(b"help\n" * 20000 + b"version\x1b[31m\nhelp\x00\n"
                              b"unknown-secret\nversion\nhelp")
        if os.name != "nt": self.path.chmod(0o600)
        self.cli(b"version\nexit\n")
        lines = self.path.read_bytes().splitlines()
        self.assertEqual(len(lines), 256)
        self.assertEqual(lines[-2:], [b"version", b"version"])
        self.assertTrue(all(line in (b"version", b"help") for line in lines))
        self.assertLessEqual(self.path.stat().st_size, 256 * 256)

    def test_memory_opt_out_and_pipe_default(self):
        self.cli(b"version\nexit\n", NERO_HISTORY_FILE=":memory:")
        self.assertFalse(self.path.exists())
        env = dict(self.env, HOME=self.temp.name, USERPROFILE=self.temp.name)
        env.pop("NERO_HISTORY_FILE")
        result = subprocess.run([str(BINARY)], input=b"version\nexit\n",
                                capture_output=True, env=env, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertFalse((Path(self.temp.name) / ".nero_history").exists())

    def test_storage_failure_does_not_block_session(self):
        output = self.cli(b"version\nhistory\nexit\n",
                          NERO_HISTORY_FILE=str(self.path / "missing-parent"))
        self.assertIn(b"History storage unavailable", output)
        self.assertIn(b"\nversion\nhistory", output.replace(b"\r\n", b"\n"))

    @unittest.skipIf(os.name == "nt", "POSIX permission and file-type contract")
    def test_permissions_links_special_files_and_busy_lock(self):
        import fcntl
        self.cli(b"version\nexit\n")
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        original = self.path.read_bytes()
        self.path.chmod(0o644)
        self.assertIn(b"History storage unavailable", self.cli(b"help\nexit\n"))
        self.assertEqual(self.path.read_bytes(), original)
        self.path.chmod(0o600)
        with self.path.open("rb") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertIn(b"History storage unavailable", self.cli(b"help\nexit\n"))
        target = self.path.with_name("target")
        self.path.rename(target)
        self.path.symlink_to(target)
        self.cli(b"help\nhistory -clear\nexit\n")
        self.assertEqual(target.read_bytes(), original)
        self.path.unlink()
        os.link(target, self.path)
        self.cli(b"help\nexit\n")
        self.assertEqual(target.read_bytes(), original)
        self.path.unlink()
        os.mkfifo(self.path)
        self.assertIn(b"History storage unavailable", self.cli(b"version\nexit\n"))

    def test_concurrent_session_does_not_resurrect_cleared_history(self):
        proc = subprocess.Popen([str(BINARY)], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env)
        try:
            proc.stdin.write(b"version\n")
            proc.stdin.flush()
            deadline = time.monotonic() + 5
            while True:
                try:
                    if b"version\n" in self.path.read_bytes(): break
                except (FileNotFoundError, PermissionError):
                    pass  # Windows writer holds an exclusive handle briefly.
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            self.cli(b"history -clear\nexit\n")
            stdout, stderr = proc.communicate(b"help\nexit\n", timeout=10)
            self.assertEqual(proc.returncode, 0, stderr)
            self.assertEqual(self.path.read_bytes(), b"help\n")
        finally:
            if proc.poll() is None: proc.kill()
            proc.communicate()


unittest.main()
