"""Optional, non-gating source baseline hash audit.

This is an AUDIT, not a quality gate. Byte-level changes to tracked source
files do NOT, on their own, fail CI. The real quality gates are the unit,
integration, regression, sanitizer and platform-compatibility tests.

Its job is to surface *drift* between the tracked source files and the
recorded baseline manifests so it can be reviewed:

  * Intentional edits are recorded in tests/local-fixes-baseline.json
    (changes: before/after/reason). A recorded edit is not drift.
  * Intentional deletions are recorded there too (removed: [paths]). A
    recorded deletion is not drift; the audit checks the file is really gone.
  * Anything else -- an unrecorded edit, or a tracked file that vanished
    without being recorded as removed -- is reported here (non-zero exit) so
    a human can either record it or revert it.

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
        local_fixes = json.loads((ROOT / "tests/local-fixes-baseline.json").read_text())
        fixes = local_fixes["changes"]
        removed = set(local_fixes.get("removed", []))

        def is_removed(path):
            """A path recorded as intentionally removed must be absent on disk."""
            if path in removed:
                self.assertFalse((ROOT / path).exists(),
                                 f"{path} is recorded as removed but still present")
                return True
            return False

        def digest(path):
            """Hash a tracked file, failing clearly if it vanished unrecorded."""
            full = ROOT / path
            self.assertTrue(full.exists(),
                            f"{path} is tracked but missing; record it under "
                            f"'removed' in tests/local-fixes-baseline.json or restore it")
            return hashlib.sha256(full.read_bytes()).hexdigest()

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
                if is_removed(entry["path"]):
                    continue
                self.assertEqual(entry["original"], baseline["files"][original])
                expected = entry["after"]
                if entry["path"] in changes:
                    change = changes[entry["path"]]
                    self.assertEqual(change["before"], expected)
                    expected = change["after"]
                self.assertEqual(digest(entry["path"]),
                                 translated_digest(entry["path"], expected))
                if original != entry["path"]:
                    self.assertFalse((ROOT / original).exists())

        for path, change in changes.items():
            with self.subTest(changed=path):
                if is_removed(path):
                    continue
                self.assertTrue(change["reason"])
                self.assertEqual(digest(path), translated_digest(path, change["after"]))
        for path, digest_value in readability["added"].items():
            with self.subTest(added=path):
                if is_removed(path):
                    continue
                self.assertEqual(digest(path), translated_digest(path, digest_value))

        for path, change in english.items():
            with self.subTest(translated=path):
                if is_removed(path):
                    continue
                self.assertTrue(change["reason"])
                self.assertEqual(digest(path), fixed_digest(path, change["after"]))

        for path, change in fixes.items():
            with self.subTest(fixed=path):
                if is_removed(path):
                    continue
                self.assertTrue(change["reason"])
                self.assertEqual(digest(path), change["after"])


if __name__ == "__main__":
    unittest.main()
