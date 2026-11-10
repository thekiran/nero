"""Fail closed on credential/permission regressions in checked-in workflows."""
from pathlib import Path
import itertools
import json
import os
import re
import unittest
ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / '.github/workflows'
# Job-level write scopes that are deliberately granted: (workflow, job, scope).
ALLOWED_WRITES = {('release.yml', 'publish', 'contents'),
                  ('security.yml', 'codeql', 'security-events')}
ACTIONS_APP_ID = 15368
# GitHub Advanced Security reports the PR code-scanning result as "CodeQL".
CODE_SCANNING_RESULT = ('CodeQL', 57789)


def split_jobs(text):
    """Return (workflow-level text, {job id: job text}) for a workflow file."""
    top = re.search(r'(?m)^jobs:[ \t]*\n', text)
    if not top:
        return text, {}
    body = text[top.end():]
    end = re.search(r'(?m)^\S', body)
    rest = body[end.start():] if end else ''
    body = body[:end.start()] if end else body
    matches = list(re.finditer(r'(?m)^  ([A-Za-z_][A-Za-z0-9_-]*):[ \t]*\n', body))
    jobs = {m.group(1): body[m.start(): matches[i+1].start() if i+1 < len(matches) else len(body)]
            for i, m in enumerate(matches)}
    return text[:top.start()] + rest, jobs


def permission_grants(text):
    """Yield (scope, level) for every permissions mapping or shorthand in text."""
    for m in re.finditer(r'(?m)^([ \t]*)permissions:[ \t]*([^\n#]*?)[ \t]*(?:#.*)?$', text):
        if m.group(2):
            yield '*', m.group(2).strip('\'"')
            continue
        for line in text[m.end():].split('\n')[1:]:
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            if len(line) - len(line.lstrip()) <= len(m.group(1)):
                break
            scope, _, level = line.strip().partition(':')
            yield scope.strip(), level.split('#')[0].strip().strip('\'"')


def scalar_list(value):
    value = value.split('#')[0].strip()
    if value.startswith('[') and value.endswith(']'):
        return [item.strip().strip('\'"') for item in value[1:-1].split(',') if item.strip()]
    return None


def matrix_combinations(job):
    """Expand the simple matrix shapes used in this repository (fail closed otherwise)."""
    m = re.search(r'(?m)^( +)matrix:[ \t]*\n', job)
    if not m:
        return [{}]
    indent = len(m.group(1))
    lines = []
    for line in job[m.end():].split('\n'):
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        if line.strip() and not line.lstrip().startswith('#'):
            lines.append(line)
    axes, include, current = {}, [], None
    key_indent = len(lines[0]) - len(lines[0].lstrip()) if lines else 0
    for line in lines:
        depth, stripped = len(line) - len(line.lstrip()), line.strip()
        if depth == key_indent:
            key, _, value = stripped.partition(':')
            if key == 'include' and not value.strip():
                current = include
                continue
            values = scalar_list(value)
            if values is None:
                raise AssertionError(f'unsupported matrix entry: {stripped}')
            axes[key], current = values, None
        elif current is include:
            if stripped.startswith('- '):
                include.append({})
                stripped = stripped[2:]
            key, _, value = stripped.partition(':')
            include[-1][key.strip()] = value.split('#')[0].strip().strip('\'"')
        else:
            raise AssertionError(f'unsupported matrix entry: {stripped}')
    if axes and include:
        raise AssertionError('matrix mixes axes and include; extend this parser deliberately')
    if include:
        return include
    keys = list(axes)
    return [dict(zip(keys, combo)) for combo in itertools.product(*(axes[k] for k in keys))]


def job_display_names(job_id, job):
    """Check-run names GitHub reports for a job, including matrix expansion."""
    m = re.search(r'(?m)^    name:[ \t]*(.+?)[ \t]*$', job)
    template = m.group(1).strip('\'"') if m else None
    names = set()
    for combo in matrix_combinations(job):
        if template is None:
            names.add(f"{job_id} ({', '.join(combo.values())})" if combo else job_id)
        else:
            names.add(re.sub(r'\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}',
                             lambda ref: combo[ref.group(1)], template))
    return names


def container_files(root):
    """Dockerfiles/Containerfiles in the source tree (hidden and build dirs skipped)."""
    for directory, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ('build', 'node_modules') and
                   (not d.startswith('.') or d == '.github')]
        for name in files:
            if (name.startswith(('Dockerfile', 'Containerfile')) or
                    name.endswith(('.Dockerfile', '.dockerfile', '.Containerfile'))):
                yield Path(directory) / name

