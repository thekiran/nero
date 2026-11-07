# Testing and Quality Assurance

[Documentation Index](README.md) · [Development Guide](building.md) · [Architecture](architecture.md) · [Platform Support](platform-support.md)

NERO maintains a deterministic, automated test infrastructure. All automated CI and pull request gates operate strictly in **offline-safe** mode: tests never transmit unsolicited packets, never contact external endpoints, and execute within predictable resource bounds.

---

## Testing Principles

1. **Deterministic Execution:** Tests avoid network timing dependencies, external DNS lookups, or unmocked remote endpoints.
2. **Offline Safety:** CI and test executables use `NERO_SAFE_TEST` or `NERO_NETWORK_UNAVAILABLE`. The raw packet engine is excluded from automated test binaries.
3. **Exact Terminal Restoration:** Terminal quality tests verify that every session cleans up signal masks, restoring raw mode flags to original `termios` (POSIX) or console input/output modes and code page (Windows).
4. **Platform Isolation:** Platform-specific features (e.g., POSIX PTY sessions vs. Win32 console handles) use native test fixtures rather than silent skips.

---

## Test Suites Overview

| Suite | Runner / Fixture | Scope | Platforms |
| --- | --- | --- | --- |
| **Core Safe Regressions** | `tests/test_safe.py` | Command line options, input validation, offline mock CLI | macOS, Linux |
| **Terminal Quality & PTY** | `tests/test_terminal_quality.py` | PTY allocation, ANSI rendering, signal handling (SIGINT, SIGHUP, SIGTERM), EOF | macOS, Linux |
| **Operation Layout** | `tests/test_operation_layout.py` | Viewport dimensions, line wrapping, logo rendering, `NO_COLOR` support | macOS, Linux |
| **Platform Foundation** | `tests/integration/test_foundation.py` | Platform identification, CPU, OS release, compiler version | macOS, Linux, Windows |
| **Windows Native CLI** | `tests/windows/test_cli.py` | Interactive pipe, EOF, input boundaries, invalid input recovery | Windows (MinGW) |
| **Win32 Boundaries** | `tests/windows/compat_boundaries.c` | Pointer-sized `SOCKET` handles (`> INT_MAX`), `fd_set`, polling boundary | Windows (MinGW) |
| **Win32 Console Restore** | `tests/windows/console_restore.c` | Exact `GetConsoleMode` / `SetConsoleMode` and code page restoration | Windows (MinGW) |
| **Win32 Socket Loopback** | `tests/windows/socket_loopback.c` | Single-byte exchange on `127.0.0.1` ephemeral port using production wrappers | Windows (MinGW) |
| **Memory Sanitizers** | ASan (`-fsanitize=address`) & UBSan | Out-of-bounds reads/writes, use-after-free, undefined integer behavior | macOS, Linux |
| **Static Analysis** | Clang Static Analyzer | Dead store, memory leak, null-dereference analysis | macOS, Linux |

---

## Running Tests Locally

### On macOS

Using Make (Xcode Command Line Tools):

```sh
# Run core regressions, terminal quality, and operation layout
make test

# Run offline test suite with AddressSanitizer and UndefinedBehaviorSanitizer
make test-sanitize

# Run Clang static analysis
make analyze
```

Using CMake presets:

```sh
cmake --preset macos
cmake --build --preset macos
ctest --preset macos --output-on-failure

# Sanitizer preset
cmake --preset macos-sanitize
cmake --build --preset macos-sanitize
ctest --preset macos-sanitize --output-on-failure
```

### On Linux (Ubuntu 24.04 / Debian)

Ensure required dependencies are installed:

```sh
sudo apt update && sudo apt install -y build-essential cmake python3 clang \
    libcurl4-openssl-dev libcjson-dev libldns-dev libncurses-dev
```

Run test targets:

```sh
# Authoritative Makefile path
make test CC=gcc
make test-sanitize CC=gcc

# CMake Linux preset
cmake --preset linux
cmake --build --preset linux
ctest --preset linux --output-on-failure
```

