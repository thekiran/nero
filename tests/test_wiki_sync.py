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


if __name__ == "__main__":
    unittest.main()
