# Nero Code Audit — 2026-11-03

> **Historical snapshot (HEAD `97438f0`).** This records the repository state on
> the audit date and is kept as-is. Several observations have since been
> superseded: `src/cli/printing.c` and `src/platform/macos/network_unavailable.c`
> were later removed, the macOS Make build now links the real raw packet engine
> (`-Syn`, root-only), and `src/platform/windows/` holds a Win32 adapter plus
> interface source that the MinGW CLI builds. For current state see
> [architecture.md](architecture.md), [code-map.md](code-map.md) and
> [platform-compatibility.md](platform-compatibility.md).

## 1. Scope and repository state

- Branch: `main`, HEAD `97438f0` ("fix(macos): remove unnecessary comment and
  simplify error message in SYN_Flood function"), up to date with `origin/main`.
- Working tree was clean at audit start; no user changes were discarded at any
  point (verified with `git stash` / `git stash pop` around the baseline
  comparison run).
- Reviewed: `src/cli/` (terminal, input, commands, render, ascii), `src/core/`,
  `src/modules/`, `src/platform/{linux,macos,windows}`, `include/nero/`,
  `tests/`, `Makefile`, `CMakeLists.txt`, `CMakePresets.json`,
  `.github/workflows/`, README and `docs/`.
- The Windows platform directory is empty; the CMake Windows path builds only
  the offline foundation target and `ci.yml` labels CLI/interface features
  Unsupported there. Windows jobs currently fail in CI for reasons that cannot
  be reproduced on this macOS host (no Windows toolchain) — see
  [test-results.md](test-results.md) BLOCKED entries.

## 2. Verified findings

| # | Severity | File:line | Finding |
|---|----------|-----------|---------|
| F1 | P0 | `src/cli/commands.c:40` (before fix) | `SYN_Flood()` return value discarded; engine failures were silently reported as success and the failure reason was invisible to the caller. |
| F2 | P1 | `src/cli/commands.c:53-55` (before fix) | `history -clear` printed `done` but never cleared anything — an ineffective command silently succeeding. |
| F3 | P1 | `src/modules/portscanner.c:72` (before fix) | Port selection menu looped forever on stdin EOF (`continue` on failed `read`), redrawing endlessly — the "terminal stays open unexpectedly" class of defect. |
| F4 | P1 | `src/modules/portscanner.c:231-233`, `src/modules/syn_flood_powered.c:325-327` (before fix) | `pthread_join` called for every slot of the fixed-size thread array even when `pthread_create` failed partway — joining an identifier that was never created (undefined behavior, potential hang). |
| F5 | P1 | `src/modules/portscanner.c:134-137,172-176` (before fix) | `exit(1)` called from inside a scanner thread on `realloc` failure — abrupt whole-process termination from library code, without cleanup. |
| F6 | P1 | `src/modules/syn_flood_powered.c:48-53` (before fix) | `sigint_handler` called `printf()` — not async-signal-safe; also read/wrote non-`volatile` shared state from the handler. |
| F7 | P1 | `src/modules/syn_flood_powered.c:172,228` (before fix) | `malloc` results used without NULL checks in the packet builders. |
| F8 | P1 | `tests/linux/run.sh` (before fix) | `run()` always returned 0 and the script's own exit status came from the last command only; mid-script failures (including failed builds) were invisible to CI. Hardcoded `/tmp/nero` broke on re-run (`mkdir` without `-p`). |
| F9 | P1 | `tests/linux/cli_signals.py`, `tests/linux/interface_regression.py` | Hardcoded `/tmp/nero-gcc` and `/tmp/interfaces` paths, coupled to F8's fixed copy destination. |
| F10 | P2 | `Makefile:83-86` (before fix) | Stray `@` prefix on a continuation line inside the `run` recipe was passed to the shell (`@echo: command not found`) when `DOMAIN` was set. |
| F11 | P2 | `README.md` | `[MIT License](../LICENSE.txt)` link was broken (file is `LICENSE.txt`); the macOS "Undefined symbols `_SYN_Flood`" troubleshooting block was stale — the described issue was already fixed on HEAD. |
| F12 | P0 (CI) | `tests/*-baseline.json` chains | The foundation integrity test failed on **clean HEAD** (before this audit's edits): `src/platform/macos/network_unavailable.c` digest from commit `97438f0` was never recorded in the baseline chain, keeping the whole foundation CI matrix red. |
| F13 | P1 | `src/modules/syn_flood_powered.c` | Self-introduced during this audit and caught by the Linux `-Werror` build: a double `free(pseudogram_tcp)` in the UDP allocation-failure path. Fixed before merge; see [fix-report.md](fix-report.md). |
| F14 | P1 | `src/modules/portscanner.c` (round 5) | Port-selection menu kept looping on EOF/read error; a pipe at EOF spun the redraw loop and the return value could not distinguish "user aborted" from "nothing found". Fixed: EOF/read error → return -1, documented in `include/nero/portscanner.h`; `port_get` aborts the engine call on -1. |
| F15 | P1 | `src/modules/portscanner.c` (round 5) | Raw mode was entered without an `isatty` check and restored unconditionally, so non-TTY stdin got `tcsetattr` on uninitialized state. Fixed: isatty gate, `tcgetattr` snapshot, single `atexit(disable_raw_mode)` registration, no-op-safe disable. |
| F16 | P1 | `src/modules/portscanner.c` (round 5) | Partial `pthread_create` failure (already joined correctly since round 4) was still reported as success to the caller. Fixed: `portscanner()` returns 1 with an explicit "only N of M workers started" message; `SYN_Flood` likewise returns 1. |
| F17 | P2 | `src/cli/commands.c` (round 5) | Passive legacy commands `-Sc`/`-mode` executed silently; silence is indistinguishable from "command not recognized". Fixed: one explicit status line per invocation. |
| F18 | P2 | `Makefile` (round 5) | `run` target carried a `DOMAIN` variable the application never consumed (verified against argument parsing and help text). Fixed: dead plumbing removed; `make run` launches the interactive CLI directly. |
| F19 | P1 | `tests/linux/scanner_runtime.c` (round 5) | Harness `memcmp` of `struct termios` compared indeterminate padding bytes (abort, exit 134). Fixed: zero-initialized structs; the comparison now reflects real field state. |

## 3. CI failure investigation (P0: macos-15 `make test-sanitize`)

- Failing run: https://github.com/thekiran/nero/actions/runs/41606628780
  (job `safe-validation (macos-15, clang)`, failed step "No-network memory
  safety tests", exit code 2). The same job also fails on HEAD `97438f0`.
- The job log is access-restricted (`403 Must have admin rights to Repository`
  from the public API; the HTML page renders no test output) — **root cause
  BLOCKED on log access**.
- Local reproduction (macOS 27.2, Apple clang, ASan+UBSan): the reported
  timeout test `test_closed_stdin_error` exits in ~0.01 s with returncode 1 and
  `terminal read: Bad file descriptor` on stderr; the full
  `make test-sanitize` suite passes (11 + 20 tests), including under CMake
  `macos-sanitize` ctest.
- Code-path audit of closed-stdin / EOF / EBADF / EINTR:
  - Closed stdin → `read()` returns EBADF immediately → `cli_read_line`
    returns -1 → `perror("terminal read")` → exit 1. No loop, no hang.
  - `/dev/null` stdin → EOF → clean exit 0 (new regression test added).
  - EINTR is retried unless a termination signal is pending; the terminal is
    restored on every exit path including `atexit` cleanup.
- Candidate causes for the runner-only hang that could not be excluded without
  logs: (a) fd 0 being reused by a runtime component before `main`, turning the
  blocking `read(0)` into a wait on an unrelated descriptor; (b) sanitiser
  runtime startup slowness on the image interacting with the 10 s budget;
  (c) the step actually failing at compile time under the image's newer clang
  with `-Werror` (exit code 2 is consistent with a build failure and would
  produce no unittest output). The test now emits partial output on timeout to
  discriminate these on the next CI run.
- **Round 5 update:** hypothesis (a) was experimentally refuted on macOS 27.2.
  A probe (`/tmp/fdprobe/probe.c`) compiled both plain and under ASan, run with
  fd 0 closed before `exec`, shows `F_GETFD` on fd 0 → EBADF at `main` entry
  and the first `open()` receiving fd 0 in both builds — ASan does not reuse
  fd 0 on this host. Root cause therefore remains **BLOCKED on log access**
  (403, admin-only); (b) and (c) above are the live candidates.
- The timeout value was **not** raised.

## 4. Unverified suspicions (not claimed as bugs)

- `is_target_online()` in `syn_flood_powered.c` builds a `system("ping ...")`
  command. Callers validate the address with `inet_pton` first, so no injection
  is reachable from the CLI; flagged for awareness only.
- `fd_set` use in `portscanner.c` assumes sockets stay below `FD_SETSIZE`
  (fine per-thread: one socket at a time).
- `src/cli/printing.c` is a stale, uncompiled duplicate of
  `include/nero/printing.h` (left in place; deletion is a maintainer decision).
- `include/nero/printing.h` defines the unused `TERMINAL_CURSOR_1 ---` macro
  twice; harmless because nothing references it.
- Flood-thread `sendto` is not interrupted by SIGINT on all kernels; threads
  exit when `stop_flag` is observed after the syscall returns. Pre-existing
  behavior, unchanged by this audit.

## 5. Architecture notes

- Platform isolation is intact: macOS builds `network_unavailable.c` (no
  socket symbols — enforced by `nm` inspection in tests) plus the read-only
  getifaddrs adapter; Linux keeps the legacy engine behind the Makefile and it
  is compiled but never executed in CI.
- No flood/scan capability was added, ported, or improved on macOS; the
  unavailable-adapter contract and its test gate are preserved.
- Layering today: `main.c` → `terminal_code()` (session lifecycle) →
  `cli_read_line` (input/history) → `cli_dispatch_command` (commands) with
  rendering isolated in `render.c`; `network_unavailable.c` is the macOS
  network boundary; `select_interface_macos.c` is the read-only enumeration
  boundary.
