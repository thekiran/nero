#!/usr/bin/env python3
"""Contract test: `neroconsole` is a real second frontend over the shared core.

It verifies behavioural parity with `nero`, not just that a binary exists:
  * `--doctor` diagnostics are byte-for-byte identical;
  * the interactive `version` command reports the same version;
  * `help` exposes the same command vocabulary.

Run: python3 tests/test_console_parity.py <nero> <neroconsole>
Uses only the local binaries; performs no network I/O.
"""
import subprocess
import sys


def run_doctor(binary):
    out = subprocess.run(
        [binary, "--doctor"],
        capture_output=True, text=True, timeout=30,
        env={"NERO_LOG_FILE": ":memory:", "NO_COLOR": "1"},
    )
    return out.returncode, out.stdout


def run_session(binary, commands):
    out = subprocess.run(
        [binary],
        input="".join(c + "\n" for c in commands),
        capture_output=True, text=True, timeout=30,
        env={"NERO_LOG_FILE": ":memory:", "NO_COLOR": "1"},
    )
    return out.stdout


def strip_banner(text):
    # Drop the ASCII logo / blank lines so we compare command output, not art.
    keep = []
    for line in text.splitlines():
        s = line.strip()
        if not s or set(s) <= set("░▒▓█ "):
            continue
        keep.append(s)
    return keep


def main():
    if len(sys.argv) != 3:
        print("usage: test_console_parity.py <nero> <neroconsole>", file=sys.stderr)
        return 2
    nero, console = sys.argv[1], sys.argv[2]
    failures = 0

    rc_n, doc_n = run_doctor(nero)
    rc_c, doc_c = run_doctor(console)
    if rc_n != rc_c or doc_n != doc_c:
        print("FAIL: --doctor output differs between nero and neroconsole")
        failures += 1
    else:
        print("PASS: --doctor identical (%d bytes, rc=%d)" % (len(doc_c), rc_c))

    ver_n = strip_banner(run_session(nero, ["version", "exit"]))
    ver_c = strip_banner(run_session(console, ["version", "exit"]))
    vlines = [l for l in ver_c if l.lower().startswith("version")]
    if ver_n != ver_c or not vlines:
        print("FAIL: version output differs (nero=%r console=%r)" % (ver_n, ver_c))
        failures += 1
    else:
        print("PASS: version identical -> %r" % vlines[0])

    help_n = strip_banner(run_session(nero, ["help", "exit"]))
    help_c = strip_banner(run_session(console, ["help", "exit"]))
    if help_n != help_c:
        print("FAIL: help/command vocabulary differs between frontends")
        failures += 1
    else:
        print("PASS: help vocabulary identical (%d lines)" % len(help_c))

    if failures:
        print("%d console-parity failure(s)" % failures)
        return 1
    print("console parity: neroconsole matches nero through the shared core")
    return 0


if __name__ == "__main__":
    sys.exit(main())
