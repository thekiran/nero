#!/usr/bin/env python3
"""Trusted-main Wiki publication. Credentials exist only in child environment.

Git output is deliberately not forwarded: transports and helpers may echo an
encoded credential that GitHub's literal secret masking would not recognize.
The outcome comes from a fixed status word that wiki_sync.py writes to a file,
never from transport output, and only allowlisted words are ever reported.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ASKPASS = '''#!/usr/bin/env python3
import os, sys
if "Username" in sys.argv[1]:
    print("x-access-token")
elif "Password" in sys.argv[1]:
    print(os.environ["WIKI_TOKEN"])
else:
    sys.exit(1)
'''

# Fixed, non-secret result lines keyed by the wiki_sync.py status word.
OUTCOMES = {
    "pushed": "Wiki publication succeeded: documentation changes were pushed.",
    "unchanged": "Wiki publication succeeded: no changes; the Wiki was already current.",
}
UNREPORTED = "Wiki publication finished, but synchronization reported no outcome."


def publish(repo, token):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo) or not token:
        raise ValueError("Wiki publication blocked: missing credential or invalid repository")
    with tempfile.TemporaryDirectory(prefix="nero-wiki-") as directory:
        root = Path(directory)
        helper = root / "askpass"
        helper.write_text(ASKPASS, encoding="utf-8")
        helper.chmod(0o700)
        # Do not inherit Git trace/config/credential helpers or persist auth.
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("GIT_", "WIKI_"))}
        env.update(HOME=str(root), XDG_CONFIG_HOME=str(root),
                   GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                   GIT_TERMINAL_PROMPT="0", GIT_ASKPASS=str(helper),
                   WIKI_TOKEN=token)
        # Empty helper overrides inherited helper chains. No credential is in
        # an argument, URL, file or git config. The helper uses the environment.
        env.update(GIT_CONFIG_COUNT="2", GIT_CONFIG_KEY_0="credential.helper",
                   GIT_CONFIG_VALUE_0="", GIT_CONFIG_KEY_1="core.hooksPath",
                   GIT_CONFIG_VALUE_1=os.devnull)
        checkout = root / "wiki"
        status_file = root / "sync-status"
        commands = [
            ["git", "clone", "--", f"https://github.com/{repo}.wiki.git", str(checkout)],
            [sys.executable, str(Path(__file__).with_name("wiki_sync.py")),
             "--status-file", str(status_file), "--sync", str(checkout), "--push"],
        ]
        for index, command in enumerate(commands):
            # Never write diagnostic artifacts or transport output, even on a
            # failure. Fixed messages keep secrets out of exception tracebacks.
            try:
                result = subprocess.run(command, env=env, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL, timeout=180, check=False)
            except (OSError, subprocess.TimeoutExpired):
                raise RuntimeError("Wiki publication failed: transport unavailable or timed out") from None
            if result.returncode:
                phase = "clone" if index == 0 else "sync/push"
                raise RuntimeError(f"Wiki {phase} failed; check credential scope and Wiki initialization")
        try:
            status = status_file.read_text(encoding="utf-8").strip()
        except OSError:
            status = ""
    message = OUTCOMES.get(status, UNREPORTED)
    print(message)
    return message


def write_step_summary(line):
    """Append one fixed, non-secret line to the GitHub Actions job summary."""
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    try:
        with open(path, "a", encoding="utf-8") as summary:
            summary.write(line + "\n")
    except OSError:
        print("::warning::Could not write the job summary.", file=sys.stderr)


def main():
    token = os.environ.pop("WIKI_TOKEN", "")
    try:
        message = publish(os.environ.get("GITHUB_REPOSITORY", ""), token)
    except (ValueError, RuntimeError) as error:
        print(f"::error::{error}", file=sys.stderr)
        write_step_summary(str(error))
        return 1
    write_step_summary(message)
    return 0


if __name__ == "__main__":
    sys.exit(main())
