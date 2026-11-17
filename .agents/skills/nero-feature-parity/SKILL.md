---
name: nero-feature-parity
description: Audit and implement NERO shared-operation parity across nero, neroconsole and nero-gui. Use for new features, command or alias changes, core API changes, platform capability changes, adapter work and release-readiness reviews.
---

# NERO feature parity

Read repository AGENTS.md, docs/engineering/feature-parity.md and
docs/engineering/unified-operations.md. Inspect current source before relying on
the recorded baseline. Follow the existing reliability roadmap.

1. Inventory affected operations from dispatch, parser, catalogs, public APIs,
   build targets and documentation. Include canonical commands, legacy aliases,
   options and actual unsupported behavior. Do not infer removal from short help.
2. Trace each path from the CLI, Console and Qt controller into the same C11
   operation. Identify terminal I/O, Qt dependencies, networking, parsing,
   serialization or safety policy incorrectly located across that boundary.
3. Reuse scan/Wi-Fi session APIs. Keep operation metadata separate from execution.
   Preserve process-local session ownership, explicit lifetimes, cancellation,
   terminal states, snapshot provenance and cleanup.
4. Verify validation, runtime capability reason, permission, confirmation and
   error behavior through all adapters. Separate active scan, passive capture,
   radio changes and offline analysis. Never invent missing platform data.
5. Preserve commands/aliases and interactive nero startup. Keep clear/exit local
   where appropriate. Keep history scopes explicit and logs/redaction shared.
6. Exercise deterministic core and three-adapter contract fixtures. Compare IDs,
   target/options, normalized results, capability reasons, failures, cancellation
   and cleanup. Use offline packets and loopback-only TCP; never external scans
   or experimental transmission. Label real-hardware evidence separately.
7. Update the feature matrix and user documentation with exact paths, tests,
   platform/build limitations and outcomes. Missing adapters, unrun tests or
   a missing runner prevent IMPLEMENTED status.
8. Before claiming significant completion, request the read-only parity reviewer.
   Provide source and test artifacts; resolve findings or report BLOCKED/PARTIAL.

Keep new laboratory execution adapters blocked until equivalent existing
authorization, preflight, explicit confirmation and safety checks are proven.
Never expand packet generation or privileges for parity.

For each violation report: operation/alias, core path, CLI path, Console path,
GUI path, affected platform, missing contract/test, exact recommended fix and
verification still required. Do not approve based on visible buttons or screenshots.
If a path does not exist, name it as proposed, not observed.

Review mode is read-only: report changes for the implementation owner. In
implementation mode update docs alongside the implementation, preserving accurate
partial status. A docs-only milestone does not implement an operation engine.
