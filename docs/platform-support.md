# Platform support and validation

Experimental, authorized laboratory development only. A configured target is not
a verified platform. Directory and CLI organization changes preserve existing behavior.

| Feature | Linux x86_64 / ARM64 | macOS ARM64 | Windows x64 |
| --- | --- | --- | --- |
| Existing Make path | Preserved; local runtime NOT TESTED | PASS | Unsupported |
| Existing CLI / history / colors | Preserved; NOT TESTED locally | PASS, existing safe tests | Unsupported |
| Local interface listing | Existing sysfs; NOT TESTED locally | PASS, getifaddrs + fixtures | Unsupported |
| New platform report | Configured, NOT TESTED | Direct Clang build and smoke PASS | Configured, NOT TESTED |
| CMake configure/build | NOT TESTED | PASS: configure, build and 4 CTest groups | NOT TESTED |
| CMake traffic generation / scanning | Unsupported | Unsupported | Unsupported |
| Legacy Linux engine | Behavior preserved; intentionally not executed | Excluded | Excluded |
| Win32 console / Winsock | N/A | N/A | Not implemented; no support claim |

## Reproduction

With CMake 3.21+ and Python 3 installed, select `macos`, `linux` or `windows`:

```sh
cmake --preset macos
cmake --build --preset macos
ctest --preset macos
```

POSIX sanitizer presets: `macos-sanitize`, `linux-sanitize`.
Windows static-analysis preset: `windows-analysis` (/analyze).
Windows tests cover only the foundation report, source integrity and unavailable
network adapter. They are not Windows CLI smoke tests. No POSIX PTY tests are
silently skipped to claim Windows CLI compatibility; that executable is absent.

CMake omits unused bundled cJSON from development targets, matching the established
CLI test scope. The legacy Make build still includes it unchanged. cJSON sanitizer
validation remains incomplete from the previous phase. Existing linker alignment
warnings, scanner/header issues, signal restoration limits and engine risks are
listed in platform-compatibility.md and were not introduced here.

## Local phase results

- Baseline and final Make/CLI regression, ASan/UBSan and Clang analysis: PASS on macOS.
- New platform report and unavailable adapter: direct Clang build, runtime checks,
  ASan/UBSan and Clang analysis performed locally; results recorded in task report.
- New integration suite: platform/architecture report and all original C/header/
  Makefile hashes checked.
- CMake macOS configure, build and all four CTest groups passed using an isolated
  CMake installation. It is a development tool, not a runtime dependency.
- Windows/MSVC, Linux/GCC runtime and new remote CI: NOT TESTED.

CI adds ubuntu-latest, ubuntu-24.04-arm, macos-latest and windows-latest. The existing
compatibility workflow is retained during migration. Runners may change hardware;
actual architecture is checked against the platform report in tests. No push or
workflow dispatch was performed for this phase. Full acceptance remains pending.
