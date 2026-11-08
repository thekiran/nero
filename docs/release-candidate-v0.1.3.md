# v0.1.3 release verification

[v0.1.3](https://github.com/thekiran/nero/releases/tag/v0.1.3) was published on
2026-11-08 from `f9904830d2caf3d57df16f36e876ebe56d2c0feb`.
The evidence below comes from [Release packages #58](https://github.com/thekiran/nero/actions/runs/42478613983)
and inspection of the published release. Do not rerun this version to replace
published assets or move its tag.

## Verified

- [x] CLI version stamping PR #28 and documentation corrections PR #36 merged.
- [x] New v0.1.3 tag created at the intended commit after package checks passed.
- [x] macOS ARM64 and x86-64 Make, CMake, PTY and sanitizer jobs passed.
- [x] Linux ARM64 and x86-64 Make, CMake, PTY and sanitizer jobs passed.
- [x] Windows x86-64 native CLI, socket, console and installer job passed.
- [x] All five archives, five `.sha256` sidecars and `SHA256SUMS` are present.
- [x] Individual and aggregate SHA-256 checks passed in the final release job.
- [x] Package jobs verified build metadata, executable contents and release version.
- [x] Release notes reviewed with unsigned/not-notarized package limitations stated.
- [x] Release published and shown as Latest with the expected commit and assets.

## Independent follow-up

- [ ] Review [stability tracker #9](https://github.com/thekiran/nero/issues/9)
      against its acceptance evidence before closing it.
- [ ] Perform additional installation checks on fresh end-user machines.
      CI packaging and installer checks are recorded above; separate manual
      clean-machine validation is not claimed.

## Rollback

Do not repoint a published tag or overwrite published assets. Fix discovered
issues in a PR and prepare a new version. Follow the
[installation guide](installation.md) for downloads and checksum verification.

CI verifies offline fixtures, bounded local fixtures and packaging. It does
not certify the experimental raw-packet engine for uncontrolled networks.
