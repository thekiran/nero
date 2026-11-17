# NERO implementation progress

Living status for the three-frontend platform (`nero` / `neroconsole` / `nero-gui`).
Update after every verified change. Trust Git + build state over any prior report.

## Current milestone

**Milestone 3 (Desktop Foundation) — first slice delivered.** `nero-gui` builds
and runs on macOS against the shared C core, showing real version + `doctor`
data. Remaining Milestone 3 work: wire more pages to real operations, add a Qt
CI job, and scaffard the scanner/wireless views. Milestone 2 (`neroconsole`) also
has open install-script/CI parity items.

## Status by frontend

| Frontend | State | Evidence |
| --- | --- | --- |
| `nero` (CLI/TUI) | Working (pre-existing) | existing `make` build + full `make test` |
| `neroconsole` (TUI) | **Delivered, verified (macOS arm64)** | `tests/test_console_parity.py` under `make test` (TEST=0): identical `--doctor`/`version`/`help` vs `nero` |
| `nero-gui` (Qt6) | **Delivered, verified (macOS arm64, Qt 6.12)** | `cmake -DNERO_BUILD_GUI=ON` builds `nero-gui.app` clean; runs offscreen with no QML errors; `--grab` screenshot shows real `nero_doctor()` output + version from the shared core |

## Shared core / operation layer

- Unified operation registry (`include/nero/operation.h` dispatcher): **PLANNED**.
  `neroconsole` reaches parity by reusing the existing command dispatcher directly,
  so the registry is a refactor for maintainability, not a blocker for console parity.
- `operation_ui.h` / `operation_ui.c` exist on this branch (layout contract only).

## Files created / changed this session

- `src/apps/neroconsole_main.c` — standalone console entry point (shared core).
- `Makefile` — `neroconsole` build target, `all`/`clean`/`test-console-parity`.
- `CMakeLists.txt` — `neroconsole` (Windows leg) + `NERO_BUILD_GUI` option guarding `apps/desktop`.
- `tests/test_console_parity.py` — behavioural parity contract (doctor/version/help).
- `apps/desktop/` — Qt 6 `nero-gui`: `main.cpp` (+ `--grab`), `backend/NeroController.{h,cpp}`
  (calls `nero_doctor()` via `open_memstream`, no shell), `qml/Main.qml`, `CMakeLists.txt`.
- `docs/engineering/{feature-parity,implementation-progress}.md` — status.

## Validation executed (macOS arm64)

- `make`: builds `nero` + `neroconsole` cleanly (`-Werror`); `make test` TEST=0 incl. console parity.
- `neroconsole --doctor` == `nero --doctor` (byte-identical, 816 bytes).
- `cmake -DNERO_BUILD_GUI=ON -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt`: configures (Qt 6.12), builds `nero-gui.app` with no warnings/errors.
- `nero-gui` offscreen run: loads QML, no runtime/binding errors; `--grab` PNG shows real doctor/version data.
- Non-Qt build preserved: plain `cmake` (no `NERO_BUILD_GUI`) does not require Qt; `make` unaffected.

## Blocked / not verified

- `nero-gui` on Linux/Windows: not built here (no Qt on those legs locally); needs a Qt CI job.
- `nero-gui` only wires the Overview page (version + doctor); other pages are honest "not implemented" placeholders, no fake data.
- `neroconsole`/`nero-gui` install-script + packaging parity (`scripts/install.sh` installs `nero` only).
- Unified operation registry (`operation.h`) still PLANNED; frontends share the dispatcher directly.

## Next concrete step

Milestone 3 continuation: add a Qt CI job (so the GUI build is gated), then connect
the next real operation to the GUI — the Port Scanner page over the existing
`scan_session` C API (start/cancel/live results) with a Qt model, keeping parity
with the CLI/console scan path.
