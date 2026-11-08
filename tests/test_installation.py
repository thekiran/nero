"""Installer lifecycle in a disposable prefix, including a failed upgrade."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipIf(os.name == "nt", "POSIX installer")
class InstallationTests(unittest.TestCase):
    def test_install_upgrade_failure_uninstall_and_data_retention(self):
        with tempfile.TemporaryDirectory(prefix="nero install ") as tmp:
            base = Path(tmp)
            package = base / "package"
            (package / "bin").mkdir(parents=True)
            for name in ("install.sh", "uninstall.sh"):
                shutil.copy2(ROOT / "scripts" / name, package / name)
            executable = package / "bin/nero"
            staging = base / "staging"
            env = dict(os.environ, PREFIX="/usr/local", DESTDIR=str(staging))
            command = staging / "usr/local/bin/nero"
            sentinel = staging / "usr/local/bin/keep-me"
            for version in ("first", "upgraded"):
                executable.write_text("#!/bin/sh\nprintf '%s\\n' '" + version + "'\n")
                executable.chmod(0o755)
                subprocess.run(["sh", str(package / "install.sh")], env=env, cwd=base, check=True, capture_output=True)
                self.assertEqual(subprocess.check_output([str(command)], cwd=base).strip(), version.encode())
            sentinel.write_text("unrelated")
            history = base / ".nero_history"
            history.write_text("version\n")
            executable.unlink()
            failed = subprocess.run(["sh", str(package / "install.sh")], env=env, cwd=base, capture_output=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(subprocess.check_output([str(command)]).strip(), b"upgraded")
            self.assertEqual(list(command.parent.glob(".nero-install.*")), [])
            # Existing destination symlinks must be replaced, not followed.
            command.unlink()
            command.symlink_to(sentinel)
            executable.write_text("#!/bin/sh\nexit 0\n")
            subprocess.run(["sh", str(package / "install.sh")], env=env, check=True, capture_output=True)
            self.assertFalse(command.is_symlink())
            self.assertEqual(sentinel.read_text(), "unrelated")
            for _ in range(2):
                subprocess.run(["sh", str(package / "uninstall.sh")], env=env, check=True, capture_output=True)
            self.assertFalse(command.exists())
            self.assertEqual(history.read_text(), "version\n")
            self.assertEqual(sentinel.read_text(), "unrelated")


if __name__ == "__main__": unittest.main()
