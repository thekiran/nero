<div align="center">

# NERO

**Native terminal engineering. Low-level systems. Controlled network research.**

A C11 project built to explore terminal interfaces, operating-system boundaries,
and network diagnostics—without hiding the implementation behind a framework.

<p>
  <a href="https://github.com/thekiran/nero"><img src="https://img.shields.io/badge/Language-C11-2563EB?style=flat-square&logo=c&logoColor=white" alt="C11"></a>
  <a href="#platform-support"><img src="https://img.shields.io/badge/Platforms-macOS%20%7C%20Linux%20%7C%20Windows-1F2937?style=flat-square&logo=apple&logoColor=white" alt="macOS, Linux and Windows"></a>
  <a href="#project-status"><img src="https://img.shields.io/badge/Status-Experimental-F59E0B?style=flat-square" alt="Experimental"></a>
  <a href="LICENSE.txt"><img src="https://img.shields.io/badge/License-MIT-1F2937?style=flat-square" alt="MIT license"></a>
</p>

[**Download & Install**](#download--install) &nbsp;·&nbsp;
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
| **Cross-platform development** | Native macOS/Linux and Windows x86-64 CLI builds; WSL2 is also available |
| **Validation** | Offline regression tests, sanitizers, and Clang static analysis |

> [!IMPORTANT]
> NERO is a **research and learning project**, not a production-ready security product.
> Network features must only be used in isolated labs or against systems you are
> explicitly authorized to test.

## Download & Install

<div align="center">

[**Download Source (.zip)**](https://github.com/thekiran/nero/archive/refs/heads/main.zip) &nbsp;·&nbsp;
[**Clone Repository**](https://github.com/thekiran/nero) &nbsp;·&nbsp;
[**GitHub Releases**](https://github.com/thekiran/nero/releases)

<sub>Versioned native packages for macOS, Linux and Windows are prepared by GitHub Actions.
The first release stays a draft until reviewed; drafts are not public downloads.</sub>

</div>

<br>

Choose your operating system below. Each command block has GitHub's built-in
**Copy** button so you can paste the steps directly into your terminal.

### Prebuilt release packages

Open **[GitHub Releases](https://github.com/thekiran/nero/releases)** and select a
published version. Download the package matching your OS and CPU, plus its
`.sha256` file. If no version is published yet, use the source instructions below.

| Your system | Package |
| :--- | :--- |
| macOS 15+, Apple Silicon | `nero-vX.Y.Z-macos-arm64.tar.gz` |
| macOS 15+, Intel | `nero-vX.Y.Z-macos-x86_64.tar.gz` |
| Ubuntu 24.04 compatible, Intel/AMD | `nero-vX.Y.Z-linux-x86_64.tar.gz` |
| Ubuntu 24.04 compatible, ARM64 | `nero-vX.Y.Z-linux-arm64.tar.gz` |
| Windows 10/11, Intel/AMD | `nero-vX.Y.Z-windows-x86_64.zip` (native `nero.exe`) |

Verify the downloaded archive before extracting:

```bash
# macOS (replace the filename with your downloaded package)
shasum -a 256 -c nero-vX.Y.Z-macos-arm64.tar.gz.sha256
# Linux / WSL2
sha256sum -c nero-vX.Y.Z-linux-x86_64.tar.gz.sha256
```

Extract the selected archive. On macOS/Linux, run `sudo sh install.sh` inside
its extracted folder to install `nero` into `/usr/local/bin`. You can then run
`nero` or `sudo nero` from any directory. Linux packages require runtime libraries; macOS packages are
unsigned and use system frameworks. See the **[package installation guide](docs/releases.md)**
for complete copyable commands, dependencies and supported OS versions.

### macOS — native

**Requirements:** macOS, Apple Command Line Tools, and Git.

<details>
<summary>First time on macOS? Install developer tools</summary>

Run this once in Terminal if the tools are missing:

```bash
xcode-select --install
```

</details>

**1. Download and build:**

```bash
git clone https://github.com/thekiran/nero.git && cd nero && make CC=clang -j4
```

**2. Install once, then run from any directory:**

```bash
sudo make install
nero
# For operations that require administrator privileges:
sudo nero
```

Works with the macOS native CLI. Some experimental network functions may need
additional permissions and may behave differently across OS versions.

### Linux — native

**Recommended:** Ubuntu or Debian-based distributions. Other Linux
distributions may require different dependency package names.

**1. Install build dependencies:**

```bash
sudo apt update && sudo apt install -y git build-essential libcurl4-openssl-dev libcjson-dev libldns-dev libncurses-dev python3 clang
```

**2. Download and build:**

```bash
git clone https://github.com/thekiran/nero.git && cd nero && make -j"$(nproc)"
```

**3. Install once, then run from any directory:**

```bash
sudo make install
nero
# For operations that require administrator privileges:
sudo nero
```

The Linux build includes experimental network modules. A successful build is
not a guarantee that every feature works in your environment.

### Windows — native x86-64

Download `nero-vX.Y.Z-windows-x86_64.zip` and its `.sha256` file from a published
[release](https://github.com/thekiran/nero/releases). The ZIP contains a real
MinGW-w64 Windows CLI, statically linked to its compiler runtime; no MSYS2 or
WSL installation is needed to run the downloaded executable.

In PowerShell, replace the example version with your downloaded version:

```powershell
$package = "nero-v0.1.1-windows-x86_64"
$expected = ((Get-Content "$package.zip.sha256").Trim() -split '\s+')[0]
$actual = (Get-FileHash "$package.zip" -Algorithm SHA256).Hash
if ($actual -ne $expected) { throw "Checksum mismatch" }
Expand-Archive "$package.zip" -DestinationPath .
& ".\$package\install.ps1"
nero
```

The installer copies `nero.exe` to `%LOCALAPPDATA%\Programs\Nero\bin` and
adds that directory to your user PATH. Open a new terminal after installation;
`nero` then works from any directory. For all Windows users, run
`install.ps1 -PathScope Machine` from Administrator PowerShell; this uses
`%ProgramFiles%\Nero\bin` and the machine PATH. Windows elevation uses an
Administrator terminal; `sudo` is not a requirement of the native Windows CLI.
See [manual Windows installation](docs/releases.md#native-windows-command-installation)
if your PowerShell script policy blocks the installer.

Use Windows Terminal / PowerShell for the interactive CLI. The binary is
unsigned. Native Windows supports the terminal, interface selection and TCP
code paths; the raw packet engine (`-Syn`) explicitly reports unsupported.
The release tests exercise offline CLI behavior and adapter selection, not
live scanning or raw traffic. Windows ARM64 has no native package.

To build from source in an **MSYS2 UCRT64** shell:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python
git clone https://github.com/thekiran/nero.git
cd nero
cmake -S . -B .build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
cmake --build .build/windows --parallel 4
ctest --test-dir .build/windows --output-on-failure
powershell.exe -NoProfile -File scripts/install.ps1 -BinaryPath .build/windows/nero.exe
nero
```

### Windows — optional WSL2 route

In Administrator PowerShell, run `wsl --install -d Ubuntu-24.04`, complete the
Ubuntu setup, then follow the Linux installation steps inside Ubuntu. Choose
the Linux package matching `uname -m`. WSL2 networking differs from native Linux.
See the [complete installation guide](docs/releases.md#windows-11--wsl2).

### For developers

Prefer Git? This single command downloads the complete repository on any
system with Git installed:

```bash
git clone https://github.com/thekiran/nero.git
```

Prefer a ZIP? Use **[Download Source ZIP](https://github.com/thekiran/nero/archive/refs/heads/main.zip)**,
then extract it and follow the relevant platform's build steps.

For CMake, static analysis, sanitizers and test presets, consult the
[development and build guide](docs/building.md).

### Automated releases

Version tags such as `v0.1.0` trigger native tests, ASan/UBSan checks, builds and
packaging for all five platforms. Each archive includes build metadata and a
SHA-256 sidecar; `SHA256SUMS` covers the complete release. Releases remain drafts
for review. See the [maintainer release procedure](docs/releases.md#maintainer-release-procedure).

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

On Linux, use the `linux` preset instead. The Windows Visual Studio preset builds
only the foundation; the native CLI uses MinGW-w64, as documented above.

## Platform support

| Capability | macOS | Linux | Windows |
| :--- | :---: | :---: | :---: |
| Native interactive CLI | Yes | Yes | Yes (x86-64 MinGW) |
| Terminal rendering and history | Yes | Yes | Yes |
| Offline development tests | Yes | Yes | Foundation + native CLI |
| Network modules | Included* | Included* | TCP diagnostics/scanner; raw engine unavailable |
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

