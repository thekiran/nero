# Source layout and build contracts

[Code map](code-map.md) · [Build guide](building.md) · [Documentation](README.md)

The directory migration groups project headers under `include/nero/` and sources
under `src/`. Includes use the `nero/` prefix with `include/` on the search path.
The CLI entry point is `src/main.c`; terminal and ASCII code live in `src/cli/`.
`src/cli/printing.c` is the retained legacy macro duplicate, not a build input.
Public signatures and runtime behavior are preserved.

## CLI responsibilities

- `terminal.c`: session setup, signals, cleanup and orchestration.
- `input.c`: byte input, escape sequences, line editing and session history.
- `history.c`: recognized-command persistence, private file validation, bounded import and serialized updates.
- `render.c`: terminal capabilities, prompts, redraws and help presentation.
- `commands.c`: tokenization, validation and existing command dispatch.
- `ascii.c`: logo and existing animation frames.
- `cli_internal.h`: private shared types, state declarations and function contracts.

The public API remains in `include/nero/terminal.h`. The private contract is not
an application-wide header. Make and CMake share their CLI source lists between
production and test targets; the fault fixture replaces only `terminal.c`.
Make object and dependency files live in `.build/obj/`, outside the source tree.

The TCP scanner is split into `src/modules/scan_session.c` (terminal-independent
production core) and `src/modules/portscanner.c` (interactive adapter). The public
session API is `include/nero/scan_session.h`; private I/O and adapter contracts
are in `src/modules/scan_internal.h` and `scan_cli_internal.h`. Sessions own their
ledger, profile, cancellation and work state. Synthetic tests inject per-session
I/O, while runtime tests use guarded loopback connections. The existing
`portscanner(target)` entry point and menu remain available. See the
[quality acceptance plan](engineering/quality-plan.md).

`src/modules/` holds the packet engine and scanner. Their header is retained as
`include/nero/syn_flood_powered.h`. Both Linux and macOS Make builds link them;
macOS raw sockets require root. The offline-safe executables use
`NERO_NETWORK_UNAVAILABLE` in `commands.c` and never link a network engine.
Mock executables retain their mock handler calls.
Windows has a native MinGW x86-64 CLI. `src/platform/windows/` provides a
Win32 console/Winsock adapter and interface enumeration. The Windows CLI excludes
the raw packet engine explicitly; its support scope differs from macOS/Linux.
`include/nero/socket_handle.h` keeps SOCKET values pointer-sized on Windows and
integer-sized on POSIX. CRT stdin polling and socket polling use separate paths.
Console snapshots preserve the full Win32 mode for exact restoration; process
cleanup restores the output mode and code page changed during startup.

Bundled cJSON is unchanged under `third_party/cjson/` and built by Make.
CMake builds the offline CLI on POSIX, the native CLI with MinGW on Windows,
and the platform foundation on all supported platforms. Shared compiler settings live in `cmake/NeroTargets.cmake`.
The foundation, compatibility and static-analysis workflows use Make/CMake
presets. The release workflow additionally verifies all five native packages and
their installers; its aggregate Release checks job gates pull requests.

## Integrity records

`tests/baseline.json` retains the historical snapshot and declarations.
`tests/layout-baseline.json` maps those entries to their current locations,
retaining historical hashes, hashes immediately before this layout migration
(including existing user edits), and hashes after the migration. An optional informational source audit records drift; it is separate from the
behavioral foundation test.
This replaces a reference to the absent `terminal-quality-baseline.json` file.
`tests/readability-baseline.json` records the subsequent CLI extraction and build
changes separately; the layout and historical snapshots are not rewritten.
`tests/english-baseline.json` records the subsequent translation of messages and
comments, preserving ANSI/control escapes and format placeholders.
Integrity checks do not replace compilation, terminal regression or platform tests.

## Validation

Run `make`, `make test`, `make test-sanitize` and `make analyze` on macOS.
CMake: configure, build and test with the `macos` or `macos-sanitize` presets.
Linux and Windows use their respective CI presets; a macOS pass does not certify
those systems. Windows MinGW has native CLI, adapter, synthetic boundary,
real console-restoration and isolated loopback tests. MSVC remains foundation-only.
