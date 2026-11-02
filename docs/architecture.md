# Source layout and build contracts

[Code map](code-map.md) · [Build guide](building.md) · [Documentation](README.md)

The directory migration groups project headers under `include/nero/` and sources
under `src/`. Includes use the `nero/` prefix with `include/` on the search path.
The CLI entry point is `src/main.c`; terminal and ASCII code live in `src/cli/`.
`src/cli/printing.c` is the retained legacy macro duplicate, not a build input.
Public signatures and runtime behavior are preserved.

## CLI responsibilities

- `terminal.c`: session setup, signals, cleanup and orchestration.
- `input.c`: byte input, escape sequences, line editing and history writes.
- `render.c`: terminal capabilities, prompts, redraws and help presentation.
- `commands.c`: tokenization, validation and existing command dispatch.
- `ascii.c`: logo and existing animation frames.
- `cli_internal.h`: private shared types, state declarations and function contracts.

The public API remains in `include/nero/terminal.h`. The private contract is not
an application-wide header. Make and CMake share their CLI source lists between
production and test targets; the fault fixture replaces only `terminal.c`.
Make object and dependency files live in `.build/obj/`, outside the source tree.

`src/modules/` also retains the Linux engine and scanner omitted from the example
tree. Their header is retained as `include/nero/syn_flood_powered.h`.
The Linux Make build keeps its previous source selection and link dependencies.
The macOS Make build uses the unavailable-network adapter. All test executables
use that adapter or mocks, never the Linux engine or scanner.

The requested `src/platform/macos/network_unavailable.c` location contains a
portable stub also used by Linux and Windows offline tests. Its location does
not imply a dependency on macOS APIs. Windows currently supports only the
platform foundation; `src/platform/windows/` reserves space for future adapters.

Bundled cJSON is unchanged under `third_party/cjson/` and built by Make.
CMake builds the offline CLI on POSIX and the platform foundation on all supported
platforms. Shared compiler settings live in `cmake/NeroTargets.cmake`.
The three existing CI workflows continue to use Make and CMake presets.

## Integrity records

`tests/baseline.json` retains the historical snapshot and declarations.
`tests/layout-baseline.json` maps those entries to their current locations,
retaining historical hashes, hashes immediately before this layout migration
(including existing user edits), and hashes after the migration. The foundation
test verifies the mapping, current contents and absence of old source locations.
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
those systems. The Windows CLI and interface enumeration remain unsupported.
