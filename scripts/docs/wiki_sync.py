#!/usr/bin/env python3
"""Automated GitHub Wiki synchronization for the NERO project.

Translates canonical documentation files under docs/ and the repository root
into GitHub Wiki format, generates navigation (_Sidebar.md and _Footer.md),
converts relative repository links to Wiki page names or GitHub blob URLs,
and supports idempotent, safe synchronization to the Wiki Git repository.

Usage:
    python3 scripts/docs/wiki_sync.py --check
    python3 scripts/docs/wiki_sync.py --build <output-directory>
    python3 scripts/docs/wiki_sync.py --sync <wiki-git-directory> [--push]
        [--status-file <path>]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

REPO_ROOT = Path(__file__).resolve().parents[2]
REPO_URL = "https://github.com/thekiran/nero"
RAW_BASE_URL = "https://raw.githubusercontent.com/thekiran/nero/main"
BLOB_BASE_URL = "https://github.com/thekiran/nero/blob/main"

# Explicit mapping between canonical repository source files and Wiki page names.
# All Wiki page filenames will be <WikiName>.md.
PAGE_MAP = {
    "docs/wiki-home.md": "Home",
    "docs/installation.md": "Installation",
    "docs/command-reference.md": "Command-Reference",
    "docs/architecture.md": "Architecture",
    "docs/platform-support.md": "Platform-Compatibility",
    "docs/building.md": "Development",
    "docs/testing.md": "Testing",
    "docs/developer-contracts.md": "Developer-Contracts",
    "docs/distribution.md": "Distribution",
    "docs/troubleshooting.md": "Troubleshooting",
    "ROADMAP.md": "Roadmap",
    "docs/releases.md": "Releases",
    "CONTRIBUTING.md": "Contributing",
    "SECURITY.md": "Security",
}

MANAGED_SYSTEM_FILES = {"_Sidebar.md", "_Footer.md"}

# Fixed, non-secret outcomes reported by --sync (and --status-file).
SYNC_STATUSES = ("unchanged", "committed", "pushed")


def get_sidebar_content() -> str:
    """Generate structured navigation sidebar for the GitHub Wiki."""
    return """### Overview
* [[Home]]
* [[Installation]]
* [[Command Reference|Command-Reference]]

### Architecture & Platforms
* [[Architecture]]
* [[Platform Compatibility|Platform-Compatibility]]
* [[Releases]]

### Development & QA
* [[Development]]
* [[Testing]]
* [[Developer Contracts|Developer-Contracts]]
* [[Distribution]]
* [[Troubleshooting]]

### Project Governance
* [[Roadmap]]
* [[Contributing]]
* [[Security]]

