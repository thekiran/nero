# NERO Documentation

Welcome to the NERO technical documentation library. This directory serves as the canonical source of truth for the project's architecture, development toolchains, platform support, command catalog, testing methodologies, release engineering, and operational diagnostics.

A synchronized GitHub Wiki is maintained at **[github.com/thekiran/nero/wiki](https://github.com/thekiran/nero/wiki)** using automated continuous integration.

---

## Primary Guides

| Guide | Description |
| --- | --- |
| **[Installation](installation.md)** | Package downloads, checksum verification, native macOS/Linux, native Windows MinGW, and WSL2 setup |
| **[Command Reference](command-reference.md)** | Exhaustive reference of interactive terminal commands, diagnostics, syntax, and permission boundaries |
| **[Platform Support & Compatibility](platform-support.md)** | Operating system compatibility matrix, verified capabilities, test evidence, and platform boundaries |
| **[Development & Build Guide](building.md)** | Make and CMake build commands, platform presets, sanitizers, toolchain setup, and code formatting |
| **[Testing & Quality Assurance](testing.md)** | Testing philosophy, offline-safe regressions, PTY tests, Win32 fixtures, sanitizers, and mock architecture |
| **[Troubleshooting & Diagnostics](troubleshooting.md)** | Verified solutions for Windows shell/PATH issues, Linux missing libraries, macOS quarantine, and raw mode recovery |
| **[Architecture & Design Contracts](architecture.md)** | Component boundaries, CLI responsibilities, header contracts, and build modularity |
| **[Code Map](code-map.md)** | Source file ownership and guide to finding specific implementations |
| **[Release Engineering](releases.md)** | Packaging procedures, checksum sidecars, installer verification, and draft release workflows |
| **[Repository & Merge Policies](repository-rules.md)** | Main branch protection, required CI quality gates, and code review rules |
| **[Laboratory Safety Guidelines](laboratory-safety.md)** | Operational protocols, ethical constraints, and isolated environment requirements |

---

## Project Policies and Governance

* **[Contributing Guidelines](../CONTRIBUTING.md):** Contributor workflow, commit expectations, and validation rules.
* **[Project Roadmap](../ROADMAP.md):** Target milestones, feature scope, and distribution prerequisites.
* **[Security Disclosure Policy](../SECURITY.md):** Responsible reporting guidelines for experimental modules.
* **[Changelog](../CHANGELOG.md):** Chronological log of released versions and documented changes.
* **[Code of Conduct](../CODE_OF_CONDUCT.md):** Community standards and behavioral expectations.

---

## Historical Reviews and Audit Archives

These historical documents record findings, benchmarks, and validation logs produced during earlier development phases:

* **[Code Audit (Historical)](code-audit.md):** Baseline code audit report.
* **[Fix Report (Historical)](fix-report.md):** Action report from initial audit resolution.
* **[Platform Compatibility Review (Historical)](platform-compatibility.md):** Historical compatibility analysis.
* **[Terminal Quality Review (Historical)](terminal-quality.md):** Terminal session stability and signal investigation.
* **[Test Results (Historical)](test-results.md):** Offline test validation records.
* **[Linux Runtime Validation (Historical)](linux-runtime-validation.md):** Linux execution logs.
* **[Local Validation Logs (Historical)](local-validation.md):** Local environment verification records.
* **[Real Network Validation (Historical)](real-network-validation.md):** Authorized laboratory validation records.
