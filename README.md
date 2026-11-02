# NERO

**Experimental Network Testing & Terminal Research Framework**

A C-based experimental project focused on authorized network testing, terminal interface development, and cross-platform systems programming.

[Repository](https://github.com/thekiran/nero) · [Platform Compatibility](https://github.com/thekiran/nero/blob/main/docs/platform-compatibility.md) · [Demonstration](https://www.youtube.com/watch?v=2HGtMkmqwpU)

---

## Overview

**Nero** is an experimental systems programming project written in C, designed to explore network resilience, terminal interaction, and operating system compatibility within controlled research environments.

The project emphasizes low-level development, platform-specific implementations, and reproducible validation.

> **Important:** Nero is intended exclusively for authorized research and isolated laboratory environments. Network testing must never be conducted against systems without explicit permission.

## Features

| Component | Description |
|---|---|
| Network Research | Evaluate network behavior and resilience in controlled environments |
| Terminal Interface | ANSI colors, ASCII graphics, command history, and interactive terminal behavior |
| Platform Compatibility | Develop and validate platform-specific C implementations |
| Defensive Testing | Support authorized firewall and IDS/IPS validation |
| Quality Assurance | Offline testing, sanitizers, and static analysis |

## Platform Support

Nero uses platform-specific build configurations to separate development features from network-related functionality.

| Capability | macOS | Linux |
|---|:---:|:---:|
| Terminal interface | Supported | Supported |
| ANSI rendering | Supported | Supported |
| ASCII graphics | Supported | Supported |
| Command history | Supported | Supported |
| Network interface enumeration | Supported | Platform-dependent |
| Linux load engine | Unavailable | Build-dependent |
| Port scanner | Unavailable | Build-dependent |
| Offline validation | Supported | Supported |

Network-related components are intentionally restricted by platform and build configuration.

For detailed information, refer to the [Platform Compatibility Documentation](docs/platform-compatibility.md).

---

## Getting Started

### macOS

The macOS development target uses the system compiler and SDK. No additional Homebrew libraries are required.

**Build**

```bash
git clone https://github.com/thekiran/nero.git
cd nero
make
```

To explicitly select Clang:

```bash
make CC=clang
```

**Validation**

```bash
make test
make test-sanitize
make analyze
```

The macOS build provides the terminal development interface without linking the Linux load engine or port scanner.

The `-Syn` operation is unavailable on macOS. Interface enumeration tests do not generate network probes.

### Linux

Install the required development dependencies on Debian or Ubuntu-based systems:

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

Clone and build the project:

```bash
git clone https://github.com/thekiran/nero.git
cd nero
make
```

**Legacy installation**

The following commands are retained for development reference. Review the build configuration and executable behavior before installation.

```bash
make install
sudo nero
```

Linux offline validation uses a no-network adapter and does not execute the load engine.

---

## Development & Validation

Nero provides a set of development commands for building, testing, and analyzing the codebase.

| Command | Purpose |
|---|---|
| `make` | Build the platform-specific executable |
| `make test` | Run the available tests |
| `make test-sanitize` | Execute sanitizer-enabled validation |
| `make analyze` | Perform static analysis using Clang |

These commands should be executed in an appropriate development environment. Available targets and behavior may vary by platform.

## Demonstration

### Terminal Interface

![Nero terminal demonstration](https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084)

![Nero terminal screenshot](https://github.com/user-attachments/assets/60ef735b-c908-48c2-a8c1-99245a935e03)

### Video Demonstration

[![Watch Nero demonstration](https://img.youtube.com/vi/2HGtMkmqwpU/hqdefault.jpg)](https://www.youtube.com/watch?v=2HGtMkmqwpU)

[Watch the demonstration on YouTube](https://www.youtube.com/watch?v=2HGtMkmqwpU)

---

## Security & Responsible Use

Nero is provided strictly for educational, experimental, and defensive security research.

All use must comply with the following principles:

- Obtain explicit authorization before testing any system.
- Conduct network experiments only in approved, isolated environments.
- Do not target public infrastructure or third-party services without permission.
- Review platform restrictions and build configurations before execution.
- Comply with applicable laws, policies, and organizational requirements.

The maintainers do not endorse unauthorized testing, service disruption, or malicious use.

## Project Status

**Experimental / Research & Development**

Nero is an evolving project. Interfaces, internal architecture, platform compatibility, and available capabilities may change during development.

Production deployment is not recommended.

---

<p align="center">
  <strong>NERO</strong>
</p>

<p align="center">
  Written in C · Systems Programming · Authorized Security Research
</p>

<p align="center">
  <a href="https://github.com/thekiran/nero">GitHub Repository</a>
  ·
  <a href="https://github.com/thekiran/nero/blob/main/docs/platform-compatibility.md">Documentation</a>
</p>
