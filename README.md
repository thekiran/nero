
<div align="center">

<br />

# NERO

### Network Engineering & Research Operations

**An experimental, cross-platform C project for systems programming, terminal development, and authorized network research.**

<br />

[![Language](https://img.shields.io/badge/Language-C-24292f?style=flat-square&logo=c&logoColor=white)](https://en.cppreference.com/w/c)
[![macOS](https://img.shields.io/badge/macOS-Development-24292f?style=flat-square&logo=apple&logoColor=white)](docs/platform-compatibility.md)
[![Linux](https://img.shields.io/badge/Linux-Platform_Target-24292f?style=flat-square&logo=linux&logoColor=white)](docs/platform-compatibility.md)
![Status](https://img.shields.io/badge/Status-Experimental-d97706?style=flat-square)
![Research](https://img.shields.io/badge/Use-Authorized_Research-0969da?style=flat-square)

<br />

[**Overview**](#overview) ·
[**Features**](#features) ·
[**Architecture**](#architecture) ·
[**Installation**](#installation) ·
[**Documentation**](#documentation)

<br />

---

<sub>
Built for systems research, controlled experimentation, and cross-platform development.
</sub>

</div>

<br />

## Overview

**Nero** is an experimental systems-level project developed in C, exploring the intersection of network engineering, terminal interfaces, and platform-specific programming.

It provides a development environment for studying low-level software behavior, validating terminal functionality, and researching network resilience within explicitly authorized environments.

The project is structured around platform-dependent capabilities, allowing development and validation workflows to remain separated from restricted network-testing functionality.

### Design principles

| Principle | Description |
|:---|:---|
| **Systems-oriented** | Native C development with an emphasis on operating system behavior and low-level interfaces. |
| **Cross-platform development** | Platform-specific build configurations for macOS and Linux. |
| **Controlled execution** | Network functionality is constrained by platform and build configuration. |
| **Testability** | Offline validation, sanitizer-enabled testing, and static analysis. |
| **Research-focused** | Designed for experimentation and authorized defensive research rather than production deployment. |

> [!IMPORTANT]
> **Authorized environments only.**
>
> Nero must only be used in isolated laboratories or on systems for which explicit authorization has been obtained.
>
> The project is not intended for public, production, or unauthorized network testing.

<br />

---

## Features

Nero combines terminal development, platform compatibility research, and controlled network experimentation within a native C codebase.

<table>
<tr>
<td width="50%" valign="top">

### Terminal Environment

Interactive terminal development with support for:

- ANSI escape sequences and color rendering
- ASCII-based visual elements
- In-memory command history
- Terminal input processing
- Terminal and PTY behavior research

</td>
<td width="50%" valign="top">

### Network Research

Controlled network testing capabilities for:

- Authorized resilience evaluation
- Network behavior experimentation
- Firewall testing
- IDS/IPS validation
- Laboratory-based research

Network functionality is subject to platform and build restrictions.

</td>
</tr>
<tr>
<td width="50%" valign="top">

### Platform Engineering

Development and compatibility testing across supported environments.

- Native C compilation
- macOS system compiler support
- Linux-specific implementations
- Platform-dependent source selection
- Build configuration validation

</td>
<td width="50%" valign="top">

### Quality & Validation

Development workflows focused on correctness and reproducibility.

- Offline test execution
- No-network test adapters
- Sanitizer-enabled testing
- Static code analysis
- Platform compatibility documentation

</td>
</tr>
</table>

<br />

---

## Architecture

Nero separates its development interface, platform-specific implementations, and network-related capabilities through build-time configuration.

The following diagram illustrates the project's **conceptual capability boundaries**, rather than an exact source-directory or module dependency map.

```text
                       NERO
              Experimental C Project
                          |
          +---------------+---------------+
          |                               |
          v                               v
   TERMINAL DEVELOPMENT           PLATFORM SUPPORT
          |                               |
   +------+------+                  +-----+-----+
   |      |      |                  |           |
   v      v      v                  v           v
 Input   ANSI   History            macOS       Linux
   |      |      |                  |           |
   +------+------+                  |           |
          |                         |           |
          v                         v           v
   Interactive CLI             Development   Linux-specific
                                Interface    Components
                                    |           |
                                    v           v
                              Offline Tests  Restricted
                              & Validation   Network Engine
                                    |           |
                                    +-----+-----+
                                          |
                                          v
                                  Build Configuration
                                  & Platform Controls
```

### Platform boundaries

**macOS development target**

The macOS executable provides terminal development functionality without linking the Linux load engine or port scanner.

**Linux target**

Linux retains its existing source selection and library dependencies. Network-related functionality must be reviewed before execution and restricted to approved environments.

**Offline validation**

Offline testing uses a no-network adapter, ensuring that tests do not invoke the Linux load engine.

> [!NOTE]
> The diagram describes the separation of capabilities. For implementation-specific behavior and known limitations, consult the platform compatibility documentation.

<br />

---

## Platform Compatibility

Nero supports different capabilities depending on the target operating system and build configuration.

| Capability | macOS | Linux |
|:---|:---:|:---:|
| Native C development | Yes | Yes |
| Terminal interface | Yes | Yes |
| ANSI rendering | Yes | Yes |
| ASCII graphics | Yes | Yes |
| Command history | Yes | Yes |
| Local interface enumeration harness | Yes | Platform-dependent |
| Offline tests | Yes | Yes |
| Linux load engine | Not linked | Build-dependent |
| Port scanner | Not linked | Build-dependent |
| Sanitizer validation | Available target | Available target |
| Static analysis | Clang | Clang |

**Compatibility notes**

- macOS builds use the native system compiler and SDK.
- No Homebrew-specific paths or third-party libraries are required for the macOS development target.
- The `-Syn` operation is unavailable in the macOS executable.
- The macOS interface test harness enumerates local interfaces without generating network probes.
- Linux retains its existing library dependencies.
- Availability of network-related components does not imply authorization to execute them.

Full details are available in the [Platform Compatibility Guide](docs/platform-compatibility.md).

<br />

---

## Installation

### Prerequisites

Development requirements depend on the target operating system.

| Requirement | macOS | Linux |
|:---|:---|:---|
| C compiler | Apple Clang | GCC or compatible compiler |
| Build system | Make | Make |
| Python 3 | For tests | For tests |
| Clang | For analysis | For analysis |
| Platform libraries | System SDK | Linux development dependencies |

### 1. Clone the repository

```bash
git clone https://github.com/thekiran/nero.git
cd nero
```

### 2. Select your platform

<details open>
<summary><strong>macOS — Native development build</strong></summary>

<br />

Build Nero using the system C compiler:

```bash
make
```

To explicitly use Clang:

```bash
make CC=clang
```

Run the available validation targets:

```bash
make test
make test-sanitize
make analyze
```

The macOS development target supports terminal functionality while excluding the Linux load engine and port scanner.

</details>

<details>
<summary><strong>Linux — Development build</strong></summary>

<br />

Install the required dependencies on Debian or Ubuntu-based distributions:

```bash
sudo apt update
sudo apt install \
  build-essential \
  libcurl4-openssl-dev \
  libcjson-dev \
  libldns-dev \
  libncurses-dev \
  python3 \
  clang
```

Build the project:

```bash
make
```

Run offline validation where supported:

```bash
make test
```

The legacy installation workflow also includes:

```bash
make install
sudo nero
```

**Warning:** Review the executable, build configuration, and network-related capabilities before using the legacy installation workflow.

Do not run network functionality outside an authorized laboratory.

</details>

<br />

---

## Development

The project provides Make-based development workflows for compilation, testing, and static analysis.

### Build commands

| Command | Description |
|:---|:---|
| `make` | Compile the platform-specific development target. |
| `make CC=clang` | Explicitly select Clang as the compiler. |
| `make test` | Execute the available test suite. |
| `make test-sanitize` | Run sanitizer-enabled validation. |
| `make analyze` | Execute Clang-based static analysis. |

### Recommended validation workflow

```bash
# Build the target
make

# Run the offline tests
make test

# Validate with sanitizers
make test-sanitize

# Run static analysis
make analyze
```

### Validation boundaries

The current validation workflow is intended to evaluate development functionality without executing network load operations.

| Validation area | Scope |
|:---|:---|
| Terminal behavior | Terminal functionality and related interfaces |
| Interface enumeration | Local enumeration without active network probing on macOS |
| Offline testing | No-network adapter |
| Memory safety | Sanitizer-enabled validation |
| Static analysis | Clang-based source analysis |
| Network engine | Excluded from offline tests |

Successful validation does not establish that all platform-specific behavior has been tested or that the project is ready for production.

<br />

---

## Demonstration

### Terminal Preview

A look at Nero's terminal-based interface and visual presentation.

<div align="center">

<img
  src="https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084"
  alt="Nero terminal demonstration"
  width="90%"
/>

<br /><br />

<img
  src="https://github.com/user-attachments/assets/60ef735b-c908-48c2-a8c1-99245a935e03"
  alt="Nero terminal interface"
  width="90%"
/>

</div>

### Video Demonstration

<div align="center">

<a href="https://www.youtube.com/watch?v=2HGtMkmqwpU">
  <img
    src="https://img.youtube.com/vi/2HGtMkmqwpU/hqdefault.jpg"
    alt="Watch the Nero project demonstration"
    width="75%"
  />
</a>

<br />

**[Watch the demonstration on YouTube](https://www.youtube.com/watch?v=2HGtMkmqwpU)**

</div>

<br />

---

## Documentation

Technical documentation is maintained alongside the source code.

| Resource | Description |
|:---|:---|
| [Platform Compatibility](docs/platform-compatibility.md) | Supported environments, platform restrictions, validation scope, and known risks. |
| [Source Repository](https://github.com/thekiran/nero) | Project source code and development history. |
| [Issue Tracker](https://github.com/thekiran/nero/issues) | Bug reports, compatibility issues, and development discussions. |
| [Commit History](https://github.com/thekiran/nero/commits) | Historical changes and development activity. |

### Known limitations

Nero is an experimental project with platform-dependent behavior.

Current limitations include:

- The Linux load engine is not linked into the macOS executable.
- The port scanner is not linked into the macOS executable.
- The `-Syn` operation is not available on macOS.
- Platform-specific behavior may remain untested.
- Network functionality is not included in offline validation.
- Production readiness is neither established nor guaranteed.

Refer to the compatibility documentation for more information.

<br />

---

## Contributing

Contributions that improve code quality, platform compatibility, documentation, testing, and defensive research are welcome.

Areas of particular interest include:

- **Portability:** Improvements to platform-specific C implementations.
- **Terminal development:** Input handling, rendering, and interactive terminal behavior.
- **Testing:** Additional offline test coverage and regression prevention.
- **Code quality:** Memory safety, static analysis, and error handling.
- **Documentation:** Clearer technical documentation and reproducible development procedures.

### Development workflow

1. Fork the repository.
2. Create a descriptive development branch.
3. Implement and validate your changes.
4. Run the relevant tests and analysis tools.
5. Open a pull request describing the changes and validation performed.

Example branch naming:

```text
feat/terminal-improvements
fix/platform-compatibility
test/offline-validation
docs/development-guide
refactor/terminal-core
```

All contributions must respect the project's authorization requirements and network-testing restrictions.

<br />

---

## Security Policy

Nero is intended for authorized education, experimental research, and defensive security validation.

### Responsible use

Users are responsible for ensuring that all testing complies with applicable laws, regulations, organizational policies, and written authorization requirements.

**Permitted research contexts include:**

- Isolated development laboratories
- Personally owned and controlled test systems
- Explicitly authorized research infrastructure
- Approved defensive security environments

**Prohibited uses include:**

- Unauthorized testing of third-party systems
- Disruption of public or production services
- Network activity outside approved environments
- Any activity that violates applicable legal or organizational requirements

> [!CAUTION]
> **Network testing can affect system availability.**
>
> Never assume that an accessible system is an authorized testing target. Obtain explicit permission, verify the intended scope, and use an isolated environment whenever possible.

The maintainers do not endorse malicious activity, unauthorized testing, or disruption of services.

<br />

---

## Project Status

**Nero is under experimental development.**

The project is intended as a research and development environment. Its interfaces, internal implementations, and platform-specific capabilities may change as development progresses.

| Area | Status |
|:---|:---|
| Project maturity | Experimental |
| Primary language | C |
| macOS development target | Available |
| Linux development target | Available |
| Offline validation | Available |
| Production deployment | Not recommended |

No production stability or comprehensive platform compatibility guarantees are provided.

<br />

---

<div align="center">

### NERO

**Systems Programming · Terminal Engineering · Authorized Network Research**

<br />

[Repository](https://github.com/thekiran/nero) ·
[Documentation](docs/platform-compatibility.md) ·
[Issues](https://github.com/thekiran/nero/issues) ·
[Contributions](https://github.com/thekiran/nero/pulls)

<br />

<sub>
Experimental software for controlled research and development.
</sub>

<br /><br />

</div>
