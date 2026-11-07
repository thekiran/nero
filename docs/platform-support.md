# Platform support and validation

NERO is experimental. A successful build does not certify every network feature.
The native package workflow covers the following combinations:

| Capability | macOS ARM64 / Intel | Linux x86-64 / ARM64 | Windows x86-64 |
| --- | --- | --- | --- |
| Native CLI / history / rendering | Make | Make | CMake + MinGW UCRT64 |
| Interface enumeration | getifaddrs + fixtures | sysfs + offline tests | GetAdaptersAddresses + selection tests |
| Offline CLI tests | Make + CMake | Make + CMake | Native pipe/EOF/input tests |
| Terminal tests | POSIX PTY | POSIX PTY | Synthetic and real Win32 console mode restoration |
| ASan / UBSan | Offline targets | Offline targets | Not claimed for MinGW |
| Socket adapter tests | POSIX path retained | POSIX path retained | Synthetic 64-bit handles and one bounded loopback TCP exchange |
| Raw packet engine | Included in Make build; not exercised by CI | Included in Make build; not exercised by CI | Explicitly unavailable |
| System command installation | `/usr/local/bin/nero` | `/usr/local/bin/nero` | User or machine PATH |
| MSVC foundation report | N/A | N/A | Separate foundation-only preset |

Windows ARM64 has no native release package. WSL2 follows the matching Linux
architecture. macOS packages target macOS 15+; Linux packages target Ubuntu 24.04
compatible runtime libraries. See [installation and release scope](releases.md).

## Evidence

The v0.1.2 package workflow passed on all five platforms:
[run 42378472442](https://github.com/thekiran/nero/actions/runs/42378472442).
That run predates the v0.1.3 socket/console boundary regressions. Consult the
stability pull request and its checks for evidence of those fixes; the presence
of a test or workflow alone is not a passing result.

## Reproduce

On macOS/Linux, run `make test`, `make test-sanitize` and the host CMake test
preset. Windows uses the MinGW commands in [building.md](building.md).
Windows native tests are additional suites; POSIX PTY tests are not silently
skipped to claim equivalent Windows terminal coverage.

Tests never run the raw packet engine or contact third-party targets. The new
Windows loopback fixture opens one ephemeral TCP listener bound to 127.0.0.1,
exchanges one byte locally, and closes its sockets. Synthetic tests exercise
handle values above INT_MAX without opening sockets. Full scanner and network
engine runtime behavior remains outside this validation claim.
