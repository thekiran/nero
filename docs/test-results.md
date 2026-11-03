# Nero Test Results — 2026-11-03

Every result below was produced by the command shown, run on 2026-11-03 against
the audited working tree (HEAD `97438f0` + fixes). Host: macOS 27.2 ARM64,
Apple clang (Xcode), CMake 3.x; Linux via Docker image built from
`tests/linux/Dockerfile` (Ubuntu 24.04 aarch64, GCC 13.3.0, Clang 18.1.3).

## PASS

| Check | Command | Result |
|---|---|---|
| Whitespace/conflict hygiene | `git diff --check` | PASS ("diff --check: PASS") |
| Production build (macOS) | `make clean && make CC=clang -j4` | PASS ("[INFO] Built nero") |
| Offline regression suite | `make test` | PASS — `Ran 11 tests … OK` (test_safe) + `Ran 20 tests … OK` (test_terminal_quality, incl. new `test_devnull_stdin_is_eof_not_error`) |
| Sanitizer suite | `make test-sanitize` | PASS — `Ran 11 tests … OK` + `Ran 20 tests … OK` (ASan+UBSan, `UBSAN_OPTIONS=halt_on_error=1`) |
| Static analysis | `make analyze` | PASS (exit 0, `-Xanalyzer -analyzer-werror`) |
| CMake macOS | `cmake --preset macos && cmake --build --preset macos && ctest --preset macos` | PASS — `100% tests passed out of 4` (network-unavailable, foundation, terminal-quality, legacy-safe-regression) |
| CMake macOS sanitize | `cmake --preset macos-sanitize && cmake --build --preset macos-sanitize && ctest --preset macos-sanitize` | PASS — `100% tests passed out of 4` |
| Linux full validation | `docker run --rm -v "$PWD:/source" nero-validation-local sh /source/tests/linux/run.sh` | PASS — script exit 0, `TOTAL FAILURES: 0`; GCC and Clang full builds link the real engine; scanner worker harness PASS (10 loopback connects/EOF, closed port, invalid IPv4, FD baseline restored) in both strace and ASan/UBSan runs; interface parser PASS (valid + six invalid/overflow/EOF cases); production CLI SIGINT/SIGTERM exit 130/143 |
| `make run DOMAIN=…` recipe | `printf 'exit\n' \| make run DOMAIN=example.com` | PASS — no more `@echo: command not found`; CLI runs and exits cleanly |
| Closed-stdin behavior (timing probe) | 5× `subprocess.run(nero-mock, preexec_fn=close(0))` under sanitizers | PASS — each run rc=1 in 0.01–0.02 s, stderr `terminal read: Bad file descriptor` |

## BLOCKED (environment / access limits, not claimed as pass)

| Check | Reason |
|---|---|
| Exact root cause of CI run 41606628780 (`safe-validation (macos-15, clang)` → "No-network memory safety tests") | Job log download returns `403 Must have admin rights to Repository` for this account; the failure does not reproduce on macOS 27.2 (see [code-audit.md](code-audit.md) §3). The test now captures partial output on timeout so the next CI run discriminates causes. |
| Windows preset / MSVC jobs | No Windows toolchain on this host; the empty `src/platform/windows/` + CMake `WIN32` branch already restrict Windows to foundation-only. Current Windows CI failures predate this audit. |
| Linux CMake presets (`linux`, `linux-sanitize`) locally | Presets gated to `hostSystemName == Linux`; equivalent coverage obtained via the Docker container's full `make` builds and scanner harness instead. |
| Active-operation signal tests, flood fault injection | Out of scope by policy: engine execution beyond the loopback scanner worker is prohibited; documented as NOT TESTED in [linux-runtime-validation.md](linux-runtime-validation.md). |

## FAIL

None at the time of writing. Pre-audit failures that this change set resolves:

1. `foundation` ctest on clean HEAD (baseline chain broken by `97438f0`).
2. Linux `run.sh` swallowed failures (verified: before the fix, a forced build
   error mid-script still exited 0; after the fix the same scenario exits 1).
3. `make run DOMAIN=x` shell error from the stray `@` continuation.

## Round 5 results (2026-11-03, second pass)

| Check | Command | Result |
|---|---|---|
| Whitespace/conflict hygiene | `git diff --check` | PASS |
| Production build (macOS) | `make clean && make CC=clang -j4` | PASS |
| Offline regression suite | `make test` | PASS — `Ran 11 tests … OK` + `Ran 20 tests … OK` (incl. passive-command assertions) |
| Sanitizer suite | `make test-sanitize` | PASS — 11 + 20 tests OK |
| Static analysis | `make analyze` | PASS (exit 0) |
| CMake macOS | `cmake --preset macos && cmake --build --preset macos && ctest --preset macos` | PASS — `100% tests passed out of 4` |
| Linux full validation | `docker run --rm -v "$PWD:/source" nero-validation-local sh /source/tests/linux/run.sh` | PASS — exit 0, `TOTAL FAILURES: 0`; new harness checks green: menu EOF on pipe → -1; pty Enter/arrow → expected ports with slave termios restored (plain + ASan/UBSan); injected `pthread_create` failure → `portscanner` returns 1, state cleaned (plain + ASan/UBSan); GCC + Clang engine builds; interface parser; CLI signal exits 130/143 |

### Round 5 per-finding status

| # | Finding | Status |
|---|---|---|
| 1 | macos-15 CI `make test-sanitize` root cause | **BLOCKED** — job logs 403 (admin-only); local runs all green; fd-0-reuse hypothesis experimentally refuted on macOS 27.2 (plain + ASan probe, fd 0 EBADF at `main`, first `open` gets fd 0); timeout diagnostics from round 4 remain the CI discriminator |
| 2 | Port menu EOF/read error → valid selection | **PASS** — fixed (`select_port_menu` returns -1); pipe-EOF and pty regression tests green on Linux harness |
| 3 | Raw mode tcgetattr/tcsetattr/cleanup/atexit | **PASS** — isatty-gated, single atexit registration, no-op-safe disable; termios byte-identical after menu on pty (padding-safe comparison) |
| 4 | Partial thread creation reported as full success | **PASS** — `portscanner` reports "only N of M workers started" and returns 1; injected-failure test green |
| 5 | `-Sc` / `-mode` passive command feedback | **PASS** — explicit single-line status messages; exact-count assertions in terminal suite |
| 6 | `Makefile DOMAIN` processing | **PASS** — verified app never consumed it; dead plumbing removed, `make run` launches interactive CLI |
| 7 | Regression test per fix | **PASS** — three new harness checks (EOF, termios restore, partial create) × (plain, ASan/UBSan) + updated Python assertions |

## Notes

- No test timeouts were raised; the only timing-related change is improved
  diagnostics when the existing 10 s timeout fires.
- Sanitizer runs use the repository's own flags; nothing was weakened.
- `git status` / `git diff --stat` were used to confirm no unrelated user files
  were touched; all edits are listed in [fix-report.md](fix-report.md).
