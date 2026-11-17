# Unified operations: migration contract

Status: **design, not implemented**. The [baseline matrix](feature-parity.md)
records source evidence and missing adapters. This document does not introduce
a runtime registry or claim that proposed APIs exist.

## Smallest useful boundary

Retain the C11 core and existing public scan/Wi-Fi APIs. Introduce a public
operation descriptor/request/result interface only when the first vertical
slice can be compiled and exercised through all adapters. Start with discovery,
version and capability reporting; do not wrap terminal handlers as a “core.”

Each operation has one stable ID, canonical name, description/category,
typed input/output schemas, validation policy, capability/privilege requirements,
platform/build metadata, cancellation/progress support, mutation/confirmation
flags, risk class, aliases and help metadata. Alias patterns resolve to that ID
and options, not another executor. A separate alias contract fixture protects
historical syntax without duplicating execution metadata.

Requests must own or explicitly borrow validated arguments for a documented
lifetime. Copy asynchronous arguments before parser buffers expire. Results must
include operation identity, typed status, capability reason and typed payload;
events include operation/session identity and ordered state/progress snapshots.
Keep shell control flow (continue/exit) separate from operation success/failure.

Shared dispatch order: parse/typed request, validation, capability,
permission/authorization/confirmation, execute, structured result/events.
Adapters may validate forms early, but shared validation is authoritative.
No terminal reads, printf protocol, Qt objects or shell execution in dispatch.
Use explicit service context for process-local policy/session state.

## Adapters

| Adapter | Responsibility |
| --- | --- |
| nero | Existing interactive startup; typed command parsing, compatibility aliases, rendering, accurate direct-mode exit codes and signal-to-cancel handoff |
| neroconsole | Reuse terminal input/rendering; navigation, progress, contextual discovery and session views through the same requests |
| nero-gui | Qt/C++ ownership wrappers, background execution, queued model updates, forms, errors, cancellation and export through the C API |

Clear/exit remain local presentation actions, with consistent semantics.
Do not create a no-op core executor just to make a checkbox pass.
History needs explicit scope (CLI/Console command history versus GUI action
history); preserve existing allowlist/privacy policy. Clear operates on an
explicit scope. Logs share records/redaction; current cross-process rotation
limitations remain documented until separately solved.

## Capability and errors

Use these stable capability reasons: SUPPORTED, UNSUPPORTED_PLATFORM,
UNSUPPORTED_ADAPTER, MISSING_PERMISSION, MISSING_DEPENDENCY, NOT_INITIALIZED,
UNAVAILABLE_BUILD. A capability probe does not prove operation success.
Keep execution errors separate: validation, authorization/confirmation denial,
I/O failure, allocation failure and cancellation must not become SUPPORTED.

No-frame delivery is an observation, not automatically proof of an unsupported
adapter. Preserve monitor requested/set, actual link type, frame counts and
OS-scan provenance. Redacted identifiers and missing signal metrics need explicit
validity flags. All frontends render the same facts and reasons.

A missing UI implementation is a development gap, not UNSUPPORTED_PLATFORM.
A build intentionally omitting an operation can report UNAVAILABLE_BUILD, but
must not disguise unfinished parity as a platform restriction.

## Lifecycle and ownership

Reuse nero_scan_session and nero_wifi_session rather than replacing them.
Map their actual semantics into CREATED, READY, RUNNING, CANCELLING,
COMPLETED, CANCELLED or FAILED only where meaningful. Cancellation requests do
not imply completed cancellation; publish terminal state after workers/resources
finish. Preserve partial scan coverage and unknown/error classifications.

Snapshot ownership, callback threading and destruction must be explicit.
No callbacks after destruction; GUI workers must stop/join before core ownership
is released. Separate processes have separate contexts, sessions and IDs unless
an explicit future IPC design says otherwise. No daemon is needed for parity.

## Migration and acceptance

1. Freeze canonical/legacy characterization fixtures from actual source.
2. Implement a minimal registry and typed result/capability contracts. Generate
   discovery/help from it, and migrate the first complete three-adapter slice.
3. Move logs/doctor/history and network state beneath adapters where needed.
4. Reuse the TCP session; extract the selected-port recheck from commands.c.
5. Migrate Wi-Fi session orchestration, radio policy, scan assist and CSV export.
   Retain strict --no-scan behavior and explicit radio-mutation consent.
6. Extend shared PCAP APIs before adding retained packet list/detail/bytes.
7. Review restricted legacy access last; keep unavailable controls truthful.
   No packet-engine changes, privilege expansion or unsafe direct-mode shortcut.
8. Run parity review and update evidence after each slice.

The registry-to-adapter gate must enumerate actual registry entries and exercise
CLI parser resolution, Console action binding and Qt controller binding.
Require normalized identity/options/results, equivalent failures and capability
reasons, cancellation/cleanup, and alias characterization. Include malformed
input, missing permissions/dependencies and absent initialization. Test removal
of a real binding to prove the gate rejects an omission.

Keep non-GUI CMake builds valid without Qt; add a separate Qt-enabled job.
Generate any manifest/docs export from registry metadata; do not maintain another
independent executable catalog. Maintain explicit baseline debt during migration,
but prevent new unverified features from receiving global release status.
A passing existing CI run is not a parity gate.

## Review infrastructure

The project-scoped reviewer file follows the documented custom-agent
name/description/developer_instructions schema. Model settings inherit from the
parent. Its instructions require read-only work. Do not claim that this TOML
alone enforces an OS sandbox: configure a read-only parent session/tool surface
when an enforced boundary is required. The current official custom-agent
documentation says sandbox/approval fields in an agent file do not override
the parent session.

Reference: https://learn.chatgpt.com/docs/agent-configuration/subagents

The reviewer configuration and repository skill still need loading/validation
in an execution-capable Codex checkout. Eight other requested NERO skills were
absent at baseline; AGENTS.md requires their eventual creation/extension to
include the invariant. No personal/global skill installation is implied.
