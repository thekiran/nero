"""Optional, non-gating source baseline hash audit.

This is an AUDIT, not a quality gate. Byte-level changes to tracked source
files do NOT, on their own, fail CI. The real quality gates are the unit,
integration, regression, sanitizer and platform-compatibility tests.

Its job is to surface *drift* between the tracked source files and the
recorded baseline manifests so it can be reviewed:

  * Intentional changes are recorded in tests/local-fixes-baseline.json
    (before/after/reason). A recorded change is not drift.
  * Unrecorded drift is reported here (non-zero exit) so a human can either
    record it (add a before/after/reason entry) or revert it.

Run locally:      python3 tests/audit/source_baseline_audit.py
Run via CMake:    cmake -DNERO_SOURCE_AUDIT=ON <build> && ctest -R source-baseline-audit
In CI it runs as a non-blocking, informational step (continue-on-error).
"""
import hashlib
import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class SourceBaselineAudit(unittest.TestCase):
    def test_tracked_sources_match_recorded_baseline(self):
        baseline = json.loads((ROOT / "tests/baseline.json").read_text())
        layout = json.loads((ROOT / "tests/layout-baseline.json").read_text())["files"]
        readability = json.loads((ROOT / "tests/readability-baseline.json").read_text())
        changes = readability["changes"]
        english = json.loads((ROOT / "tests/english-baseline.json").read_text())["changes"]
        fixes = json.loads((ROOT / "tests/local-fixes-baseline.json").read_text())["changes"]

        def fixed_digest(path, expected):
            if path in fixes:
                self.assertEqual(fixes[path]["before"], expected)
                return fixes[path]["after"]
            return expected

        def translated_digest(path, expected):
            if path in english:
                self.assertEqual(english[path]["before"], expected)
                expected = english[path]["after"]
            return fixed_digest(path, expected)

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
                                 fixed_digest(path, change["after"]))

        for path, change in fixes.items():
            with self.subTest(fixed=path):
                self.assertTrue(change["reason"])
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(),
                                 change["after"])


if __name__ == "__main__":
    unittest.main()
