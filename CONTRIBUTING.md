# Contributing to the laboratory project

Read [laboratory-safety.md](docs/laboratory-safety.md) first. Preserve the legacy
Linux engine, CLI commands and Make path. Use small, reviewable changes on a
separate branch; never rewrite user history or force-push.

Run `make`, `make test`, `make test-sanitize`, `make analyze` on a supported POSIX
development host. Run the appropriate CMake configure/build/test preset when
CMake is available. Record compiler, OS, architecture, failures and skipped tests.
Never infer runtime support from compilation alone. Never run network-load tests.

The baseline manifest protects existing C/header/Makefile contents. Do not regenerate
it simply to make a failure disappear: intentional changes need review, targeted
regression evidence and an explicit baseline update. Windows CI currently tests
foundation components only; implementing the real console requires separate tests.

Check `git diff --check`, review the full diff (including new files), and obtain
explicit approval before committing, pushing, merging or releasing work.

## Finding and placing code

Start with [the code map](docs/code-map.md). Keep CLI session handling, line input,
rendering and dispatch in their respective modules. Put public declarations in
`include/nero/`; keep CLI-only declarations in `src/cli/cli_internal.h`.
Document ownership whenever adding shared state. Keep generated files in `.build/`.

Follow `.editorconfig` and `.clang-format` for edited CLI code. Do not reformat
bundled dependencies or unrelated Linux modules. Update the code map when adding
or moving a responsibility. The [build guide](docs/building.md) lists validation
commands and explains the historical, layout and readability integrity records.