---
[NERO Repository](https://github.com/thekiran/nero)
[Release Packages](https://github.com/thekiran/nero/releases)
[Issue Tracker](https://github.com/thekiran/nero/issues)
"""


def get_footer_content() -> str:
    """Generate canonical footer for the GitHub Wiki."""
    return """---
*NERO Documentation Mirror — Automatically synchronized from the [canonical repository](https://github.com/thekiran/nero).*<br>
[Repository](https://github.com/thekiran/nero) · [Releases](https://github.com/thekiran/nero/releases) · [MIT License](https://github.com/thekiran/nero/blob/main/LICENSE.txt)
"""


def transform_markdown(source_rel_path: str, content: str) -> str:
    """Transform canonical repository Markdown into GitHub Wiki format.

    - Resolves relative links to other canonical docs into Wiki page names.
    - Resolves relative links to code, headers, or assets into GitHub blob or raw URLs.
    - Preserves external URLs and local anchor links.
    """
    source_file = REPO_ROOT / source_rel_path
    source_dir = source_file.parent

    # Match Markdown links: [label](target)
    # Ignore escaped brackets or images handled separately
    link_pattern = re.compile(r'(?<!\!)\[([^\]]+)\]\(([^)]+)\)')

    def replace_link(match):
        text = match.group(1)
        target = match.group(2).strip()

        # External URLs, mailto, or anchor-only links remain untouched
        if re.match(r'^(?:https?://|mailto:|#)', target):
            return f"[{text}]({target})"

        # Split link and anchor
        parts = target.split('#', 1)
        link_path_str = parts[0]
        anchor = f"#{parts[1]}" if len(parts) > 1 else ""

        if not link_path_str:
            return f"[{text}]({anchor})"

        # Resolve relative target against source directory
        resolved_target = (source_dir / link_path_str).resolve()

        try:
            repo_rel = resolved_target.relative_to(REPO_ROOT).as_posix()
        except ValueError:
            # Target is outside repo root; preserve as-is
            return f"[{text}]({target})"

        # Check if target is a managed Wiki page
        if repo_rel in PAGE_MAP:
            wiki_page = PAGE_MAP[repo_rel]
            return f"[{text}]({wiki_page}{anchor})"

        # Target is a repository source/asset file -> link to GitHub blob
        blob_url = f"{BLOB_BASE_URL}/{repo_rel}{anchor}"
        return f"[{text}]({blob_url})"

    transformed = link_pattern.sub(replace_link, content)

    # Transform relative images: ![alt](target) or <img src="target">
    img_pattern = re.compile(r'\!\[([^\]]*)\]\(([^)]+)\)')

    def replace_img(match):
        alt = match.group(1)
        src = match.group(2).strip()
        if re.match(r'^(?:https?://)', src):
            return f"![{alt}]({src})"
        resolved = (source_dir / src).resolve()
        try:
            rel = resolved.relative_to(REPO_ROOT).as_posix()
            raw_url = f"{RAW_BASE_URL}/{rel}"
            return f"![{alt}]({raw_url})"
        except ValueError:
            return f"![{alt}]({src})"

    transformed = img_pattern.sub(replace_img, transformed)

    # HTML <img> tag replacement for relative sources
    html_img_pattern = re.compile(r'(<img\s+[^>]*src=["\'])(?!https?://)([^"\']+)(["\'][^>]*>)', re.IGNORECASE)

    def replace_html_img(match):
        prefix = match.group(1)
        src = match.group(2).strip()
        suffix = match.group(3)
        resolved = (source_dir / src).resolve()
        try:
            rel = resolved.relative_to(REPO_ROOT).as_posix()
            return f"{prefix}{RAW_BASE_URL}/{rel}{suffix}"
        except ValueError:
            return match.group(0)

    transformed = html_img_pattern.sub(replace_html_img, transformed)

    return transformed


def generate_wiki_pages() -> dict:
    """Generate dictionary of {filename: content} for all Wiki pages."""
    pages = {}

    for src_rel, wiki_name in PAGE_MAP.items():
        src_path = REPO_ROOT / src_rel
        if not src_path.exists():
            raise FileNotFoundError(f"Canonical source file '{src_rel}' not found.")
        raw_content = src_path.read_text(encoding="utf-8")
        transformed = transform_markdown(src_rel, raw_content)
        pages[f"{wiki_name}.md"] = transformed

    pages["_Sidebar.md"] = get_sidebar_content()
    pages["_Footer.md"] = get_footer_content()
    return pages


def check_wiki_docs() -> bool:
    """Validate that all sources exist and transformed links point to valid destinations."""
    print("Running Wiki Documentation Audit...")
    success = True

    # 1. Check source existence
    for src_rel, wiki_name in PAGE_MAP.items():
        src_path = REPO_ROOT / src_rel
        if not src_path.exists():
            print(f"ERROR: Canonical source file '{src_rel}' does not exist.")
            success = False
        else:
            print(f"  OK: {src_rel} -> {wiki_name}.md")

    # 2. Check generated pages and internal link validity
    pages = generate_wiki_pages()
    valid_wiki_names = set(PAGE_MAP.values())

    link_pattern = re.compile(r'\[([^\]]+)\]\(([^)]+)\)')

    for filename, content in pages.items():
        if filename in MANAGED_SYSTEM_FILES:
            continue
        links = link_pattern.findall(content)
        for label, target in links:
            if target.startswith("http://") or target.startswith("https://") or target.startswith("#") or target.startswith("mailto:"):
                continue
            wiki_target = target.split("#")[0]
            if wiki_target not in valid_wiki_names:
                print(f"WARNING: In '{filename}', link to '{target}' does not match any Wiki page.")
                success = False

    if success:
        print(f"PASS: All {len(PAGE_MAP)} canonical sources verified and Wiki generation validated.")
    return success


def build_wiki_pages(output_dir: Path) -> None:
    """Write generated Wiki pages to the target directory."""
    output_dir.mkdir(parents=True, exist_ok=True)
    pages = generate_wiki_pages()

    for filename, content in pages.items():
        target_path = output_dir / filename
        target_path.write_text(content, encoding="utf-8")
        print(f"Wrote {target_path}")

    print(f"\nSuccessfully generated {len(pages)} Wiki pages in {output_dir}")


def sync_wiki_repo(wiki_git_dir: Path, push: bool = False) -> str:
    """Synchronize generated pages into a local clone of the Wiki repository.

    Returns one of SYNC_STATUSES: "unchanged" when nothing differed,
    "committed" for a local-only commit, or "pushed" after a successful push.
    """
    if not (wiki_git_dir / ".git").exists():
        raise ValueError(f"'{wiki_git_dir}' is not a valid Git repository.")

    print(f"Synchronizing Wiki repository at: {wiki_git_dir}")

    # Generate pages
    pages = generate_wiki_pages()
    managed_filenames = set(pages.keys())

    # Detect existing unmanaged files to ensure they are preserved
    existing_files = {p.name for p in wiki_git_dir.glob("*.md")}
    unmanaged_files = existing_files - managed_filenames

    if unmanaged_files:
        print(f"Preserving {len(unmanaged_files)} unmanaged Wiki pages: {', '.join(sorted(unmanaged_files))}")

    # Write managed pages
    for filename, content in pages.items():
        dest = wiki_git_dir / filename
        dest.write_text(content, encoding="utf-8")

    # Check git status
    status = subprocess.check_output(
        ["git", "status", "--porcelain"], cwd=wiki_git_dir, text=True
    ).strip()

    if not status:
        print("Idempotent check: No changes detected. Wiki repository is already up to date.")
        return "unchanged"

    print("Detected changes to Wiki documentation:")
    for line in status.splitlines():
        print(f"  {line}")

    # Stage only managed files
    for filename in managed_filenames:
        target_file = wiki_git_dir / filename
        if target_file.exists():
            subprocess.check_call(["git", "add", filename], cwd=wiki_git_dir)

    # Commit
    commit_msg = "docs: automated synchronization from canonical docs/"
    env = dict(os.environ)
    env["GIT_AUTHOR_NAME"] = env.get("GIT_AUTHOR_NAME", "github-actions[bot]")
    env["GIT_AUTHOR_EMAIL"] = env.get("GIT_AUTHOR_EMAIL", "github-actions[bot]@users.noreply.github.com")
    env["GIT_COMMITTER_NAME"] = env["GIT_AUTHOR_NAME"]
    env["GIT_COMMITTER_EMAIL"] = env["GIT_AUTHOR_EMAIL"]

    subprocess.check_call(
        ["git", "commit", "-m", commit_msg], cwd=wiki_git_dir, env=env
    )
    print("Committed documentation changes to Wiki repository.")

    if push:
        print("Pushing updates to origin (never using force push)...")
        # Determine current branch
        branch = subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=wiki_git_dir, text=True
        ).strip()
        subprocess.check_call(["git", "push", "origin", branch], cwd=wiki_git_dir)
        print("Wiki publication completed successfully.")
        return "pushed"
    print("Local sync completed. (Run with --push to publish to remote Wiki).")
    return "committed"


def main():
    parser = argparse.ArgumentParser(description="NERO GitHub Wiki Synchronizer")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="Validate canonical documentation and link mappings")
    group.add_argument("--build", type=Path, help="Build Wiki pages into specified directory")
    group.add_argument("--sync", type=Path, help="Sync generated Wiki pages into cloned Wiki Git repository")

    parser.add_argument("--push", action="store_true", help="Push changes when running --sync")
    parser.add_argument("--status-file", type=Path,
                        help="With --sync, write the fixed outcome word "
                             f"({', '.join(SYNC_STATUSES)}) to this file on success")
    args = parser.parse_args()
    if args.status_file and not args.sync:
        parser.error("--status-file requires --sync")

    if args.check:
        valid = check_wiki_docs()
        sys.exit(0 if valid else 1)
    elif args.build:
        build_wiki_pages(args.build)
    elif args.sync:
        status = sync_wiki_repo(args.sync, push=args.push)
        if args.status_file:
            args.status_file.write_text(status + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
