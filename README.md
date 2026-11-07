<div align="center">

# NERO

**Native C Terminal & Network Research**

![C11](https://img.shields.io/badge/C11-2563EB?style=flat-square&logo=c&logoColor=white)
![Platforms](https://img.shields.io/badge/macOS%20%7C%20Linux%20%7C%20Windows-1F2937?style=flat-square)
[![MIT](https://img.shields.io/badge/License-MIT-1F2937?style=flat-square)](LICENSE.txt)

A native interactive terminal with command history, platform adapters and experimental
network diagnostics, written in C11.

[**Releases**](https://github.com/thekiran/nero/releases) ·
[**Installation**](docs/installation.md) ·
[**Command Reference**](docs/command-reference.md) ·
[**Documentation**](docs/README.md) ·
[**GitHub Wiki**](https://github.com/thekiran/nero/wiki) ·
[**Roadmap**](ROADMAP.md)

<img src="https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084" alt="NERO terminal screenshot" width="780">

</div>

## Overview and Platform Support

NERO provides a unified terminal interface across POSIX and Windows environments. Supported platforms are verified through native automated packaging and CI test runs:

| Platform | Architecture | Binary Type | Verification Evidence |
| --- | --- | --- | --- |
| **macOS** 15+ | Apple Silicon (ARM64) | Native Mach-O 64-bit | Make, CMake, PTY tests, ASan/UBSan |
| **macOS** 15+ | Intel (x86-64) | Native Mach-O 64-bit | Make, CMake, PTY tests, ASan/UBSan |
| **Ubuntu** 24.04+ compatible | x86-64 | Native ELF 64-bit | Make, CMake, PTY tests, ASan/UBSan |
| **Ubuntu** 24.04+ compatible | ARM64 | Native ELF 64-bit | Make, CMake, PTY tests, ASan/UBSan |
| **Windows** 10 / 11 | x86-64 | Native PE32+ (MinGW UCRT64) | CMake + Ninja, Win32 console/socket CTest |
| **Windows (WSL2)** | x86-64 / ARM64 | Linux package inside Ubuntu | Standard Linux execution path |

Windows ARM64 has no native package; use WSL2 on ARM64 hardware.

---

## Feature Status

* **Implemented & Verified:**
  * Interactive terminal shell, command tokenization, and line editor (history search, cursor motion, backspace, delete).
  * ANSI VT100 styling with automatic fallback to ASCII when `NO_COLOR=1` or `TERM=dumb`.
  * Network interface enumeration across POSIX (`getifaddrs`) and Windows (`GetAdaptersAddresses`).
  * TCP port diagnostics (`<target> -Sc`) with timeout boundaries.
  * System command installation scripts for `/usr/local/bin` (macOS/Linux) and `%LOCALAPPDATA%\Programs\Nero\bin` (Windows).
* **Experimental Modules (Authorized Laboratory Use Only):**
  * Raw packet engine (`<target> -Syn`) on Linux and macOS. Requires administrative root elevation (`sudo nero`) for `SOCK_RAW` and BPF access.
* **Unavailable Features:**
  * Raw packet engine (`-Syn`) on Windows: user-space raw TCP injection is restricted by the Windows desktop network stack. The command reports unsupported without transmitting traffic.
* **Planned Features:**
  * Authenticode and Apple Developer ID signing certificates (tracked in [ROADMAP.md](ROADMAP.md)).
  * Distribution via package managers (Homebrew, APT).

---

## Quick Installation

Download the verified release package matching your operating system from [GitHub Releases](https://github.com/thekiran/nero/releases) and verify its `.sha256` checksum.

### macOS & Linux
```bash
# Extract and run the installer
tar -xzf nero-v*.tar.gz
cd nero-v*
sudo sh install.sh

# Run NERO
nero
```

### Windows (Native PowerShell)
```powershell
# Extract package and install to user PATH
Expand-Archive nero-v*-windows-x86_64.zip -DestinationPath .
& ".\nero-v*-windows-x86_64\install.ps1"

# Run NERO (open a fresh terminal for updated PATH)
nero
```

For complete step-by-step instructions, checksum validation, dependencies, and building from source, see the **[Installation Guide](docs/installation.md)**.

---

## Usage Example

Launch the interactive shell:

```text
$ nero

░▒▓███████▓▒░ ░▒▓████████▓▒░░▒▓███████▓▒░  ░▒▓██████▓▒░
░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░
░▒▓█▓▒░░▒▓█▓▒░░▒▓██████▓▒░  ░▒▓███████▓▒░ ░▒▓█▓▒░░▒▓█▓▒░
░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░       ░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░
░▒▓█▓▒░░▒▓█▓▒░░▒▓████████▓▒░░▒▓█▓▒░░▒▓█▓▒░ ░▒▓██████▓▒░

► help
Available Commands:
  help                 : Show this help message
  version              : Show program version
  history              : Show command history
  history -clear       : Clear command history
  clear                : Clear the screen
  exit                 : Quit the shell
  <target> -Sc         : Check TCP ports (offline builds simulate)
  -mode [on|off]       : Configure network diagnostics

► version
version 1.0

► exit
Exiting...
```

You can also pipe commands into standard input:

```bash
printf "version\nhelp\nexit\n" | nero
```

See the **[Command Reference](docs/command-reference.md)** for syntax, parameter options, and execution constraints.

---

## Testing and Quality Assurance

All automated CI workflows run strictly in **offline-safe** mode: tests do not generate unsolicited packets or contact external servers.

```bash
# macOS / Linux (Make)
make test             # Core regressions and terminal PTY tests
make test-sanitize    # AddressSanitizer and UndefinedBehaviorSanitizer

# CMake cross-platform presets
cmake --preset <macos|linux|windows>
cmake --build --preset <macos|linux|windows>
ctest --preset <macos|linux|windows> --output-on-failure
```

Detailed test architectures, mock fixtures, and Win32 loopback tests are documented in **[Testing and QA](docs/testing.md)**.

---

## Documentation Library

* **[Documentation Hub](docs/README.md):** Complete index of technical guides and review history.
* **[GitHub Wiki](https://github.com/thekiran/nero/wiki):** Automated mirror of all project documentation.
* **[Architecture](docs/architecture.md):** Modular design, header contracts, and build boundaries.
* **[Building & Toolchains](docs/building.md):** Make and CMake presets, compilers, and code formatting.
* **[Troubleshooting](docs/troubleshooting.md):** Solutions for shell PATH overflows, terminal resets, and build issues.
* **[Contributing](CONTRIBUTING.md):** Pull request expectations, review criteria, and safety agreements.
* **[Security Policy](SECURITY.md):** Responsible reporting and vulnerability disclosure.
* **[Roadmap](ROADMAP.md):** Milestone targets and release criteria.

---

## Security Notice

NERO is experimental software intended exclusively for isolated laboratories and explicitly authorized testing. Do not use experimental network modules on untrusted networks or without authorization. See [SECURITY.md](SECURITY.md) and [Laboratory Safety](docs/laboratory-safety.md).

Green CI runs verify offline behavior and package integrity; they do not imply certification for production security environments.

---

[Demonstration Video](https://www.youtube.com/watch?v=2HGtMkmqwpU) · [Report an Issue](https://github.com/thekiran/nero/issues/new/choose) · [Changelog](CHANGELOG.md)

Distributed under the [MIT License](LICENSE.txt).
Copyright © 2025–2026 Muhammed Emin Kıran.
