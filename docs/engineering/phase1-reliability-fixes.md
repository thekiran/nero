# Reliability audit round — findings and fixes

Base: `origin/main` at `dc505ac` (after the `scan_session` engine split, PR #42).
Host: Darwin arm64, Apple clang 21.0.0. Method: a multi-agent source audit of four
subsystems (scan correctness, concurrency/resources, terminal/log/history,
packaging/CI), each finding independently re-verified against the tree before
acceptance. Source inspection is not runtime proof; every applied fix is matched
to a build and the offline test suite, and the scanner-core change ships with a
regression test that fails before it and passes after.

Scope guard: reliability/correctness/resource/terminal/packaging only. The traffic
module (`src/modules/syn_flood_powered.*`) is out of scope and untouched. The
existing CLI surface, commands, colors and flows are preserved.

## Confirmed findings and disposition

| # | Sev | Area | Finding | Disposition |
| - | --- | ---- | ------- | ----------- |
| 1 | medium | scan correctness | `connect()` returning `EAGAIN`/`EWOULDBLOCK` (local ephemeral-port / buffer / routing-cache exhaustion) was treated as connect-in-progress, so a purely local failure could surface as `filtered` or even `open`, and the adaptive back-off never engaged. | FIXED + regression test |
| 2 | medium | packaging/CI | `CMakeLists.txt` version-tag regex was stricter than `release.yml`'s, so a tag the release gate accepts (e.g. `v1.0.0-beta-2`) could `FATAL_ERROR` only on the Windows leg, yielding an asymmetric release. | FIXED (regex aligned) |
| 3 | low | concurrency | If the monotonic clock fails at the final reading, `scan_duration_ms` could print as a large negative value. | FIXED (clamped) |
| 4 | low | terminal/log | `open_log` did not validate owner/link-count the way `open_history` does, so a pre-existing or override log path owned by another user was appended to instead of rejected. | FIXED (owner/nlink guard) |
| 5 | low | terminal/log | No inter-process lock around log append + rotation; two `nero` processes sharing one log file can race during rotation. | DOCUMENTED as a known limitation (see below) |
| 6 | low | packaging/CI | `actions/checkout` was pinned to the mutable tag `@v7.0.1` in `ci.yml`, `static-analysis.yml`, `compatibility.yml`, inconsistent with the SHA pins used elsewhere. | FIXED (SHA-pinned) |
| 7 | low | packaging | `uninstall.sh` ignored `INSTALL_DIR`, so a binary installed to a custom dir was never removed although the script reported success. | FIXED (honors `INSTALL_DIR`) |
| 8 | info | concurrency | `manual_profile` is a mutable `static` global; harmless for the single interactive CLI, but not reentrant across the concurrent sessions the engine supports. | DOCUMENTED as intentional (see below) |

Rejected during verification (kept for the record):
- "Windows re-poll passes `INVALID_SOCKET` through to `WSAPoll`" — `INVALID_SOCKET`
  is all-bits-set and `WSAPoll` ignores such slots; not a defect.
- "Negative `printf` precision on an ultra-narrow terminal" — unreachable; the
  compact-layout guard returns before the format string is reached.

## Fix detail for finding 1

- `connect_pending()` no longer treats `EAGAIN`/`EWOULDBLOCK` as pending on POSIX;
  `is_resource_error()` now covers them. An exhausted connect is recorded
  `PORT_UNKNOWN` (never closed/filtered/open) and sets the resource flag that
  drives `run_deep_scan`'s bounded back-off — matching the existing
  `EADDRNOTAVAIL` handling.
- Guarded for Windows, where `nero_win_compat.h` aliases
  `EINPROGRESS == EWOULDBLOCK == WSAEWOULDBLOCK` and `WSAEWOULDBLOCK` is the
  legitimate connect-pending signal.
- Regression: `tests/portscanner_reliability.c`, `CONNECT_LIMIT` scenario — fails
  on the pre-fix engine (records `CLOSED`), passes after (`UNKNOWN` + one resource
  failure). No network I/O; mock `scan_io` only.

## Testing actually run

- `make test` → exit 0 (unit + Python + PTY suites)
- `make test-sanitize` (ASan/UBSan) → exit 0, no sanitizer reports
- `make test-tsan` (TSan) → exit 0, 0 data races
- Before/after confirmed for finding 1 as above

## Known limitation (finding 5)

Concurrent `nero` processes writing the same log file can still race during log
rotation: rotation renames the file and recreates the inode, so an advisory lock
held on a single descriptor does not cover the recreated file. A correct fix needs
a dedicated side-car lock file; it is deferred rather than shipped as a partial
lock that would give a false sense of safety. The default per-user log directory
is created `0700`, which contains the common case. Single-line append integrity is
preserved by `O_APPEND` plus sub-buffer single writes.

## Intentional single-session scope (finding 8)

`manual_profile` is deliberately process-scoped: it remembers the user's last
manual thread/batch/timeout choice across scans within one interactive session.
The whole CLI front-end (`portscanner_with_io`) is single-session by design — the
interrupt flag, saved `termios`, and raw-mode flag are all process globals — so
`manual_profile` matches that contract rather than being an oversight. The
reusable, reentrant unit is the `scan_session` engine, which copies the chosen
profile into per-session state and already supports concurrent sessions
(`tests/scan_session.c`). Making only `manual_profile` local would drop the
remembered-settings UX without making the terminal front-end reentrant, so it is
documented rather than changed.