### On Windows (Native x86-64 via MSYS2 UCRT64)

From an MSYS2 UCRT64 shell or configured PowerShell terminal:

```powershell
# Configure with Ninja and GCC
cmake -S . -B .build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DBUILD_TESTING=ON

# Build all test targets
cmake --build .build/windows --parallel 4

# Execute CTest suite
ctest --test-dir .build/windows --output-on-failure --no-tests=error
```

Output:
```text
Test project C:/.../nero/.build/windows
    Start 1: foundation
1/5 Test #1: foundation .......................   Passed    0.15 sec
    Start 2: windows-native-cli
2/5 Test #2: windows-native-cli ...............   Passed    0.15 sec
    Start 3: windows-compat_boundaries
3/5 Test #3: windows-compat_boundaries ........   Passed    0.06 sec
    Start 4: windows-console_restore
4/5 Test #4: windows-console_restore ..........   Passed    0.04 sec
    Start 5: windows-socket_loopback
5/5 Test #5: windows-socket_loopback ..........   Passed    0.05 sec

100% tests passed out of 5
```

---

## Mock and Fault Injection Architecture

Automated tests inject deterministic conditions using test-specific wrappers located in `tests/mocks/`:

* **`tests/mocks/terminal_faults.c`:** Mocks terminal window dimensions and simulates window resize (`SIGWINCH`) events to verify dynamic reflow without crashing.
* **`tests/mocks/input_faults.c`:** Injects asynchronous signals (such as `SIGHUP`) directly between the user flag check and the blocking `pselect`/`read` syscall, verifying that atomic signal masking prevents lost wakeups or hung sessions.
* **`tests/mocks/network_handler.c`:** Replaces live network operations with mock responses, ensuring that test binaries never transmit raw packets or access live network hardware.

---

## Continuous Integration Quality Gates

GitHub Actions enforces strict quality gates for every pull request and push to `main`:

* **`ci.yml` (Cross-Platform Foundation):** Runs CMake presets, unit tests, sanitizer builds, and legacy Make paths across Ubuntu x86-64, Ubuntu ARM64, macOS, and Windows.
* **`release.yml` (Package Verification):** Compiles and packages native artifacts for all 5 platform targets, builds release binaries, verifies system installation scripts in isolated staging environments, and tests standalone executables.
* **`static-analysis.yml`:** Checks code formatting (`clang-format`) and static analysis rules.


## TCP scanner reliability

The production scanner is tested with synthetic socket/poll outcomes and real
worker threads, without creating real sockets. See the [reliability report](portscanner-reliability.md)
for coverage, the complete-port invariant and validation limits. `make test`
and CTest include this fixture. `make test-sanitize` runs it under ASan/UBSan;
`make test-tsan` runs the scanner fixture separately under ThreadSanitizer on
supported POSIX toolchains. Windows native MinGW runs the same fixture via CTest.
Optional ICMP discovery is disabled in these tests.


## Application logging

`tests/test_logging.py` verifies the `log` command, warnings/errors filter,
previous-session loading, file path, clearing, memory fallback, permissions,
control-character sanitization and 1 MiB rotation. Its C fixture uses eight
real pthread writers (4,800 records), checks errno preservation, and verifies
that the latest 256 entries remain intact. The scanner fixture also verifies
that grouped socket errors reach the error log. Make/CTest include these checks;
ASan/UBSan cover them and `make test-tsan` additionally stresses the logger.
Tests use temporary paths or `NERO_LOG_FILE=:memory:` to avoid user log files.


Logging recovery regressions additionally cover partial trailing records,
clearing after transient/permanent storage failures, preserving current records
when a backup cannot be cleared, read-only stdout, FIFO/Windows device targets,
and pre-existing oversized current/backup files. The scanner fixture injects
mutex and banner worker initialization failures and verifies their log records.
These cases require no real device or remote network traffic.
