# Nero

Nero is an experimental C-based project for authorized network testing,
terminal development, and platform compatibility research.

> **Use only in isolated laboratory environments or on systems for which you
> have explicit authorization.**
>
> Nero is not intended for public, production, or unauthorized use. The
> maintainers do not endorse disruption of services or testing third-party
> systems without permission.

## Project scope

Nero provides a controlled environment for:

- Evaluating network resilience in authorized test environments
- Exercising terminal input, history, ANSI rendering, and PTY behavior
- Developing and validating platform-specific C code
- Testing defensive tooling such as firewalls and IDS/IPS solutions

The network traffic components are intentionally restricted by platform and
build configuration. Never run network-related functionality outside an
approved test environment.

## macOS development and safe validation

On macOS, `make` builds the development CLI with the system C compiler and
SDK. No Homebrew paths or third-party libraries are required. Use
`make CC=clang` to select Clang explicitly.

```sh
make
make test
make test-sanitize
make analyze
```

The macOS build supports the terminal interface, ANSI colors, ASCII graphics,
and in-memory command history. Its interface test harness enumerates local
interfaces without probing the network. The `-Syn` operation is unavailable
in the macOS executable; the Linux load engine and port scanner are not linked
into that target.

On Linux, the existing source list and library dependencies are retained.
Offline tests use a no-network adapter and do not execute the Linux load
engine. Python 3 is required for tests, and Clang is required for analysis.

See [platform analysis and validation](docs/platform-compatibility.md) for
supported platforms, validation scope, known risks, and untested behavior.

## Installation

The following legacy Linux installation commands are retained for development
reference. Review the build configuration before installing any executable.

```bash
git clone https://github.com/thekiran/nero.git
cd nero
sudo apt update
sudo apt install libcurl4-openssl-dev libcjson-dev libldns-dev libncurses-dev
make
make install
sudo nero
```

## Media

![Nero terminal demonstration](https://github.com/user-attachments/assets/270e73c3-a231-474a-a812-7a5dba635084)

<div align="center">
  <img src="https://github.com/user-attachments/assets/60ef735b-c908-48c2-a8c1-99245a935e03" width="731" height="504" alt="Nero terminal demonstration" />
</div>

<div align="center">
  <a href="https://www.youtube.com/watch?v=2HGtMkmqwpU">
    <img src="https://img.youtube.com/vi/2HGtMkmqwpU/0.jpg" width="731" height="504" alt="Nero project demonstration video" />
  </a>
</div>

## Legal and safety notice

This project is provided for authorized education, research, and defensive
testing only. You are responsible for complying with applicable laws,
organizational policies, and written authorization requirements.
