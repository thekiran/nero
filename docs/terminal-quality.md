# Terminal quality audit and refactoring

Baseline: `6cfce78`, with the previous uncommitted cross-platform foundation
preserved. No new branch was created because the working tree was dirty.
Initial `make test`, `make test-sanitize` (9 tests each) and `make analyze` passed.

| File / function | Cause | Severity | Planned correction |
| --- | --- | --- | --- |
| src/cli/terminal.c / terminal_code input | Cursor index doubles as length; Enter/backspace truncate suffix; insert overwrites bytes | High | Separate bounded length/cursor and use memmove for edits |
| src/cli/terminal.c / input append | NUL depends on initial zero-fill; no explicit invariant after editing | High | Maintain terminator after every edit |
| src/cli/terminal.c / command dispatch | strstr accepts unintended commands and suffix parameters | High | Exact tokens, bounded arguments, IPv4 validation for -Syn |
| src/cli/terminal.c / escape handling | Blocking reads and fallthrough insert sequence bytes | Medium | Bounded, timed CSI/SS3 parsing; ignore unsupported sequences |
| src/cli/terminal.c / history | Repeated shifts; down cannot restore draft | Medium | Preserve chronological storage/API; one bounded memmove at capacity, draft restoration |
| src/cli/terminal.c / termios | Failed setup continues, read errors look like EOF | High | Fail setup, report errors, centralized cleanup |
| src/cli/terminal.c / signals | Default termination leaves raw settings | High | Signal handlers set sig_atomic_t only; cleanup in normal flow |
| src/cli/terminal.c / exit | Dead return after break | Low | One cleanup path |
| src/cli/ascii.c / ascii_function | ANSI clear plus system(clear), repeated clearing/forking | Medium | One capability-aware clear; retain logo/frame literals |
| src/cli/terminal.c / rendering | No width-aware editing or plain terminal path | Medium | Single-line viewport, ASCII/no-color fallback |
| engine signal handler / animation | Unsafe stdio in existing engine handler; independent screen output | High | Do not change engine; serialize shell-owned rendering and never hold its mutex during dispatch |

## Preserved baseline behavior

Commands: help, help -All, history, history -clear (prints done; does not erase),
clear, exit, exit -All, version, <IPv4> -Syn. -Sc and -mode remain inactive stubs.
ANSI colors, Unicode logo, animation frame literals, in-memory 256-entry history,
255-byte command limit and public terminal/ascii entry points are retained.
Intentional corrections: exact matching replaces accidental substring matches;
invalid parameters/targets are rejected; insertion/deletion preserve suffixes;
history down restores the draft; standalone Escape no longer waits indefinitely.

No live engine/scanner calls are permitted in tests. A link-time mock records only
handler dispatch, including valid documentation-only IPv4 literals.
