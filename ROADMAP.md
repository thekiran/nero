# NERO roadmap

Reliability, reviewed releases and reproducible developer workflows take priority
over new features. A milestone name is a target, not a promise of stable support.
Track work in the linked issues and keep each change in a focused pull request.

| Target | Work | Acceptance | Tracking |
| --- | --- | --- | --- |
| v0.1.3 — Stability | Windows handle width and exact console cleanup; release serialization; support docs | New boundary/console/loopback regressions and all five package checks pass; review, merge, then inspect a draft | [#9](https://github.com/thekiran/nero/issues/9), [PR #8](https://github.com/thekiran/nero/pull/8) |
| v0.2.0 — Quality | Parser boundaries, deterministic terminal tests, static analysis, isolated TCP and install/uninstall coverage | Failures block merging; test scope and limitations are explicit; dependency updates and contribution templates maintained | [#10](https://github.com/thekiran/nero/issues/10) |
| v0.3.0 — Developer experience | Command/platform API boundaries; install/upgrade/uninstall contract; developer docs | Small documented interfaces and reproducible clean-environment setup | [#11](https://github.com/thekiran/nero/issues/11) |
| v1.0.0 — Stable release | Support policy and release acceptance | Core CLI, packaging, installation and critical failures verified on every claimed platform; no unresolved critical defects | [#11](https://github.com/thekiran/nero/issues/11) |

## Distribution prerequisites

Developer ID/notarization and Authenticode require owner-controlled signing
credentials. Debian packages and Homebrew integration follow a reviewed public
release and reproducible package tests. Do not publish unsigned packages as
signed, invent package-manager availability or commit signing credentials.

## Working agreement

Use an issue to describe the problem and observable acceptance criteria. Implement
on a branch, include targeted regression evidence, and request review in a PR.
The required Release checks gate must cover every supported package. Review draft
assets and notes before publishing. Green CI is evidence, not a security guarantee.

Known follow-up: the POSIX SIGHUP cleanup test has intermittently timed out on
hosted runners. Track and diagnose it in #10; retries are not a substitute for a
deterministic regression and a documented root cause.
