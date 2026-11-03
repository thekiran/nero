
<div align="center">

# NERO

### Native Network & Terminal Research

**A C-based project for low-level systems engineering, terminal development, and controlled network research.**

[![C](https://img.shields.io/badge/C-Native-24292f?style=flat-square&logo=c&logoColor=white)](https://en.cppreference.com/w/c)
[![macOS](https://img.shields.io/badge/macOS-Development-24292f?style=flat-square&logo=apple&logoColor=white)](docs/platform-compatibility.md)
[![Linux](https://img.shields.io/badge/Linux-Target-24292f?style=flat-square&logo=linux&logoColor=white)](docs/platform-compatibility.md)
![Status](https://img.shields.io/badge/Status-Experimental-orange?style=flat-square)

<br>

<img src="https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084" alt="Nero terminal demonstration" width="90%">

<br>

[Getting Started](#getting-started) ·
[Features](#features) ·
[Architecture](#architecture) ·
[Compatibility](#platform-compatibility) ·
[Contributing](#contributing)

</div>

---

## Overview

**Nero** is an experimental systems programming project written in C, focused on native terminal interfaces, operating system compatibility, and authorized network research.

The project provides a development environment for exploring low-level system behavior while keeping platform-specific network components separate from offline testing.

Nero is under active development and is not intended for production deployment.

## Features

| Component | Description |
|:--|:--|
| **Terminal Interface** | Interactive CLI, ANSI rendering, and ASCII graphics |
| **Command History** | In-memory command history and input handling |
| **Platform Support** | Native macOS development and Linux-specific functionality |
| **Network Research** | Controlled network evaluation in authorized environments |
| **Offline Testing** | Network-independent validation workflows |
| **Code Analysis** | Sanitizer testing and Clang static analysis |

> [!NOTE]
> Feature availability depends on the operating system and build configuration. The macOS development target does not include the Linux load engine or port scanner.

## Getting Started

### Requirements

- C compiler (Clang or GCC)
- GNU Make
- Python 3 for test workflows
- Platform-specific development libraries where required

### Clone

```bash
git clone https://github.com/thekiran/nero.git
cd nero
```

### macOS

The macOS version runs the interactive development CLI and offline tests.
It does not include the Linux network load engine or port scanner.

Install Apple's Command Line Tools if they are not already available:

```bash
xcode-select --install
```

From the repository root, build and launch Nero (no `sudo` required):

```bash
make CC=clang -j4 && ./nero
```

At the `►` prompt, try `help`, `version`, `history`, and `exit`.
In the standard macOS build, an IPv4 address followed by `-Syn` reports
`unsupported in this build; no network traffic is generated.`
This is expected platform behavior.

Run the offline checks with Python 3 installed:

```bash
make test
make test-sanitize
```

Alternatively, with CMake 3.21 or newer installed:

```bash
cmake --preset macos
cmake --build --preset macos -j4
ctest --preset macos
.build/cmake/macos/nero-safe
```

See [build documentation](docs/building.md) for additional details.

### Linux

Install dependencies on Debian/Ubuntu:

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
```

Build:

```bash
make
```

Platform-specific components depend on the selected build configuration.

### Testing

```bash
make test           # Run offline tests
make test-sanitize  # Run sanitizer-enabled tests
make analyze        # Run static analysis
```

These targets help validate changes without requiring live network testing.

## Architecture

Nero separates terminal functionality, platform-specific implementations, and validation workflows.

```text
                      NERO
                        |
          +-------------+-------------+
          |                           |
    Terminal Interface          Platform Layer
          |                           |
    +-----+------+              +-----+-----+
    | Input      |              | macOS     |
    | History    |              | Linux     |
    | Rendering  |              | Toolchain |
    +------------+              +-----------+
          |                           |
          +-------------+-------------+
                        |
                 Validation
                        |
                +-------+-------+
                | Offline Tests |
                | Sanitizers    |
                | Static Analysis|
                +---------------+
```

The architecture prioritizes:

- **Platform isolation** — Operating system-specific components are selected at build time.
- **Offline validation** — Development tests avoid executing network load functionality.
- **Native implementation** — Core functionality uses C and operating system interfaces.

## Platform Compatibility

| Capability | macOS | Linux |
|:--|:---:|:---:|
| Native C build | Yes | Yes |
| Interactive terminal | Yes | Yes |
| ANSI / ASCII rendering | Yes | Yes |
| Command history | Yes | Yes |
| Offline testing | Yes | Yes |
| Sanitizer testing | Available | Available |
| Static analysis | Available | Available |
| Network load engine | Excluded | Build-dependent |
| Port scanner | Excluded | Build-dependent |

For implementation details and known limitations, see [Platform Compatibility](docs/platform-compatibility.md).

## Demonstration

<div align="center">

<img src="https://github.com/user-attachments/assets/60ef735b-c908-48c2-a8c1-99245a935e03" alt="Nero terminal interface" width="85%">

<br><br>

[![Nero Demonstration](https://img.youtube.com/vi/2HGtMkmqwpU/hqdefault.jpg)](https://www.youtube.com/watch?v=2HGtMkmqwpU)

**[Watch Demonstration](https://www.youtube.com/watch?v=2HGtMkmqwpU)**

</div>

## Contributing

Contributions are welcome in the following areas:

- C code quality and memory safety
- Terminal functionality
- macOS and Linux compatibility
- Automated testing and static analysis
- Build system improvements
- Technical documentation

To contribute:

1. Fork the repository.
2. Create a focused feature or fix branch.
3. Implement and validate your changes.
4. Open a pull request with a clear description and test results.

Please include the operating system, compiler version, and relevant test results when reporting bugs.

[Open an Issue](https://github.com/thekiran/nero/issues) · [Submit a Pull Request](https://github.com/thekiran/nero/pulls)

## Security & Responsible Use

> [!CAUTION]
> Nero is intended for **isolated laboratories and explicitly authorized testing environments**.
>
> Network-related functionality may affect system or service availability. Do not execute it against third-party, public, or production infrastructure without explicit authorization.

The project is experimental. Security, reliability, and production readiness are not guaranteed.

## License

Nero is open-source software licensed under the [MIT License](LICENSE.txt).

Copyright © 2025–2026 Muhammed Emin Kıran.

---

<div align="center">

**NERO**

Native C · Systems Engineering · Network Research

[Source Code](https://github.com/thekiran/nero) ·
[Documentation](docs/platform-compatibility.md) ·
[Issues](https://github.com/thekiran/nero/issues)

<sub>Built for controlled research and development.</sub>

</div>
