# Nero Fix Report — 2026-11-03

All changes were made on `main` (HEAD `97438f0` + working-tree edits), verified
on macOS 27.2 (Apple clang) and, via Docker (`tests/linux/Dockerfile`,
Ubuntu 24.04 aarch64, GCC 13.3 / Clang 18.1), on Linux. No flood capability was
added or enhanced; robustness fixes only.

## P0 — Command error handling (`src/cli/commands.c`)

- `SYN_Flood()` return value is now captured. On non-zero status the CLI prints
  `-Syn operation failed (status N).` to stderr and **keeps the session alive**
  (dispatch still returns 0). Unsupported platform, invalid parameters, and
  operation failure are now three distinct outcomes: the first is reported by
  the adapter's own message plus the failure line, the second by
  `Invalid command parameters.`, the third by the status line.
- `history -clear` now actually erases the in-memory history
  (`memset` + `history_count = 0`) and prints `History cleared.` — the command
  is no longer a silent no-op.
- Rationale: callers of a library function must see its result; a CLI session
  should not pretend a failed engine call succeeded; a command named `-clear`
  must clear.

## P0 — CI/foundation integrity (`tests/local-fixes-baseline.json`, `tests/readability-baseline.json`)

- The foundation test failed on clean HEAD because commit `97438f0` changed
  `src/platform/macos/network_unavailable.c` without recording the new digest.
  The fix chain was extended (`before` values untouched — history preserved):
  new `after` digests for `Makefile`, `src/modules/portscanner.c`,
  `src/modules/syn_flood_powered.c`; a new entry for
  `src/platform/macos/network_unavailable.c`; the `added` digest for
  `src/cli/commands.c`. Every entry carries a non-empty `reason`.

## P0 — closed-stdin CI timeout (`tests/test_terminal_quality.py`)

- Application code was verified correct for EBADF/EOF/EINTR (see
  [code-audit.md](code-audit.md) §3); no application change was made for this
  item because none reproduced locally.
- `test_closed_stdin_error` now fails with the captured partial stdout/stderr
  when the 10 s timeout fires, so the next CI run discriminates between the
  candidate causes instead of printing a bare `TimeoutExpired`.
- New `test_devnull_stdin_is_eof_not_error`: `/dev/null` stdin must exit 0 with
  the prompt rendered (EOF is not an error; closed fd is).

## P1 — port scanner module (`src/modules/portscanner.c`)

- `select_port_menu`: on EOF or read error the menu loop now breaks, restores
  the previous terminal mode on every exit path (including the early ones), and
  only enters raw mode when stdin is a TTY. Previously EOF spun the redraw loop
  forever; previously a non-TTY stdin ran `tcsetattr` on garbage state.
- `portscanner()`: only successfully created threads are joined
  (`threads_created` counter). Previously a partial `pthread_create` failure led
  to `pthread_join` on never-created thread IDs (undefined behavior).
- `realloc` failure in a worker no longer calls `exit(1)`; it records
  `scan_alloc_failed`, the remaining workers finish their current socket, and
  `portscanner()` frees the partial list and returns 1. Reading the flag after
  `pthread_join` is race-free (join synchronizes-with the worker).

## P1 — Linux engine module (`src/modules/syn_flood_powered.c`)

Robustness only; no behavioral or performance enhancement of traffic
generation.

- `sigint_handler` rewritten to use only `write(2)` and manual integer
  formatting (async-signal-safe); `printf` family is not legal in signal
  handlers. `row` is now `volatile sig_atomic_t`.
- Only created flood threads are joined (same partial-create defect as the
  scanner).
- `malloc` results for both pseudo-header buffers are NULL-checked; on failure
  the worker closes its sockets and exits.
- `printf("[-]Thread")` gained its missing newline.

### Self-caught defect during this fix

The first version of the UDP allocation-failure path freed the TCP
pseudo-header buffer a second time (it is freed at the end of the TCP block in
the same loop iteration). The Linux `-Werror` build (`-Wuse-after-free`)
rejected it immediately; the fix removes the double free. This is exactly why
the Linux build verification matters.

## P1 — Linux validation harness (`tests/linux/run.sh`, `tests/linux/*.py`)

- `run()` accumulates failures; the script ends with `[ "$FAILURES" -eq 0 ]`
  so CI sees a real exit status. `mktemp -d` + `trap` replaces the fixed
  `/tmp/nero` copy destination; all derived paths are parameterized.
- `cli_signals.py` and `interface_regression.py` take the binary path as
  `argv[1]` (previous hardcoded `/tmp/...` defaults retained as fallback).
- The interface/CLI pipe checks now contribute to the failure count instead of
  being informational only.

