"""Render native-installer release notes without discarding generated changes."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
DELIMITER = "<!-- generated-changelog -->"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--body", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"v[0-9]+[.][0-9]+[.][0-9]+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?", args.version):
        parser.error("invalid release version")
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        parser.error("expected a full source commit")
    template = (ROOT / ".github/RELEASE_NOTES.md").read_text()
    intro = (template.replace("{version}", args.version)
             .replace("{deb_version}", args.version[1:].replace("-", "~", 1))
             .replace("{commit}", args.commit))
    body = args.body.read_text() if args.body else ""
    if body.startswith("<!-- nero-native-installers -->") and DELIMITER in body:
        body = body.split(DELIMITER, 1)[1].lstrip()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(intro.rstrip() + "\n\n" + body.rstrip() + "\n")


if __name__ == "__main__":
    main()
