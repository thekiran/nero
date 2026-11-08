# Changelog

All listed versions are drafts until published on the
[Releases page](https://github.com/thekiran/nero/releases). Dates below identify
preparation, not stable or security-certified releases.

## Unreleased

Implemented on `main` through merged [PR #8](https://github.com/thekiran/nero/pull/8); **not yet included in a publicly published release**. The following changes are planned for the v0.1.3 release:

- Preserve pointer-sized Windows SOCKET handles through socket and poll adapters.
- Restore original input/output console modes and output code page.
- Add synthetic handle/console regressions, a real console fixture and a bounded
  localhost TCP test.
- Serialize manual/tag workflows for the same version and expose Release checks.
- Correct stale Windows support documentation and clarify audit/test boundaries.
- Make the scrollbar division invariant explicit for static analysis.

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
