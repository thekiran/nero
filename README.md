
<div align="center">

<br>

# N E R O

### NETWORK ENGINEERING & RESEARCH OPERATIONS

**Native C. Cross-platform development. Controlled network research.**

An experimental systems programming project built for terminal engineering, platform compatibility research, and authorized network testing.

<br>

[![C](https://img.shields.io/badge/Language-C-1f2328?style=for-the-badge&logo=c&logoColor=white)](https://en.cppreference.com/w/c)
[![macOS](https://img.shields.io/badge/macOS-Development-1f2328?style=for-the-badge&logo=apple&logoColor=white)](docs/platform-compatibility.md)
[![Linux](https://img.shields.io/badge/Linux-Supported_Target-1f2328?style=for-the-badge&logo=linux&logoColor=white)](docs/platform-compatibility.md)
[![Status](https://img.shields.io/badge/Status-Experimental-b45309?style=for-the-badge)](docs/platform-compatibility.md)

<br>

[**Getting Started**](#getting-started) &nbsp;·&nbsp;
[**Architecture**](#architecture) &nbsp;·&nbsp;
[**Platform Support**](#platform-support) &nbsp;·&nbsp;
[**Contributing**](#contributing) &nbsp;·&nbsp;
[**Security**](#security--responsible-use)

<br>

---

**A research-oriented C project for engineers interested in low-level systems, terminal interfaces, and platform-specific development.**

</div>

<br>

## Introduction

**Nero** is an experimental C-based project focused on systems programming, terminal interface development, network resilience research, and cross-platform compatibility.

Rather than providing a general-purpose production network utility, Nero serves as a controlled development and research environment.

It explores the interaction between operating systems, terminal interfaces, native code, and network-related functionality.

### Core objectives

- Develop and validate native C implementations.
- Explore terminal input, command history, and rendering behavior.
- Improve compatibility across macOS and Linux.
- Support reproducible offline testing and code analysis.
- Research network resilience within authorized laboratory environments.
- Maintain clear boundaries between platform development and network functionality.

> [!IMPORTANT]
> **Authorized research only**
>
> Nero is intended exclusively for isolated laboratory environments and systems with explicit testing authorization.
>
> Do not use network-related components against public, production, or third-party infrastructure without permission.

<br>

## At a Glance

| | |
|---|---|
| **Project** | Nero |
| **Primary Language** | C |
| **Category** | Systems Programming / Network Research |
| **Development Platforms** | macOS, Linux |
| **Build System** | GNU Make / platform C toolchain |
| **Testing** | Offline tests, sanitizers, static analysis |
| **Maturity** | Experimental |
| **Intended Use** | Authorized research and development |

<br>

## Capabilities

<table>
<tr>
<td width="50%" valign="top">

### 01 / Terminal Engineering

Development and experimentation with native terminal behavior.

- Interactive command-line interface
- ANSI escape sequence rendering
- ASCII graphics
- In-memory command history
- Terminal input processing
- PTY-related research

</td>
<td width="50%" valign="top">

### 02 / Platform Compatibility

Native C development across different operating systems.

- macOS system compiler integration
- Linux-specific components
- Platform-dependent source selection
- Build configuration separation
- Compatibility validation
- Operating system interface research

</td>
</tr>
<tr>
<td width="50%" valign="top">

### 03 / Network Research

Network experimentation in controlled environments.

- Authorized resilience testing
- Network behavior evaluation
- Defensive tooling assessment
- Firewall testing
- IDS/IPS validation

Network functionality is restricted by platform and build configuration.

</td>
<td width="50%" valign="top">

### 04 / Development Quality

Testing workflows intended to improve correctness.

- Offline test adapters
- Sanitizer-enabled validation
- Clang static analysis
- Platform-specific checks
- Reproducible development commands
- Documented compatibility limitations

</td>
</tr>
</table>

<br>

---

## Getting Started

Start with a development-only build before working with any platform-specific functionality.

### 1. Clone

```bash
git clone https://github.com/thekiran/nero.git
cd nero
```

### 2. Build

For macOS:

```bash
make
```

Explicitly use Clang if required:

```bash
make CC=clang
```

For Linux, install the dependencies described in the [Linux setup](#linux-setup) section before building.

### 3. Validate

```bash
make test
make test-sanitize
make analyze
```

These targets provide testing and analysis workflows. Their availability and implementation are platform-dependent.

> [!NOTE]
> The macOS executable provides terminal development functionality without linking the Linux load engine or port scanner.
>
> Offline tests use a no-network adapter and do not execute the Linux load engine.

<br>

## Architecture

Nero separates platform development from restricted network functionality through platform-specific build configurations.

### Conceptual system architecture

```text
┌─────────────────────────────────────────────┐
│                    NERO                     │
│          Native C Research Project          │
└──────────────────────┬──────────────────────┘
                       │
          ┌────────────┴────────────┐
          │                         │
          ▼                         ▼
┌──────────────────┐      ┌───────────────────┐
│ Terminal Layer   │      │ Platform Layer    │
├──────────────────┤      ├───────────────────┤
│ Input processing │      │ macOS             │
│ ANSI rendering   │      │ Linux             │
│ ASCII graphics   │      │ Native C toolchain│
│ Command history  │      │ Build selection   │
└─────────┬────────┘      └──────────┬────────┘
          │                          │
          └────────────┬─────────────┘
                       │
                       ▼
             ┌───────────────────┐
             │ Validation Layer  │
             ├───────────────────┤
             │ Offline tests     │
             │ Sanitizers        │
             │ Static analysis   │
             └───────────────────┘

  Linux-specific network components are controlled
  separately through platform and build configuration.
```

This diagram represents the project's documented capability boundaries. It is not a claim about the exact internal source directory structure.

### Architectural principles

**Platform isolation**

Platform-specific source selection determines which functionality is included in each executable.

**Offline-first validation**

Tests should not require active network probing or execution of restricted network functionality.

**Native implementation**

The project uses C and system-specific interfaces rather than requiring a cross-platform application runtime.

**Explicit constraints**

Unavailable functionality and untested behavior are documented instead of silently assumed to work.

<br>

---

## Platform Support

Not all Nero capabilities are available on every platform.

| Capability | macOS | Linux |
|---|:---:|:---:|
| C development | Supported | Supported |
| Interactive terminal | Supported | Supported |
| ANSI rendering | Supported | Supported |
| ASCII graphics | Supported | Supported |
| Command history | Supported | Supported |
| Local interface enumeration harness | Supported | Platform-dependent |
| Offline test workflow | Supported | Supported |
| Linux load engine | Not linked | Build-dependent |
| Port scanner | Not linked | Build-dependent |
| Sanitizer validation | Available target | Available target |
| Clang analysis | Available target | Available target |

### macOS

The macOS development target supports the terminal environment using the system C compiler and SDK.

No Homebrew paths or third-party libraries are required for this target.

The following functionality is excluded:

- Linux load engine
- Linux port scanner
- `-Syn` operation

The interface test harness enumerates local interfaces without probing the network.

### Linux

The Linux target retains the existing source list and library dependencies.

Some functionality is platform-specific and may depend on the selected build configuration.

Linux offline testing uses a no-network adapter and does not execute the load engine.

**Additional details:** [Platform Compatibility](docs/platform-compatibility.md)

<br>

## Linux Setup

### Dependencies

For Debian and Ubuntu-based distributions:

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

### Compile

```bash
git clone https://github.com/thekiran/nero.git
cd nero
make
```

### Legacy installation

The existing Linux installation workflow includes:

```bash
make install
sudo nero
```

**Before using these commands, review the build configuration, resulting executable, and authorized testing scope.**

Elevated privileges should not be used unless required for a specific reviewed operation.

<br>

---

## Development Workflow

Nero provides Make-based commands for building, testing, and analyzing the project.

| Target | Purpose |
|---|---|
| `make` | Build the selected platform target |
| `make CC=clang` | Select the Clang compiler explicitly |
| `make test` | Execute the available test suite |
| `make test-sanitize` | Execute sanitizer-enabled tests |
| `make analyze` | Run Clang-based static analysis |

### Recommended workflow

```text
Source Changes
      |
      v
Platform Build
      |
      v
Offline Tests
      |
      v
Sanitizer Validation
      |
      v
Static Analysis
      |
      v
Review Results
```

### Validation commands

```bash
# Compile
make

# Run tests
make test

# Run sanitizer validation
make test-sanitize

# Perform static analysis
make analyze
```

### Validation expectations

Before submitting changes:

1. Confirm the project builds on the target platform.
2. Run the available offline tests.
3. Investigate sanitizer warnings or failures.
4. Review static analysis findings.
5. Document platform-specific limitations.
6. Avoid introducing unexpected network activity into the offline test workflow.

Passing these checks does not imply that every platform or code path has been tested.

<br>

## Troubleshooting

### Build errors

If compilation fails:

- Confirm that the required compiler and Make are installed.
- Check that the platform-specific dependencies are available.
- Review the first compiler error rather than only the final Make failure.
- Confirm that the correct platform target is being built.

On macOS, explicitly select Clang:

```bash
make CC=clang
```

### Missing dependencies on Linux

Install the development libraries listed in [Linux Setup](#linux-setup).

### Static analysis cannot run

The analysis target requires Clang.

Check whether Clang is available:

```bash
clang --version
```

### Python is unavailable

Python 3 is required for the test workflows.

Check the installation:

```bash
python3 --version
```

### Network operations unavailable on macOS

This is an intentional platform restriction, not necessarily a build failure.

The macOS development target excludes the Linux load engine, port scanner, and `-Syn` operation.

### Unexpected behavior

When reporting an issue, include:

- Operating system and version
- Target architecture
- Compiler and version
- Build command
- Relevant error output
- Expected and observed behavior
- Whether the issue occurs during offline validation

Do not publish credentials, private addresses, or sensitive testing infrastructure information in public issue reports.

<br>

---

## Demonstration

### Terminal Interface

The following images demonstrate the Nero terminal environment.

<div align="center">

<img src="https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084"
     alt="Nero terminal demonstration"
     width="90%">

<br><br>

<img src="https://github.com/user-attachments/assets/60ef735b-c908-48c2-a8c1-99245a935e03"
     alt="Nero interface screenshot"
     width="90%">

</div>

### Video

<div align="center">

<a href="https://www.youtube.com/watch?v=2HGtMkmqwpU">
<img src="https://img.youtube.com/vi/2HGtMkmqwpU/hqdefault.jpg"
     width="75%"
     alt="Nero demonstration video">
</a>

<br>

[**Watch the full demonstration**](https://www.youtube.com/watch?v=2HGtMkmqwpU)

</div>

<br>

---

## Project Roadmap

The following items are potential development directions rather than promises of completed functionality.

- [ ] Expand platform-specific compatibility tests
- [ ] Improve terminal input and rendering validation
- [ ] Increase automated test coverage
- [ ] Extend sanitizer and static analysis workflows
- [ ] Document internal module boundaries
- [ ] Improve error reporting and diagnostics
- [ ] Introduce reproducible build verification
- [ ] Evaluate additional CI workflows
- [ ] Expand developer documentation

Roadmap priorities may change as research and development continue.

<br>

## Documentation

| Document | Description |
|---|---|
| [Platform Compatibility](docs/platform-compatibility.md) | Supported targets, known limitations, and validation boundaries |
| [Repository](https://github.com/thekiran/nero) | Source code and project development |
| [Issues](https://github.com/thekiran/nero/issues) | Bug reports and technical discussions |
| [Pull Requests](https://github.com/thekiran/nero/pulls) | Proposed changes and code review |
| [Commit History](https://github.com/thekiran/nero/commits) | Historical project activity |

<br>

---

## Contributing

Nero welcomes contributions focused on legitimate systems programming research, platform compatibility, code correctness, documentation, and defensive testing.

### Areas of interest

- Native C improvements
- Memory safety
- Terminal input handling
- Platform-specific compatibility
- Offline test coverage
- Static analysis
- Documentation
- Build reliability

### Contribution workflow

**1. Fork the repository**

Create your own copy of the project.

**2. Create a branch**

Use a descriptive branch name.

```bash
git checkout -b feat/terminal-improvements
```

Suggested naming conventions:

```text
feat/...
fix/...
refactor/...
test/...
docs/...
```

**3. Validate your changes**

Run the appropriate build, test, and analysis targets.

**4. Create a pull request**

Explain:

- What changed
- Why the change is necessary
- How the change was tested
- Which platforms were tested
- Any known limitations

Contributions must respect the project's authorized-use requirements.

<br>

## Security & Responsible Use

Nero is provided for educational, experimental, and defensive research purposes.

### Authorized environments

Appropriate environments include:

- Isolated laboratories
- Controlled development networks
- Personally owned testing infrastructure
- Explicitly authorized systems
- Approved defensive research environments

### Restrictions

Do not:

- Test infrastructure without explicit permission.
- Disrupt production or public services.
- Assume accessibility implies authorization.
- Run network functionality outside approved environments.
- Bypass established organizational testing policies.

> [!CAUTION]
> **Potential service impact**
>
> Network experiments may affect availability and system behavior. Verify authorization, scope, and isolation before executing any network-related functionality.

Users are responsible for complying with applicable laws, regulations, organizational policies, and authorization requirements.

The maintainers do not endorse malicious activity, unauthorized network testing, or service disruption.

<br>

## Frequently Asked Questions

<details>
<summary><strong>Is Nero production-ready?</strong></summary>

No. Nero is an experimental research and development project. Production deployment is not recommended.

</details>

<details>
<summary><strong>Does Nero support macOS?</strong></summary>

Yes, for the documented macOS development target. Terminal features are available, while the Linux load engine and port scanner are excluded.

</details>

<details>
<summary><strong>Can Nero be developed without performing network tests?</strong></summary>

Yes. The project includes offline validation workflows and a no-network test adapter.

</details>

<details>
<summary><strong>Does Nero require Homebrew on macOS?</strong></summary>

The documented macOS development target does not require Homebrew-specific paths or third-party libraries.

</details>

<details>
<summary><strong>Can I contribute?</strong></summary>

Yes. Contributions to platform compatibility, testing, code quality, terminal development, and documentation are welcome.

</details>

<details>
<summary><strong>Can Nero be used on external systems?</strong></summary>

Only when explicit authorization has been obtained and testing complies with applicable laws and organizational policies.

</details>

<br>

---

## Project Status

Nero is currently an **experimental development project**.

| Area | Current Position |
|---|---|
| Project maturity | Experimental |
| Language | C |
| macOS development | Supported target |
| Linux development | Supported target |
| Offline testing | Available |
| Production readiness | Not established |
| Intended deployment | Controlled laboratories |

API stability, feature completeness, and production compatibility are not guaranteed.

<br>

---

<div align="center">

<br>

# NERO

**Native Systems Research, Built in C.**

<br>

[Repository](https://github.com/thekiran/nero)
&nbsp;·&nbsp;
[Documentation](docs/platform-compatibility.md)
&nbsp;·&nbsp;
[Issues](https://github.com/thekiran/nero/issues)
&nbsp;·&nbsp;
[Contributing](#contributing)

<br>

<sub>
Experimental systems software for authorized research and development.
</sub>

<br>

</div>
