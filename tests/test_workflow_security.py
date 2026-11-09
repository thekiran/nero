"""Fail closed on credential/permission regressions in checked-in workflows."""
from pathlib import Path
import re
import unittest
ROOT = Path(__file__).resolve().parents[1]

class WorkflowSecurity(unittest.TestCase):
    def test_external_actions_are_immutable_and_defaults_read_only(self):
        for path in (ROOT / '.github/workflows').glob('*.yml'):
            text = path.read_text()
            with self.subTest(workflow=path.name):
                self.assertRegex(text, r'(?m)^permissions:\n  contents: read\n')
                self.assertNotIn('pull_request_target:', text)
                for reference in re.findall(r'uses:\s*([^\s#]+)', text):
                    self.assertTrue(reference.startswith('./') or
                                    re.fullmatch(r'[^@]+@[0-9a-f]{40}', reference), reference)

    def test_release_write_permission_is_in_trusted_publish_job_only(self):
        text = (ROOT / '.github/workflows/release.yml').read_text()
        matches = list(re.finditer(r'(?m)^  ([a-z][a-z0-9-]*):\n', text))
        jobs = {m.group(1): text[m.start(): matches[i+1].start() if i+1 < len(matches) else len(text)]
                for i, m in enumerate(matches)}
        self.assertNotIn('contents: write', jobs['draft'])
        privileged = [name for name, body in jobs.items() if 'contents: write' in body]
        self.assertEqual(privileged, ['publish'])
        self.assertIn("github.event_name != 'pull_request'", jobs['publish'])
        self.assertIn("github.ref == 'refs/heads/main'", jobs['publish'])
        self.assertIn("startsWith(github.ref, 'refs/tags/v')", jobs['publish'])
        self.assertIn('sha256sum --check SHA256SUMS', jobs['publish'])
        self.assertIn('needs: [prepare, draft]', jobs['publish'])
        self.assertIn('verified-release-set', jobs['draft'])
        self.assertIn('verified-release-set', jobs['publish'])
        self.assertIn('test "$PUBLICATION" = success', jobs['release-checks'])
        self.assertIn('test "$PUBLICATION" = skipped', jobs['release-checks'])

if __name__ == '__main__':
    unittest.main()
