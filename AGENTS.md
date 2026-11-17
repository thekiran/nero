# NERO engineering instructions

## Mandatory universal feature parity

NERO has one C11 operation implementation and three required presentation adapters:
`nero`, `neroconsole`, and `nero-gui`. Every existing and future shared capability
must be exposed through all three with equivalent validation, authorization,
capability/error semantics, cancellation and resource cleanup.

This invariant applies to the entire project: discovery, version, history, logs,
diagnostics, networking, TCP, wireless, PCAP, export, sessions and restricted lab
operations. Frontends must not implement networking or independent packet parsers.
Use direct C APIs from C++ wrappers; do not use shell strings or ANSI output as a
GUI protocol. Pure presentation actions (clear/exit) may remain frontend-local.

Preserve all existing commands and aliases. Audit actual dispatch and parser
behavior before changing syntax. Use one authoritative operation catalog for
discovery/help and alias resolution. Do not add speculative operations as supported.

Read docs/engineering/feature-parity.md and docs/engineering/unified-operations.md
before significant changes. Treat the matrix as evidence, not permission to claim
unverified support. Inspect the current checkout: the baseline audit may be stale.

## Definition of done

Require core/API/registration, CLI, Console, GUI, capability metadata, consistent
errors, contract tests and documentation for every shared feature. Compare real
adapter behavior and normalized results, not labels or source filenames.
Record unsupported platform/build/adapter/dependency/permission states accurately.
Do not fabricate missing metrics, packets, clients or successful capture.

Update the parity matrix with exact test evidence. Mark incomplete work PARTIAL,
PLANNED or BLOCKED and expose accurate disabled/unavailable states.
No feature is globally complete/released until applicable three-frontend checks pass.
Preserve a non-Qt build; run a separate Qt test job where Qt is installed.

## Safety and storage

Do not expand legacy packet generation, traffic intensity or privileges. Preserve
existing controls. New frontend access to restricted laboratory execution remains
blocked until equivalent authorization, preflight, explicit confirmation and
existing safety checks are verified. Preflight itself performs no network I/O.
Automated tests must never run external scans or experimental traffic; use mocks,
offline capture fixtures and explicitly loopback-only TCP fixtures.

Keep action history scope explicit; never clear another frontend's unrelated data.
Use shared log records/redaction. Sessions are in-process unless cross-process
ownership is explicitly implemented; do not add a daemon without demonstrated need.
Follow existing parser limits, private-file protections and cleanup contracts.

## Review and workflow

Follow ROADMAP.md, docs/developer-contracts.md and existing targeted checks.
Work in a focused branch/PR. Report unavailable toolchains and unrun tests honestly.
Before significant feature completion, run the read-only feature-parity-reviewer
defined in .codex/agents/feature-parity-reviewer.toml. Give it exact source/test
evidence. If it cannot run, report review BLOCKED; never substitute a UI screenshot.

Use .agents/skills/nero-feature-parity/SKILL.md for parity implementation/audits.
When creating or extending nero-architecture, nero-core, nero-qt-desktop,
nero-wireless, nero-pcap, nero-validation, nero-security or nero-release skills,
include this invariant and require the same parity review. Those domain skills
were absent at the recorded baseline; do not claim they already exist.