class WorkflowSecurity(unittest.TestCase):
    def test_release_source_and_cache_boundaries(self):
        text = (WORKFLOWS / 'release.yml').read_text()
        top, jobs = split_jobs(text)
        self.assertRegex(top, r'(?m)^cache-mode: none$')
        self.assertNotRegex(text, r'(?m)^ +cache-mode:')
        self.assertNotIn('ref: ${{ needs.prepare.outputs.commit }}', text)
        for job in ('prepare', 'package', 'windows', 'draft', 'publish'):
            self.assertIn('ref: ${{ github.sha }}', jobs[job])
            self.assertIn('persist-credentials: false', jobs[job])
        self.assertIn('Requested tag must match the triggering revision', jobs['prepare'])
        self.assertIn('git merge-base --is-ancestor "$commit" refs/remotes/origin/main', jobs['prepare'])
        self.assertIn("github.event_name != 'workflow_dispatch' || github.ref == 'refs/heads/main'", jobs['prepare'])
        for job, body in jobs.items():
            if job != 'publish':
                self.assertNotIn('secrets.', body)
                self.assertNotIn('contents: write', body)

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

    def test_write_permissions_are_job_scoped_and_allowlisted(self):
        granted = set()
        workflows = sorted(WORKFLOWS.glob('*.yml'))
        self.assertTrue(workflows)
        for path in workflows:
            text = path.read_text()
            top, jobs = split_jobs(text)
            with self.subTest(workflow=path.name):
                self.assertTrue(jobs)
                self.assertNotIn('write-all', text)
                for scope, level in permission_grants(top):
                    self.assertNotEqual(level, 'write', f'workflow-level {scope}: write')
                    self.assertNotEqual(scope, '*', f'workflow-level shorthand {level}')
                for job_id, body in jobs.items():
                    for scope, level in permission_grants(body):
                        self.assertIn(level, ('read', 'none', 'write', 'read-all', '{}'),
                                      f'{job_id}: {scope}: {level}')
                        if level == 'write':
                            self.assertIn((path.name, job_id, scope), ALLOWED_WRITES)
                            granted.add((path.name, job_id, scope))
        self.assertEqual(granted, ALLOWED_WRITES)

    def test_required_checks_match_real_workflow_jobs(self):
        names = set()
        for path in WORKFLOWS.glob('*.yml'):
            for job_id, body in split_jobs(path.read_text())[1].items():
                names |= job_display_names(job_id, body)
        protection = json.loads((ROOT / '.github/main-protection.json').read_text())
        checks = protection['required_status_checks']['checks']
        self.assertTrue(checks)
        self.assertNotIn('contexts', protection['required_status_checks'])
        contexts = [check['context'] for check in checks]
        self.assertEqual(len(contexts), len(set(contexts)))
        for check in checks:
            with self.subTest(context=check['context']):
                if (check['context'], check.get('app_id')) == CODE_SCANNING_RESULT:
                    self.assertIn('github/codeql-action/analyze@',
                                  (WORKFLOWS / 'security.yml').read_text())
                    continue
                self.assertEqual(check.get('app_id'), ACTIONS_APP_ID)
                self.assertIn(check['context'], names)

    def test_container_base_images_are_digest_pinned(self):
        digest = r'@sha256:[0-9a-f]{64}'
        files = list(container_files(ROOT))
        self.assertIn(ROOT / 'tests/linux/Dockerfile', files)
        for path in files:
            stages = set()
            for line in path.read_text().splitlines():
                m = re.match(r'(?i)\s*FROM\s+(?:--\S+\s+)*(\S+)(?:\s+AS\s+(\S+))?', line)
                if not m:
                    continue
                with self.subTest(dockerfile=str(path.relative_to(ROOT)), image=m.group(1)):
                    if m.group(1).lower() != 'scratch' and m.group(1).lower() not in stages:
                        self.assertRegex(m.group(1), r'^[^\s@]+' + digest + '$')
                if m.group(2):
                    stages.add(m.group(2).lower())
        for path in WORKFLOWS.glob('*.yml'):
            for image in re.findall(r'(?m)^\s*(?:-\s*)?(?:image|container):[ \t]*([^\s#{}]+)',
                                    path.read_text()):
                with self.subTest(workflow=path.name, image=image):
                    self.assertRegex(image.strip('\'"'), digest + r'$')

if __name__ == '__main__':
    unittest.main()
