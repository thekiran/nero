"""Read-only baseline integrity and offline platform report contracts."""
import hashlib
import json
import pathlib
import platform
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
REPORT = pathlib.Path(sys.argv.pop(1)).resolve()


class FoundationTests(unittest.TestCase):
    def test_preserved_source_and_build_contract(self):
        baseline = json.loads((ROOT / "tests/baseline.json").read_text())
        layout = json.loads((ROOT / "tests/layout-baseline.json").read_text())["files"]
        readability = json.loads((ROOT / "tests/readability-baseline.json").read_text())
        changes = readability["changes"]
        english = json.loads((ROOT / "tests/english-baseline.json").read_text())["changes"]

        def translated_digest(path, expected):
            if path in english:
                self.assertEqual(english[path]["before"], expected)
                return english[path]["after"]
            return expected

        self.assertEqual(set(layout), set(baseline["files"]))
        destinations = [entry["path"] for entry in layout.values()]
        self.assertEqual(len(destinations), len(set(destinations)))
        for original, entry in layout.items():
            with self.subTest(path=entry["path"]):
                self.assertEqual(entry["original"], baseline["files"][original])
                expected = entry["after"]
                if entry["path"] in changes:
                    change = changes[entry["path"]]
                    self.assertEqual(change["before"], expected)
                    expected = change["after"]
                self.assertEqual(hashlib.sha256((ROOT / entry["path"]).read_bytes()).hexdigest(),
                                 translated_digest(entry["path"], expected))
                if original != entry["path"]:
                    self.assertFalse((ROOT / original).exists())

        for path, change in changes.items():
            with self.subTest(changed=path):
                self.assertTrue(change["reason"])
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(),
                                 translated_digest(path, change["after"]))
        for path, digest in readability["added"].items():
            with self.subTest(added=path):
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(),
                                 translated_digest(path, digest))

        for path, change in english.items():
            with self.subTest(translated=path):
                self.assertTrue(change["reason"])
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(),
                                 change["after"])

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
