# Local validation — 2026-11-03

Environment: macOS/Darwin 27.2.0 ARM64, Apple Clang 21.0.0
(clang-2100.4.1.1), GNU Make 3.81, CMake 4.4.5, Python 3.9.6.
Initial Git state: clean `main`. No AGENTS.md was found in the project tree.

## Permissions

- PASSED: shell execution, project reads/writes and build artifact creation.
- PASSED: create/read/delete a temporary file under `/private/tmp`.
- BLOCKED: sandbox network access, established earlier in this session by
  `Operation not permitted`; this is not a Nero implementation failure.
- PASSED previously: approved ping to `192.168.31.1`, 2/2 responses, no loss,
  average 5.101 ms. This repository validation did not repeat network probes.
- Sandbox remained enabled throughout this repository validation; no additional
  permissions, system installation or privilege changes were needed.

## Build and execution evidence

| Command/check | Result |
| --- | --- |
| `make -j4` | PASSED with `-Wall -Wextra -Werror -pedantic -std=c11`; final link has no alignment warning |
| `make test` | PASSED: 10 core/interface tests and 19 terminal tests |
| `make test-sanitize` | PASSED: same 29 tests under ASan and UBSan |
| `make analyze` | PASSED: no diagnostics |
| Clang analyzer on bundled `third_party/cjson/cJSON.c` | PASSED: no diagnostics; not a runtime memory-safety certification |
| `cmake --preset macos`, build, `ctest --preset macos` | PASSED: 4/4 CTest groups, rebuilt/retested after fixes |
| `cmake --preset macos-sanitize`, build, corresponding CTest preset | PASSED: 4/4 groups, rebuilt/retested after fixes |
| Make-built `./nero` with help/version/exit | PASSED: exit 0 and expected output |
| `git diff --check` | PASSED |

The tests exercise command parsing, bounded input/history, terminal restoration
on exit/signals, EOF/hangup, closed stdin, termios failures and EINTR recovery.
Interface fixtures verify `freeifaddrs` cleanup on successful enumeration,
including empty and truncated lists. The safe executable import check verifies
absence of the listed socket/connect/send/name-resolution entry points.

## Reproduced problems and fixes

1. FAILED before fix: interface inputs `1junk` and `4294967297` both selected
   fixture interface 1 and returned success. `%d` accepted a numeric prefix and
   the oversized value converted to 1 on this host. The macOS adapter now reads
   a bounded line and uses `strtol`, checks overflow/range and rejects trailing
   non-whitespace. Regression cases cover these inputs, long lines, blank input,
   valid whitespace and a final number without newline. PASSED after fix.
2. The linker warned about reducing common-section alignment from 0x8000 to
   0x4000. `nm` identified the 64 KiB history array as a common symbol. Explicit
   zero initialization removes the warning while preserving initial state.
   All history and terminal tests PASSED after the change.

`tests/local-fixes-baseline.json` records the intentional before/after hashes
and reasons. The foundation check applies this additional integrity layer;
the historical, layout, readability and English migration records are preserved.

## Remaining limitations

- NOT TESTED: Linux or Windows builds/runtime, Intel macOS and remote CI.
- NOT TESTED: live scanner/engine execution or live socket cleanup. The macOS
  executable uses an unavailable-network adapter and opens no engine sockets.
- Source review confirms existing Linux module concerns: unchecked allocations,
  signal-handler stdio, thread joins after creation failures, and scanner
  FD_SETSIZE handling. These excluded modules were not changed or executed.
- NOT TESTED: exhaustive allocation-failure injection, heap leak detection,
  descriptor-leak accounting and bundled cJSON sanitizer runtime tests. ASan/
  UBSan passes apply to the exercised offline targets, not all repository code.
- The Linux interface parser retains its previous implementation; the bounded
  selection fix and its fixture tests are macOS-specific.
- No stress tests, flooding, active scans or additional external traffic occurred.

## Files and Git

Modified: `src/cli/input.c`, `src/platform/macos/select_interface_macos.c`,
`tests/test_safe.py`, `tests/integration/test_foundation.py`, `docs/building.md`.
Added: `tests/local-fixes-baseline.json`, `docs/local-validation.md`.
Changes remain uncommitted on `main`; nothing was pushed. Build artifacts are
ignored by Git. No pre-existing uncommitted changes were present.

Suggested commit summary: `Fix macOS interface selection validation and history alignment`.
