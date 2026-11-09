"""Automated tests for GitHub Wiki synchronization tooling."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from scripts.docs.wiki_sync import (
    PAGE_MAP,
    REPO_ROOT,
    check_wiki_docs,
    generate_wiki_pages,
    get_footer_content,
    get_sidebar_content,
    sync_wiki_repo,
    transform_markdown,
)


class TestWikiSync(unittest.TestCase):
    def test_all_canonical_sources_exist(self):
        """Every file mapped in PAGE_MAP must exist in the repository."""
        for src in PAGE_MAP.keys():
            path = REPO_ROOT / src
            self.assertTrue(path.exists(), f"Source file '{src}' missing from repo.")

    def test_markdown_transformation(self):
        """Link transformations must convert repo paths to Wiki page names or blob URLs."""
        sample_md = (
            "Check [Install](installation.md) and [Arch](architecture.md#cli).\n"
            "External: [GitHub](https://github.com).\n"
            "Source file: [main.c](../src/main.c).\n"
            "Anchor only: [Top](#overview).\n"
        )
        transformed = transform_markdown("docs/building.md", sample_md)
        self.assertIn("[Install](Installation)", transformed)
        self.assertIn("[Arch](Architecture#cli)", transformed)
        self.assertIn("[GitHub](https://github.com)", transformed)
        self.assertIn("https://github.com/thekiran/nero/blob/main/src/main.c", transformed)
        self.assertIn("[Top](#overview)", transformed)

    def test_check_mode_succeeds(self):
        """The canonical repository documentation must pass audit."""
        self.assertTrue(check_wiki_docs())

    def test_sidebar_and_footer_structure(self):
        """Sidebar must link all primary documentation pages."""
        sidebar = get_sidebar_content()
        footer = get_footer_content()
        for wiki_name in PAGE_MAP.values():
            self.assertTrue(
                wiki_name in sidebar or wiki_name in footer or wiki_name == "Home",
                f"Page '{wiki_name}' missing from navigation sidebar.",
            )

    def test_local_git_sync_and_unmanaged_preservation(self):
        """Sync must be idempotent and preserve unmanaged manual Wiki pages."""
        temp_dir = Path(tempfile.mkdtemp(prefix="nero_wiki_test_"))
        try:
            # Initialize a fake Git repository representing the Wiki
            subprocess.check_call(["git", "init", "-b", "master"], cwd=temp_dir)
            subprocess.check_call(
                ["git", "config", "user.name", "Test Runner"], cwd=temp_dir
            )
            subprocess.check_call(
                ["git", "config", "user.email", "test@example.com"], cwd=temp_dir
            )

            # Create an unmanaged manual Wiki page
            unmanaged = temp_dir / "User-Custom-Page.md"
            unmanaged.write_text("# Custom Page\nManual notes.", encoding="utf-8")
            subprocess.check_call(["git", "add", "User-Custom-Page.md"], cwd=temp_dir)
            subprocess.check_call(
                ["git", "commit", "-m", "Initial custom wiki content"], cwd=temp_dir
            )

            initial_commit = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=temp_dir, text=True
            ).strip()

            # First sync: should write all pages and commit
            self.assertEqual(sync_wiki_repo(temp_dir, push=False), "committed")

            new_commit = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=temp_dir, text=True
            ).strip()
            self.assertNotEqual(initial_commit, new_commit)
            self.assertTrue(unmanaged.exists(), "Unmanaged page was deleted!")
            self.assertEqual(
                unmanaged.read_text(encoding="utf-8"), "# Custom Page\nManual notes."
            )
            self.assertTrue((temp_dir / "Home.md").exists())
            self.assertTrue((temp_dir / "_Sidebar.md").exists())
            self.assertTrue((temp_dir / "_Footer.md").exists())

            # Second sync: idempotent check, must not create an empty commit
            self.assertEqual(sync_wiki_repo(temp_dir, push=False), "unchanged")
            latest_commit = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=temp_dir, text=True
            ).strip()
            self.assertEqual(new_commit, latest_commit, "Sync is not idempotent!")

        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)



class TestWikiCredentials(unittest.TestCase):
    def test_secret_never_enters_arguments_files_or_diagnostics(self):
        from contextlib import redirect_stdout, redirect_stderr
        import io
        from unittest.mock import patch
        from scripts.docs.wiki_publish import publish
        sentinel = "DUMMY-token:/+%40-secret"
        for failure in (0, 1, None):
            calls = []
            output = io.StringIO()
            def fake_run(args, **kwargs):
                calls.append(args)
                self.assertNotIn(sentinel, repr(args))
                self.assertEqual(kwargs['env']['WIKI_TOKEN'], sentinel)
                self.assertEqual(kwargs['stdout'], subprocess.DEVNULL)
                self.assertEqual(kwargs['stderr'], subprocess.DEVNULL)
                helper = Path(kwargs['env']['GIT_ASKPASS'])
                self.assertNotIn(sentinel, helper.read_text())
                for path in helper.parent.rglob('*'):
                    if path.is_file():
                        self.assertNotIn(sentinel.encode(), path.read_bytes())
                return subprocess.CompletedProcess(args, int(len(calls) - 1 == failure))
            with patch('scripts.docs.wiki_publish.subprocess.run', side_effect=fake_run):
                with redirect_stdout(output), redirect_stderr(output):
                    if failure is None:
                        publish('owner/repo', sentinel)
                    else:
                        with self.assertRaises(RuntimeError) as caught:
                            publish('owner/repo', sentinel)
                        self.assertNotIn(sentinel, str(caught.exception))
            self.assertNotIn(sentinel, output.getvalue())
            self.assertEqual(calls[0][3], 'https://github.com/owner/repo.wiki.git')
            self.assertFalse(Path(calls[0][4]).exists())

    def test_local_publication_push_and_remote_config(self):
        """Exercise real clone/sync/push offline; inspect the actual .git config."""
        from unittest.mock import patch
        from scripts.docs.wiki_publish import OUTCOMES, publish
        real_run = subprocess.run
        sentinel = 'DUMMY-not-a-real-token-1905'
        with tempfile.TemporaryDirectory() as directory:
            remote = Path(directory) / 'wiki.git'
            subprocess.check_call(['git', 'init', '--bare', str(remote)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            configs = []
            def local_run(args, **kwargs):
                env = dict(kwargs['env'])
                env.update(GIT_CONFIG_COUNT='3',
                           GIT_CONFIG_KEY_2='url.' + str(remote) + '.insteadOf',
                           GIT_CONFIG_VALUE_2='https://github.com/owner/repo.wiki.git')
                kwargs['env'] = env
                result = real_run(args, **kwargs)
                clone = Path(args[4]) if args[0] == 'git' else Path(args[-2])
                if (clone / '.git/config').exists():
                    configs.append((clone / '.git/config').read_text())
                return result
            with patch('scripts.docs.wiki_publish.subprocess.run', side_effect=local_run):
                first = publish('owner/repo', sentinel)
                second = publish('owner/repo', sentinel)
            self.assertEqual(first, OUTCOMES['pushed'])
            self.assertEqual(second, OUTCOMES['unchanged'])
            # Two publications, each inspected after clone and after sync/push.
            self.assertEqual(len(configs), 4)
            for config in configs:
                self.assertIn('https://github.com/owner/repo.wiki.git', config)
                self.assertNotIn(sentinel, config)
                self.assertNotIn('extraheader', config.lower())
            pages = subprocess.check_output(['git', '--git-dir', str(remote),
                                             'ls-tree', '--name-only', 'HEAD'], text=True)
            self.assertIn('Home.md', pages)

    def test_askpass_and_transport_failure_do_not_echo_credentials(self):
        import os
        from unittest.mock import patch
        from scripts.docs.wiki_publish import publish
        real_run = subprocess.run
        sentinel = 'DUMMY-sensitive+/encoded'
        def failed_transport(args, **kwargs):
            helper = kwargs['env']['GIT_ASKPASS']
            for prompt, expected in [('Username', 'x-access-token'), ('Password', sentinel)]:
                reply = real_run([helper, prompt], env=kwargs['env'], capture_output=True,
                                 text=True, check=True)
                self.assertEqual(reply.stdout.strip(), expected)
            # Simulate a transport that echoes a credential in both streams.
            return real_run(['python3', '-c',
                             'import os,sys; print(os.environ["WIKI_TOKEN"]); '
                             'print(os.environ["WIKI_TOKEN"], file=sys.stderr); sys.exit(1)'], **kwargs)
        with patch('scripts.docs.wiki_publish.subprocess.run', side_effect=failed_transport):
            with self.assertRaisesRegex(RuntimeError, 'Wiki clone failed'):
                publish('owner/repo', sentinel)

    @unittest.skipIf(os.name == 'nt', 'POSIX shell fake git')
    def test_publish_process_never_prints_echoed_credentials(self):
        """Run wiki_publish.py for real with a fake failing git first on PATH."""
        sentinel = 'DUMMY-e2e-sentinel+/%40-not-a-token'
        script = REPO_ROOT / 'scripts/docs/wiki_publish.py'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake_bin = root / 'bin'
            fake_bin.mkdir()
            marker = root / 'fake-git-ran'
            fake_git = fake_bin / 'git'
            # Echo the credential (from the environment and as a literal) on
            # both streams, record the call, then fail like a broken transport.
            fake_git.write_text(
                '#!/bin/sh\n'
                f': > "{marker}"\n'
                'printf "%s\\n" "$WIKI_TOKEN"\n'
                'printf "%s\\n" "$WIKI_TOKEN" >&2\n'
                f"printf '%s\\n' '{sentinel}'\n"
                f"printf '%s\\n' '{sentinel}' >&2\n"
                'exit 1\n', encoding='utf-8')
            fake_git.chmod(0o700)
            summary = root / 'summary.md'
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(('GIT_', 'WIKI_', 'GITHUB_'))}
            env.update(PATH=str(fake_bin) + os.pathsep + env.get('PATH', ''),
                       WIKI_TOKEN=sentinel, GITHUB_REPOSITORY='owner/repo',
                       GITHUB_STEP_SUMMARY=str(summary))
            result = subprocess.run([sys.executable, str(script)], cwd=root, env=env,
                                    capture_output=True, timeout=60, check=False)
            self.assertTrue(marker.exists(), 'fake git was not the git that ran')
            self.assertNotEqual(result.returncode, 0)
            for stream in (result.stdout, result.stderr, summary.read_bytes()):
                self.assertNotIn(sentinel.encode(), stream)
            self.assertIn(b'Wiki clone failed', result.stderr)
            self.assertEqual(summary.read_text(encoding='utf-8').count('\n'), 1)
            self.assertIn('Wiki clone failed', summary.read_text(encoding='utf-8'))

    def test_missing_credentials_and_invalid_repository_fail_closed(self):
        from scripts.docs.wiki_publish import publish
        for repo, token in [('owner/repo', ''), ('owner/repo\n--evil', 'dummy')]:
            with self.assertRaises(ValueError):
                publish(repo, token)

    def test_workflow_keeps_credentials_out_of_pr_validation(self):
        workflow = (REPO_ROOT / '.github/workflows/wiki-sync.yml').read_text()
        validate, publish = workflow.split('\n  publish:', 1)
        self.assertNotIn('secrets.', validate)
        self.assertIn("github.event_name != 'pull_request' && github.ref == 'refs/heads/main'", publish)
        self.assertNotIn('x-access-token:', workflow)
        self.assertNotIn('cat clone_err', workflow)


if __name__ == "__main__":
    unittest.main()
