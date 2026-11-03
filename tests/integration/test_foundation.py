"""Offline platform report contract (behavioral quality gate).

The source baseline hash check that used to live here has moved to the
optional, non-gating audit at tests/audit/source_baseline_audit.py. This file
now asserts only observable behavior of the offline platform report, so a
legitimate source change never fails this gate on its own.
"""
import pathlib
import platform
import subprocess
import sys
import unittest

REPORT = pathlib.Path(sys.argv.pop(1)).resolve()


class FoundationTests(unittest.TestCase):
    def test_platform_report(self):
        run = subprocess.run([str(REPORT)], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        expected_os = {"Darwin": "macOS", "Linux": "Linux", "Windows": "Windows"}[platform.system()]
        self.assertIn("Platform: " + expected_os, run.stdout)
        arch = platform.machine().lower()
        expected_arch = {"arm64": "ARM64", "aarch64": "ARM64", "amd64": "x86_64", "x86_64": "x86_64"}[arch]
        self.assertIn("Architecture: " + expected_arch, run.stdout)
        self.assertIn("LABORATORY USE ONLY", run.stdout)
        self.assertIn("RESTRICTED TO LABORATORY USE", run.stdout)
        self.assertIn("Traffic generation: Unsupported", run.stdout)
        self.assertIn("Port scanning: Unsupported", run.stdout)
        if platform.system() == "Windows":
            self.assertIn("Development CLI: Unsupported", run.stdout)
            self.assertIn("Interface listing: Unsupported", run.stdout)


unittest.main()
