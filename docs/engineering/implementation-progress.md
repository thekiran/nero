# NERO implementation progress

Living status for the three-frontend platform (`nero` / `neroconsole` / `nero-gui`).
Update after every verified change. Trust Git + build state over any prior report.

## Current milestone

**Milestone 2 (Console) — IN PROGRESS.** `neroconsole` entry point delivered and
verified; install-script parity and a dedicated Windows CI run remain.

## Status by frontend

| Frontend | State | Evidence |
| --- | --- | --- |
| `nero` (CLI/TUI) | Working (pre-existing) | existing `make` build + full `make test` |
| `neroconsole` (TUI) | **Delivered, verified (macOS arm64)** | `tests/test_console_parity.py` under `make test` (TEST=0): identical `--doctor`/`version`/`help` vs `nero` |
| `nero-gui` (Qt6) | **BLOCKED — no Qt toolchain** | `qmake`/`qmake6` absent; no Homebrew `qt`; cannot build/verify here |

## Shared core / operation layer

- Unified operation registry (`include/nero/operation.h` dispatcher): **PLANNED**.
  `neroconsole` reaches parity by reusing the existing command dispatcher directly,
  so the registry is a refactor for maintainability, not a blocker for console parity.
- `operation_ui.h` / `operation_ui.c` exist on this branch (layout contract only).

## Files created / changed this session

- `src/apps/neroconsole_main.c` — new standalone console entry point (shared core).
- `Makefile` — `neroconsole` build target, `all`/`clean`/`test-console-parity`.
- `CMakeLists.txt` — `neroconsole` target in the Windows MinGW leg.
- `tests/test_console_parity.py` — behavioural parity contract (doctor/version/help).
- `docs/engineering/feature-parity.md` — dated progress note.

## Validation executed

- `make` (macOS arm64, Apple clang): builds `nero` + `neroconsole` cleanly (`-Werror`).
- `make test`: TEST=0, including `console parity: neroconsole matches nero`.
- `neroconsole --doctor` == `nero --doctor` (byte-identical, 816 bytes).

## Blocked / not verified

- `nero-gui`: no Qt — not built, not run. Needs Qt 6 install (e.g. `brew install qt`)
  or a CI job with Qt, then a C++ wrapper over the C core (no shell/ANSI bridging).
- `neroconsole` on Windows/Linux: not built locally (no MinGW/Linux here); relies on CI.
- `neroconsole` install-script parity (`scripts/install.sh` installs `nero` only).

## Next concrete step

Either (a) `brew install qt` and scaffold `apps/desktop/` with a minimal Qt6 window
that reads real `version`/`doctor` data through the C core (Milestone 3), or
(b) extend `scripts/install.sh` + packaging to install `neroconsole` alongside `nero`
and add a Linux/Windows console-parity CI leg (finish Milestone 2). Decision pending
with the user (Qt install is a large download).
