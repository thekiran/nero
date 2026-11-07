<div align="center">

# NERO

**Native C Terminal & Network Research**

![C11](https://img.shields.io/badge/C11-2563EB?style=flat-square&logo=c&logoColor=white)
![Platforms](https://img.shields.io/badge/macOS%20%7C%20Linux%20%7C%20Windows-1F2937?style=flat-square)
[![MIT](https://img.shields.io/badge/License-MIT-1F2937?style=flat-square)](LICENSE.txt)

A native terminal with command history, platform adapters and experimental
network diagnostics, written in C11.

[**Download**](https://github.com/thekiran/nero/releases) ·
[**Installation**](docs/installation.md) ·
[**Documentation**](docs/README.md) ·
[**Roadmap**](ROADMAP.md)

<img src="https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084" alt="NERO terminal screenshot" width="780">

</div>

## Install and run

Download the archive matching your OS/CPU and verify its SHA-256 checksum.
Versions remain drafts until reviewed and published; drafts are visible only to
repository collaborators. Source installation is available meanwhile.

| System | Native package | After installation |
| --- | --- | --- |
| macOS 15+, Apple Silicon / Intel | ARM64 / x86-64 tar.gz | `nero` or `sudo nero` |
| Ubuntu 24.04 compatible | x86-64 / ARM64 tar.gz | `nero` or `sudo nero` |
| Windows 10/11, Intel/AMD | x86-64 ZIP with real `nero.exe` | `nero`; Administrator Terminal when needed |

Follow the [platform installation guide](docs/installation.md) for downloads,
dependencies, checksum verification, system PATH, source builds and WSL2.

## What is supported

- Interactive terminal, history, line editing and ANSI/ASCII rendering.
- Native macOS/Linux CLI and Windows MinGW x86-64 CLI.
- Offline regressions, POSIX PTY tests, ASan/UBSan and native package checks.
- Experimental network modules with different support and permission requirements.

Windows excludes the raw packet engine. Live scanner/engine behavior is not
certified by offline CI. See the [support matrix](docs/platform-support.md).

## Explore and contribute

Start with `help`, `version`, `history` and `exit` inside the CLI.

| Guide | Contents |
| --- | --- |
| [Building and testing](docs/building.md) | Toolchains, CMake presets and behavioral checks |
| [Architecture](docs/architecture.md) | CLI, platform boundaries and ownership |
| [Code map](docs/code-map.md) | Where to find each responsibility |
| [Release guide](docs/releases.md) | Package scope, checksums and draft procedure |
| [Changelog](CHANGELOG.md) | Prepared versions and proposed changes |
| [Contributing](CONTRIBUTING.md) | Focused PRs, review and validation expectations |

NERO is experimental software for isolated laboratories and explicitly authorized
testing. See [SECURITY.md](SECURITY.md) and the [code of conduct](CODE_OF_CONDUCT.md).
Do not infer production readiness or security certification from successful CI.

[Terminal demonstration](https://www.youtube.com/watch?v=2HGtMkmqwpU) ·
[Report a bug](https://github.com/thekiran/nero/issues/new/choose)

Distributed under the [MIT License](LICENSE.txt).
Copyright © 2025–2026 Muhammed Emin Kıran.
