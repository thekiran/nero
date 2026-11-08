# NERO roadmap

Reliability, reviewed releases and reproducible developer workflows take priority
over new features. A milestone name is a target, not a promise of stable support.
Track work in the linked issues and keep each change in a focused pull request.

[v0.1.3 was published on 2026-11-08](https://github.com/thekiran/nero/releases/tag/v0.1.3)
from `f990483`. All five package jobs and aggregate checksums passed in
[release run #58](https://github.com/thekiran/nero/actions/runs/42478613983).
[Issue #9](https://github.com/thekiran/nero/issues/9) remains available for acceptance follow-up.

| Target | Work | Acceptance | Tracking |
| --- | --- | --- | --- |
| v0.1.3 — Published | Windows handle width, console cleanup, release serialization and support docs merged in PR #8 | Five native packages and SHA-256 checks passed; notes reviewed and release published | [#9](https://github.com/thekiran/nero/issues/9), [PR #8 (merged)](https://github.com/thekiran/nero/pull/8) |
| v0.2.0 — Quality | Parser boundaries, deterministic terminal tests, static analysis, isolated TCP and install/uninstall coverage | Failures block merging; test scope and limitations are explicit; dependency updates and contribution templates maintained | [#10](https://github.com/thekiran/nero/issues/10) |
| v0.3.0 — Developer experience | Command/platform API boundaries; install/upgrade/uninstall contract; developer docs | Small documented interfaces and reproducible clean-environment setup | [#11](https://github.com/thekiran/nero/issues/11) |
| v1.0.0 — Stable release | Support policy and release acceptance | Core CLI, packaging, installation and critical failures verified on every claimed platform; no unresolved critical defects | [#11](https://github.com/thekiran/nero/issues/11) |

The [v0.1.3 verification record](docs/release-candidate-v0.1.3.md) separates CI evidence from independent checks that have not been claimed.

## Distribution prerequisites

Developer ID/notarization and Authenticode require owner-controlled signing
credentials. Debian packages and Homebrew integration follow a reviewed public
release and reproducible package tests. Do not publish unsigned packages as
signed, invent package-manager availability or commit signing credentials.

Branch protection is defined in [repository-rules.md](docs/repository-rules.md);
its configuration file must be applied by an administrator before it is enforced.

## Working agreement

Use an issue to describe the problem and observable acceptance criteria. Implement
on a branch, include targeted regression evidence, and request review in a PR.
The required Release checks gate must cover every supported package. Review draft
assets and notes before publishing. Green CI is evidence, not a security guarantee.

The POSIX SIGHUP flag-check/wait race was fixed in PR #14 with atomic
`pselect` signal-mask handoff. The deterministic before-wait regression remains
mandatory; a repeated validation run passed 150 real-signal cleanup cases and
50 injected races. Track any new failures in #10 with logs rather than retries.

Source builds after v0.1.3 add private command-history persistence, exact
64-token dispatch, parser capacity regressions and installer lifecycle checks.
[Distribution paths](docs/distribution.md) documents the macOS Homebrew formula
and test-only Ubuntu `.deb` artifacts. Signing/notarization still requires
owner-controlled credentials and is not claimed by these checks.
