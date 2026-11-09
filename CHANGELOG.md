# Changelog

Release status is stated per version. The older v0.1.0–v0.1.2 entries remain
preparation records for drafts; v0.1.3 is publicly published. No release is
advertised as stable or security-certified.

## Unreleased

- Add the `preflight` read-only `-Syn` preflight screen: it reports, as
  qualified upper bounds, what the experimental engine in the current build
  would attempt (compile-time worker count, raw-socket privilege requirement,
  thread/descriptor ceilings) and clearly flags builds where live raw mode is
  not compiled in. It opens no sockets, sends no packets, starts no subprocess,
  escalates no privilege, and never enters the `-Syn` path; no knob to
  configure worker count, packet rate or packet volume is introduced. Expose
  the engine's fixed worker count as read-only `NERO_SYN_THREAD_COUNT`.
- Add the `sim` offline workload-profile screen (OFFLINE SIMULATION): it reads
  local CPU/RAM with read-only native APIs, shows a deterministic, conservative
  recommendation, and lets the user make the final selection. It opens no
  sockets, sends no packets, and never reaches the legacy `-Syn` engine,
  `THREAD_COUNT`, `portscanner()` or `scan_session.c`; offline binaries link no
  network transmission symbols.
- Fix macOS CPU detection: `hw.logicalcpu`/`hw.physicalcpu` are 32-bit sysctls
  and were being discarded by a `sizeof(long)` length check, so the screen
  reported the processor count as `Unavailable` on working Macs.
- Fix a missing NUL terminator in the simulation menu's line reader that let a
  previous entry's leftover stack bytes corrupt the next parsed value.

## 0.1.4 — native installer release

See [release status and downloads](https://github.com/thekiran/nero/releases)
for publication and platform assets.

- Add native macOS Installer `.pkg` and Windows Installer `.msi` wizards,
  plus Ubuntu-compatible `.deb` packages to the release pipeline. Normal
  installation registers the global command without shell setup commands.
- Add machine PATH and Start menu integration with managed MSI upgrades and
  removal; notify Windows about manual user PATH changes.
- Verify native package installation from unrelated folders, including normal
  and sudo POSIX shells and fresh Windows PowerShell/CMD processes.

- Persist recognized command history with bounded private storage, opt-out,
  explicit failures and cross-session clear behavior. Unknown commands and
  arbitrary arguments are excluded from disk history.
- Accept the documented 64 command tokens; add parser capacity/syntax tests.
- Verify history restarts, malformed files, permissions, links, locking and
  concurrent clearing on native CLI targets; exercise ASan/UBSan on POSIX.
- Add staged installer lifecycle tests, POSIX atomic replacement, and POSIX/
  Windows uninstallers that retain user data.
- Add a macOS Homebrew formula pinned to published v0.1.3 archives and native
  Ubuntu amd64/arm64 Debian package creation with inferred ELF dependencies.
  Distribution CI verifies install/reinstall or upgrade/removal; it does not
  publish an APT repository or claim code signing.

## 0.1.3 — published 2026-11-08

[Download v0.1.3](https://github.com/thekiran/nero/releases/tag/v0.1.3).
Built from `f9904830d2caf3d57df16f36e876ebe56d2c0feb`; all five native package jobs
and aggregate SHA-256 checks passed in [release run #58](https://github.com/thekiran/nero/actions/runs/42478613983).

Includes the stability fixes merged through [PR #8](https://github.com/thekiran/nero/pull/8):

- Preserve pointer-sized Windows SOCKET handles through socket and poll adapters.
- Restore original input/output console modes and output code page.
- Add synthetic handle/console regressions, a real console fixture and a bounded
  localhost TCP test.
- Serialize manual/tag workflows for the same version and expose Release checks.
- Correct stale Windows support documentation and clarify audit/test boundaries.
- Make the scrollbar division invariant explicit for static analysis.

- Add full-range TCP accounting, bounded retries and early port selection with partial coverage.
- Add persistent application logs and scanner error diagnostics; command history remains session-only.
- Stamp the CLI with its verified release tag.
- Correct CLI limits, scanner documentation and Wiki publication error reporting.

Repository quality work already merged: contribution forms, action dependency updates, a roadmap,
community conduct expectations and a shorter README with dedicated installation docs.

## 0.1.2 — draft prepared 2026-11-07

- Install `nero` into a system command directory on macOS/Linux.
- Install native Windows `nero.exe` and update user or machine PATH.
- Include installers in archives and verify execution outside the build tree.
- Verify all five native packages and SHA-256 checksums before preparing the draft.

## 0.1.1 — draft prepared 2026-11-07

- Add a real Windows x86-64 MinGW CLI package and native behavioral tests.
- Check PE architecture, DLL imports and extracted standalone execution.
- Keep the Windows raw packet engine explicitly unavailable.

## 0.1.0 — draft prepared 2026-11-07

- Add native macOS ARM64/Intel and Linux x86-64/ARM64 release builds.
- Gate packaging on offline regressions and POSIX ASan/UBSan tests.
- Produce archives, build metadata, SHA-256 sidecars and an aggregate manifest.

### Scanner session reliability (unreleased)

- Extract the production TCP core into independently owned scan sessions and replace source-inclusion tests with per-session I/O injection.
- Preserve successful writable connections that already have a hangup; count worker failures and fail explicitly on clock errors.
- Add simultaneous-session, stop/cleanup, known-listener and Linux interface-fallback regressions.
- Separate deterministic interface tests from mandatory native Linux enumeration and remove unused Linux link dependencies.
