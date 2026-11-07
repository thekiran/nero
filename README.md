<div align="center">

# NERO

**Native terminal engineering. Low-level systems. Controlled network research.**

A C11 project built to explore terminal interfaces, operating-system boundaries,
and network diagnostics—without hiding the implementation behind a framework.

<p>
  <a href="https://github.com/thekiran/nero"><img src="https://img.shields.io/badge/Language-C11-2563EB?style=flat-square&logo=c&logoColor=white" alt="C11"></a>
  <a href="#platform-support"><img src="https://img.shields.io/badge/Platforms-macOS%20%7C%20Linux-1F2937?style=flat-square&logo=apple&logoColor=white" alt="macOS and Linux"></a>
  <a href="#project-status"><img src="https://img.shields.io/badge/Status-Experimental-F59E0B?style=flat-square" alt="Experimental"></a>
  <a href="LICENSE.txt"><img src="https://img.shields.io/badge/License-MIT-1F2937?style=flat-square" alt="MIT license"></a>
</p>

[**Overview**](#overview) &nbsp;·&nbsp;
[**Quick start**](#quick-start) &nbsp;·&nbsp;
[**Commands**](#command-reference) &nbsp;·&nbsp;
[**Architecture**](#architecture) &nbsp;·&nbsp;
[**Development**](#development) &nbsp;·&nbsp;
[**Demo**](#demo)

<br>

<img src="https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084" alt="NERO interactive terminal interface" width="780">

<sub>Native terminal UI • ANSI rendering • command history • platform-aware builds</sub>

</div>

---

## Overview

**NERO** is an experimental, open-source systems programming project written in C.
It combines an interactive terminal with platform-specific code and a separate
offline test environment. The goal is to make the underlying systems behavior
visible, inspectable, and testable.

| Area | What it covers |
| :--- | :--- |
| **Terminal experience** | Interactive CLI, ASCII visuals, ANSI colors, keyboard input, and command history |
| **Systems programming** | C11, POSIX interfaces, terminal state, signal handling, and platform adapters |
| **Network diagnostics** | IPv4 command handling, TCP connectivity checks, and experimental network modules |
| **Cross-platform development** | macOS and Linux builds, plus a limited Windows CMake foundation |
| **Validation** | Offline regression tests, sanitizers, and Clang static analysis |

> [!IMPORTANT]
> NERO is a **research and learning project**, not a production-ready security product.
> Network features must only be used in isolated labs or against systems you are
> explicitly authorized to test.

## Quick start

### macOS

**Requirements:** Apple Command Line Tools, Make, and Python 3 for testing.

```bash
# Install the Apple build tools if needed
xcode-select --install

# Get the source
git clone https://github.com/thekiran/nero.git
cd nero

# Build and open the interactive terminal
make CC=clang -j4
./nero
```

### Linux (Debian / Ubuntu)

<details>
<summary><strong>Show dependencies and build instructions</strong></summary>

<br>

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  libcurl4-openssl-dev \
  libcjson-dev \
  libldns-dev \
  libncurses-dev \
  python3 \
  clang

git clone https://github.com/thekiran/nero.git
cd nero
make
./nero
```

</details>

Once NERO launches, type `help` to explore its available commands.
You do **not** need administrator privileges to use basic terminal commands.
Some experimental low-level network operations have additional platform and
permission requirements.

See the [full build guide](docs/building.md) for CMake presets, sanitizer builds,
and development prerequisites.

## Command reference

| Command | Purpose |
| :--- | :--- |
| `help` / `help -All` | Show the CLI command reference |
| `history` / `history -clear` | Display or clear session command history |
| `clear` | Redraw the terminal interface |
| `version` | Show version information |
| `-mode` / `-mode on` / `-mode off` | Inspect or change the diagnostics setting |
| `<IPv4> -Sc` | Run the TCP port-check workflow when supported by the build |
| `<IPv4> -Syn` | Legacy experimental network operation; authorized labs only |
| `exit` | Leave NERO and restore terminal state |

<details>
<summary><strong>Example: basic terminal session</strong></summary>

<br>

```text
► help
► version
► history
► -mode
► exit
```

</details>

Command availability and behavior depend on the selected build configuration.
**Offline test targets never initiate live network operations.**

## Architecture

NERO keeps terminal concerns separate from platform and networking code, so
changes can be reviewed and validated in smaller pieces.

```text
nero/
├── src/
│   ├── cli/             # Input, rendering, commands, ASCII and parsing
│   ├── core/            # Platform information
│   ├── modules/         # Network and diagnostic modules
│   └── platform/        # macOS / Linux implementations
├── include/nero/        # Shared C headers
├── tests/               # Offline tests, fixtures and mocks
├── cmake/               # CMake target configuration
├── docs/                # Architecture and build documentation
├── Makefile
└── CMakeLists.txt
```

**Start reading the code here:**

`src/main.c` → `src/cli/terminal.c` → `src/cli/input.c` →
`src/cli/commands.c` → `src/cli/render.c`

For a file-by-file guide, see the [code map](docs/code-map.md).

## Development

### Build, test, analyze

```bash
make                 # Build the native executable
make test            # Run network-free regression tests
make test-sanitize   # Run offline tests with ASan / UBSan
make analyze         # Run Clang static analysis
make clean           # Remove generated build artifacts
```

### CMake (offline development target)

```bash
cmake --preset macos
cmake --build --preset macos
ctest --preset macos
```

On Linux, use the `linux` preset instead. The Windows preset currently builds
only the platform foundation—not the interactive CLI.

## Platform support

| Capability | macOS | Linux | Windows |
| :--- | :---: | :---: | :---: |
| Native interactive CLI | Yes | Yes | No |
| Terminal rendering and history | Yes | Yes | No |
| Offline development tests | Yes | Yes | Foundation only |
| Make-based network modules | Included* | Included* | No |
| CMake platform foundation | Yes | Yes | Yes |

<sub>* Availability at runtime depends on the build, dependencies, operating-system
permissions, and the experimental implementation. Inclusion in a build does
not guarantee successful or safe operation on a given system.</sub>

## Demo

<div align="center">

<a href="https://www.youtube.com/watch?v=2HGtMkmqwpU">
  <img src="https://img.youtube.com/vi/2HGtMkmqwpU/hqdefault.jpg" width="520" alt="Watch a demonstration of the NERO terminal">
</a>

<br>

[**Watch the terminal demo on YouTube**](https://www.youtube.com/watch?v=2HGtMkmqwpU)

</div>

## Project status

NERO is **experimental and under active development**. The CLI, platform
adapters, testing workflows, and legacy network modules have different maturity
levels. Features and interfaces may change; no production reliability or
security guarantees are made.

To contribute, open a focused pull request with a description of the change,
the target OS/compiler, and relevant test results.

[**Report an issue**](https://github.com/thekiran/nero/issues) ·
[**Open a pull request**](https://github.com/thekiran/nero/pulls) ·
[**Browse documentation**](docs/building.md)

## License

Distributed under the [MIT License](LICENSE.txt).

Copyright © 2025–2026 **Muhammed Emin Kıran**.

---

<div align="center">

<sub><strong>NERO</strong> — native C, terminal engineering, and responsible network research.</sub>

</div>
