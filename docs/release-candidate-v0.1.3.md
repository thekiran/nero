# v0.1.3 release readiness

This is a **release candidate checklist**, not a declaration that v0.1.3 has
been built, tested, signed, or published. No release is created by this document.

## Prerequisites

- [x] [CLI version stamping](https://github.com/thekiran/nero/pull/28) merged
      in commit `e135949`; verify package version output again for this candidate.
- [ ] Review the open [stability tracker](https://github.com/thekiran/nero/issues/9)
      against its regression evidence, keeping unfinished work open.
- [ ] Confirm the intended `main` commit and update `CHANGELOG.md` with
      user-visible changes and known limitations.
- [ ] Confirm the version `v0.1.3` is new and its tag has not been published or moved.

## Build and verify

Run the **Release packages** workflow on the intended `main` commit with
`version=v0.1.3`. It must create a draft, not publish automatically.

- [ ] macOS ARM64 and x86-64 archives pass Make, CMake, PTY and sanitizer checks.
- [ ] Linux ARM64 and x86-64 archives pass Make, CMake, PTY and sanitizer checks.
- [ ] Windows x86-64 archive passes MinGW native CLI, socket, console and installer tests.
- [ ] All five archives, five per-asset `.sha256` files and `SHA256SUMS` are present.
- [ ] `sha256sum -c SHA256SUMS` passes against the downloaded archives.
- [ ] `BUILD-INFO.json` has the expected commit, platform and binary digest.
- [ ] Installation from every artifact works in a clean, disposable environment.
      `version` displays `version v0.1.3` from the installed executable.
- [ ] Review whether unsigned macOS/Windows packages need explicit user-facing
      warnings; do not claim notarization or Authenticode signing.

## Publication and rollback

Only publish the verified draft after explicit maintainer review.
Do not repoint an existing published version tag and do not overwrite published
assets. If any gate fails, leave the draft unpublished, fix the issue in a PR,
and prepare a new candidate. After publication, verify public download links
and follow the [installation guide](installation.md).

CI verifies offline fixtures and packaging. It does **not** certify the
experimental raw-packet engine for uncontrolled networks.