## P2 — build & docs

- `Makefile` `run` recipe: removed the stray `@` inside the continued shell
  command (`make run DOMAIN=example.com` previously executed a literal
  `@echo ...` line → `command not found`).
- `README.md`: license link now points to `LICENSE.txt`; removed the stale
  `Undefined symbols "_SYN_Flood"` troubleshooting block (issue already fixed
  on HEAD).
- `docs/linux-runtime-validation.md`: updated the run-script description to
  match the new failure-propagating, `mktemp`-based behavior.

## Round 5 — remaining `main` defects (2026-11-03, second pass)

1. **Port menu EOF/read errors (F14).** `select_port_menu` treated any failed
   `read` as "keep looping", so stdin EOF on a pipe made the menu spin and a
   partial arrow-key escape could be misread as input. The loop now tracks an
   `input_ok` flag: EOF or read error breaks the loop and the function returns
   **-1** (documented in `include/nero/portscanner.h`: `>0` chosen port,
   `0` none found, `-1` aborted/error). `portscanner()` propagates -1 as a
   clean failure; `port_get` in `syn_flood_powered.c` maps it to
   "Port selection aborted" and aborts the engine call instead of flooding an
   arbitrary default.
2. **Raw-mode lifecycle (F15).** `enable_raw_mode` previously ran even when
   stdin was not a TTY and restored state that was never captured. It now
   requires `isatty`, snapshots `orig_termios` with `tcgetattr`, sets the mode
   with `tcsetattr(TCSADRAIN)`, registers `atexit(disable_raw_mode)` exactly
   once, and `disable_raw_mode` is a no-op unless raw mode is active — every
   exit path (EOF, error, valid selection) leaves the terminal restored.
3. **Partial thread creation (F16).** Both `portscanner()` (200 workers) and
   `SYN_Flood` (512 workers) already joined only created threads; the missing
   piece was reporting. `portscanner()` now returns 1 with
   `Scan incomplete: only N of M workers started.` when
   `threads_created != NUM_THREADS`, after freeing the partial result list.
   `SYN_Flood` likewise returns 1 when its worker creation falls short.
4. **Passive command messages (F17).** `-Sc` and `-mode` (no-op legacy
   commands) now print a single explicit line each
   (`-Sc: not available in this build; no network traffic is generated.`) so
   users see the command was received and intentionally inactive, instead of
   silence that reads as "nothing happened".
5. **`Makefile DOMAIN` (F18).** Verified the app never consumed a positional
   domain argument; the `run` target's `DOMAIN` plumbing was dead weight with
   a history of shell breakage. Removed `DOMAIN`/`run_default`; `make run`
   simply launches the interactive CLI. Help text was checked — no command
   documents a positional domain.
6. **Regression tests (F19).** New coverage:
   - `tests/linux/scanner_runtime.c`: pipe-at-EOF → menu returns -1; pty
     Enter/arrow → expected ports **and** slave termios byte-identical after
     (structs zero-initialized so `memcmp` never compares indeterminate
     padding); injected `pthread_create` failure at worker 3 →
     `portscanner` returns 1, list freed, FD baseline restored. Runs under
     both plain and ASan/UBSan builds in the Docker harness.
   - `tests/test_terminal_quality.py`: `test_inactive_legacy_commands` now
     asserts exactly one `-Sc` line and two `-mode` lines (no duplicates, no
     `Unknown command`).
   - `tests/test_safe.py`: `test_disabled_network_command` asserts the
     `-Syn operation failed (status 1)` stderr line.
7. **CI macos-15 sanitizer failure.** Local experiment (`/tmp/fdprobe/probe.c`,
   plain + ASan, `preexec_fn=lambda: os.close(0)`) shows fd 0 stays closed at
   `main` entry on macOS 27.2 (`F_GETFD` → EBADF; first `open` receives fd 0),
   so the fd-reuse hypothesis is **refuted locally**. Job logs still return 403
   (admin-only) — root cause remains BLOCKED; the improved timeout diagnostics
   from the previous round are the discriminator for the next CI run.

## Backward compatibility

- CLI commands, help text, prompt rendering, NO_COLOR/UTF-8 handling, signal
  exit codes (128+sig), and exit statuses are unchanged, except: `history
  -clear` now really clears (intended behavior change), and a failed `-Syn`
  operation now prints a stderr line while the session continues.
- The only test expectation updated for the behavior change is
  `test_history_draft_and_legacy_clear` → `test_history_draft_and_clear`;
  new assertions lock both the clearing behavior and the preserved
  navigation draft behavior.
- Baseline manifests keep all historical `before` digests; only chain heads
  advance.
