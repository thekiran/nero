# Development and validation

[Project](../README.md) · [Code map](code-map.md) · [Documentation](README.md)

Run all commands from the repository root.

## Make

| Command | Result |
| --- | --- |
| `make` | Build the platform-specific `nero` executable |
| `make test` | Run 9 core and 19 terminal regression tests |
| `make test-sanitize` | Run the same offline tests with ASan/UBSan |
| `make analyze` | Analyze the CLI and selected macOS adapter with Clang |
| `make help` | List development commands |
| `make clean` | Remove the generated executable and `.build/` directory |

Requirements: a C11 compiler, Make, and Python 3 for tests.
On macOS, the Xcode Command Line Tools / SDK are sufficient.
The existing Linux Make target also uses the curl, cJSON, ldns and ncurses
development libraries. Offline tests do not link the network modules.

## CMake

CMake 3.21+ is required. Select the `macos`, `linux` or `windows` preset for your host.

```sh
cmake --preset macos
cmake --build --preset macos
ctest --preset macos
```

POSIX sanitizer presets are named `macos-sanitize` and `linux-sanitize`.
Windows requires the Visual Studio 2022 C tools and builds only the platform
foundation. The Windows CLI is not implemented.

## Generated files

```text
.build/
├── obj/          Make object and dependency files
├── tests/        Offline test executables
├── sanitize/     Sanitizer test executables
├── analysis/     Clang analysis reports
└── cmake/        Platform preset build directories
```

The root `nero` executable and `.build/` are excluded from Git.
Do not add build outputs to source directories.

## Code style

`.editorconfig` defines indentation and line endings; `.clang-format` defines C style.
CLI code uses four spaces, a 100-column limit and opening function braces on a
separate line. `clang-format` is a development tool, not a build dependency.

```sh
clang-format --dry-run --Werror src/cli/terminal.c src/cli/input.c \
  src/cli/render.c src/cli/commands.c src/cli/ascii.c src/cli/cli_internal.h
```

Do not bulk-format third-party code or legacy modules.
Review functional changes separately from formatting changes.

## Integrity records

`baseline.json` preserves the historical snapshot; `layout-baseline.json` records
source relocation. `readability-baseline.json` records before/after hashes and new
files from the CLI extraction and build reorganization. `english-baseline.json`
records the subsequent text-only translation. Tests verify this chain and current
source contents. Do not automatically regenerate records to hide a failure:
review the change, run relevant behavior tests and document the reason.
