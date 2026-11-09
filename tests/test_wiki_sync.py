"""Automated tests for GitHub Wiki synchronization tooling."""
from pathlib import Path
import shutil
import subprocess
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
            sync_wiki_repo(temp_dir, push=False)

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
            sync_wiki_repo(temp_dir, push=False)
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
