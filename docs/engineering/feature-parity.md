# NERO feature parity — audited baseline

Audit source: [`513539ac892860f13e32b14c0374f982157b4b19`](https://github.com/thekiran/nero/tree/513539ac892860f13e32b14c0374f982157b4b19).
This is a source inspection, not a runtime certification. No compiler, shell, Qt runtime or test runner was available during this audit. No tests were executed.

## Current release state

**Three-frontend parity: BLOCKED (GUI).** Two of the three frontends now exist; the GUI remains the open prerequisite.

### Progress — 2026-11-17 (neroconsole delivered)

- `neroconsole` is now a **real standalone executable**, not a rename. It links the exact same C11 core, command dispatcher and `terminal_code()` engine as `nero` through a distinct entry point (`src/apps/neroconsole_main.c`), wired into the Makefile (macOS/Linux) and the Windows MinGW CMake leg (`add_executable(neroconsole …)`). No Qt or graphical dependency.
- **Verified** on macOS arm64 (Apple clang) by `tests/test_console_parity.py` (run under `make test`, TEST=0): `--doctor` output byte-identical to `nero` (816 bytes, rc 0), `version` identical, and the `help` command vocabulary identical (36 lines). This proves shared-core behaviour, not just a launching binary.
- Still **BLOCKED**: `nero-gui`. No Qt 6 toolchain is installed in this environment (`qmake`/`qmake6` absent, no Homebrew `qt`), so the Qt desktop application cannot be built or verified here. The non-Qt build is preserved and unaffected. GUI delivery is the remaining gate before any feature can be marked globally IMPLEMENTED.
- Unified operation registry (`operation.h` dispatcher) remains PLANNED; `neroconsole` reaches parity today by sharing the existing dispatcher directly.

The matrix rows below retain their audited status; each row's **Console Support** is now served by the shared dispatcher inside `neroconsole` (same vocabulary as `nero`, per the parity test), but rows stay PARTIAL until the GUI adapter and per-row three-frontend contract checks land.

**Original audit note.** The audited tree had no `neroconsole` or `nero-gui` application target, Qt/QML sources, or common operation registry. `nero` is the existing interactive terminal. Calling that terminal “Console” does not establish support for the separate required application.

Operation IDs below are design candidates, not implemented registrations. The “CLI Support” column describes source handlers inside the interactive shell; it does not claim general argv execution. `src/main.c` special-cases only `--doctor`, then enters `terminal_code()`. Platform cells record source-level availability/constraints, not a claim that hardware works. Build flags may remove live features on any OS. A family of existing test files is a starting point for inspection, not proof that every row is covered or passes.

Statuses: IMPLEMENTED requires verified applicable three-adapter contracts; PARTIAL means existing implementation with parity gaps; UNSUPPORTED means an explicit supported-model limitation; PLANNED means absent functionality; BLOCKED means an unresolved prerequisite. A missing frontend is a parity gap, not an OS limitation.

## Feature matrix

| Feature | Operation ID | Core Implementation | CLI Support | Console Support | GUI Support | macOS Capability | Linux Capability | Windows Capability | Tests | Status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| GENERAL: command discovery | `general.help` | CLI catalogs/render.c | help; help --all | absent | absent | source path exists | source path exists | native CLI target only | test_safe.py; not run | PARTIAL |
| GENERAL: version | `general.version` | printing.h / cmd_version | version | absent | absent | source path exists | source path exists | native CLI target only | test_safe.py; not run | PARTIAL |
| GENERAL: clear | `presentation.clear` | frontend-local cmd_clear | clear | absent | absent | source path exists | source path exists | native CLI target only | test_terminal_quality.py; not run | PARTIAL |
| GENERAL: exit | `presentation.exit` | frontend-local cmd_exit | exit; exit -All | absent | absent | source path exists | source path exists | native CLI target only | test_safe.py; not run | PARTIAL |
| HISTORY: read | `history.list` | CLI history.c/input.c | history | absent | absent | permissions/storage dependent | permissions/storage dependent | permissions/storage dependent | test_history.py; not run | PARTIAL |
| HISTORY: clear | `history.clear` | CLI cli_history_clear | history clear | absent | absent | permissions/storage dependent | permissions/storage dependent | permissions/storage dependent | test_history.py; not run | PARTIAL |
| LOGGING: read | `logs.list` | core/log.c; FILE output | logs | absent | absent | permissions/storage dependent | permissions/storage dependent | permissions/storage dependent | test_logging.py; not run | PARTIAL |
| LOGGING: severity filter | `logs.errors` | nero_log_dump | logs errors | absent | absent | permissions/storage dependent | permissions/storage dependent | permissions/storage dependent | test_logging.py; not run | PARTIAL |
| LOGGING: location | `logs.path` | nero_log_show_path | logs path | absent | absent | permissions/storage dependent | permissions/storage dependent | permissions/storage dependent | test_logging.py; not run | PARTIAL |
| LOGGING: clear | `logs.clear` | nero_log_clear | logs clear | absent | absent | permissions/storage dependent | permissions/storage dependent | permissions/storage dependent | test_logging.py; not run | PARTIAL |
| DIAGNOSTICS: doctor | `diagnostics.doctor` | core/doctor.c; FILE output | doctor; process --doctor | absent | absent | source path exists | source path exists | native CLI target only | test_doctor.py; not run | PARTIAL |
| NETWORK: mode status | `network.status` | CLI-static scan_enabled | network status | absent | absent | source path exists | source path exists | native CLI target only | test_safe.py; not run | PARTIAL |
| NETWORK: enable | `network.enable` | CLI cmd_mode | network enable | absent | absent | source path exists | source path exists | native CLI target only | test_safe.py; not run | PARTIAL |
| NETWORK: disable | `network.disable` | CLI cmd_mode | network disable | absent | absent | source path exists | source path exists | native CLI target only | test_safe.py; not run | PARTIAL |
| NETWORK: TCP scan | `network.scan` | scan_session.c; CLI port selection and TCP recheck | scan <IP> | absent | absent | build/network dependent | build/network dependent | MinGW TCP; raw engine excluded | scan_session.c; portscanner_reliability.c; linux/scanner_runtime.c; not run | PARTIAL |
| NETWORK: interface enumeration | `network.interfaces` | select_interface platform API; no standalone general command | indirect selection only | absent | absent | platform API | platform API | adapter API | interfaces.c; not run | PARTIAL |
| WIRELESS: help | `general.help (wireless scope)` | CLI print_wifi_help | wifi; wifi help | absent | absent | source path exists | source path exists | native CLI target only | test_wifi_tables.py; not run | PARTIAL |
| WIRELESS: status | `wifi.status` | wifi_platform.c + session; CLI aggregation | wifi status | absent | absent | CoreWLAN; may redact | metadata/nl80211 | adapter metadata | test_wifi_tables.py; not run | PARTIAL |
| WIRELESS: capabilities | `wifi.capabilities` | platform/capture APIs; CLI aggregation | wifi capabilities | absent | absent | CoreWLAN; may redact | metadata/nl80211 | adapter metadata | test_wifi_tables.py; wifi_nl80211.c; not run | PARTIAL |
| WIRELESS: interfaces | `wifi.interfaces` | nero_wifi_platform_list_interfaces | wifi interfaces | absent | absent | platform API | platform API | adapter API | test_wifi_tables.py; not run | PARTIAL |
| WIRELESS: active scan | `wifi.scan` | nero_wifi_platform_scan; CLI live loop/filtering | wifi scan | absent | absent | CoreWLAN/build dependent | unsupported backend | unsupported backend | three-adapter tests missing; not run | PARTIAL |
| WIRELESS: AP observations | `wifi.aps` | nero_wifi_session_get_aps | wifi aps | absent | absent | in-process data required | in-process data required | in-process data required | wifi_core.c; test_wifi_tables.py; not run | PARTIAL |
| WIRELESS: client observations | `wifi.clients` | nero_wifi_session_get_clients | wifi clients | absent | absent | in-process data required | in-process data required | in-process data required | wifi_core.c; test_wifi_tables.py; not run | PARTIAL |
| WIRELESS: signal | `wifi.signal` | platform status; CLI live loop | wifi signal | absent | absent | RSSI/channel conditional | signal fields not exposed | signal fields not exposed | three-adapter tests missing; not run | PARTIAL |
| WIRELESS: inspect observation | `wifi.inspect` | session_find_ap / session_find_client | wifi inspect <MAC> | absent | absent | in-process data required | in-process data required | in-process data required | wifi_core.c; not run | PARTIAL |
| WIRELESS: monitor foreground | `wifi.monitor` | wifi_session; CLI dashboard, assist and hop orchestration | wifi monitor | absent | absent | libpcap/permission/adapter; zero frames possible | libpcap/permission/adapter | Npcap/permission/adapter | wifi_core.c; test_wifi_tables.py; not run | PARTIAL |
| WIRELESS: monitor mode enable | `wifi.mode.enable` | nl80211; CLI confirmation and saved state | wifi monitor enable | absent | absent | unsupported platform | nl80211/adapter/CAP_NET_ADMIN + consent | unsupported platform | wifi_nl80211.c; not run | PARTIAL |
| WIRELESS: monitor mode restore | `wifi.mode.disable` | nl80211; CLI confirmation and saved state | wifi monitor disable | absent | absent | unsupported platform | nl80211/adapter/CAP_NET_ADMIN + consent | unsupported platform | wifi_nl80211.c; not run | PARTIAL |
| WIRELESS: channel hopping | `wifi.monitor (hop option)` | nl80211 channel control; CLI orchestration | wifi monitor --hop | absent | absent | unsupported platform | nl80211/adapter/CAP_NET_ADMIN + consent | unsupported platform | wifi_nl80211.c; not run | PARTIAL |
| WIRELESS: CSV export | `wifi.export` | serializer and file I/O in CLI wifi_command.c | wifi export | absent | absent | permissions/storage dependent | permissions/storage dependent | permissions/storage dependent | three-adapter export tests missing; not run | PARTIAL |
| PCAP: analyze | `pcap.analyze` | nero_wifi_session_analyze / analyze_pcap | wifi analyze <PCAP> | absent | absent | offline parser | offline parser | offline parser | wifi_core.c; test_wifi_tables.py; not run | PARTIAL |
| PCAP: packet list/detail/bytes | `pcap.packets (proposed)` | parsers exist; no retained packet query API in wifi.h | absent | absent | absent | unimplemented query API | unimplemented query API | unimplemented query API | missing; not run | PLANNED |
| SESSIONS: start capture | `wifi.session.start` | nero_wifi_session_start | wifi monitor start | absent | absent | libpcap/permission/adapter; zero frames possible | libpcap/permission/adapter | Npcap/permission/adapter | three-adapter lifecycle tests missing; not run | PARTIAL |
| SESSIONS: stop capture | `wifi.session.stop` | nero_wifi_session_stop | wifi monitor stop | absent | absent | in-process data required | in-process data required | in-process data required | three-adapter lifecycle tests missing; not run | PARTIAL |
| SESSIONS: snapshot | `wifi.session.snapshot` | snapshot_stats / running / last_error | wifi monitor status | absent | absent | in-process data required | in-process data required | in-process data required | three-adapter lifecycle tests missing; not run | PARTIAL |
| SESSIONS: reset | `wifi.session.reset` | nero_wifi_session_reset | wifi clear | absent | absent | in-process data required | in-process data required | in-process data required | three-adapter lifecycle tests missing; not run | PARTIAL |
| LABORATORY: read-only preflight | `lab.preflight` | core/hardware.c; terminal-bound workload.c screen | preflight; syn preflight | absent | absent | source path exists | source path exists | native CLI target only | workload_profile.c; syn_settings.c; test_safe.py; not run | PARTIAL |
| LABORATORY: restricted legacy operation | `lab.legacy (reserved, blocked migration)` | existing engine + CLI entry; safety review required | syn <IP> | absent | absent | existing build/privilege gates; unaudited | existing build/privilege gates; unaudited | unavailable raw engine | safe/mock compatibility only; never live traffic; not run | BLOCKED |

## Canonical syntax and confirmed aliases

Evidence: `src/cli/commands.c`, `command_catalog.h`, `wifi_command.c`, `parser/parser.c`, `render.c`, `src/main.c` and `docs/command-reference.md`.

| Canonical syntax | Accepted compatibility forms / qualifications |
| --- | --- |
| help | no arguments; help --all and help -All request details |
| version | no arguments |
| clear | no arguments; frontend-local viewport action |
| exit | exit -All also clears; preserve that option rather than discarding it |
| history | no arguments |
| history clear | history -clear |
| logs | log |
| logs errors | log -errors |
| logs path | log -path |
| logs clear | log -clear |
| doctor | process invocation nero --doctor bypasses history/log/Winsock initialization |
| network status | -mode; <IPv4> -mode (target is validated but not contacted) |
| network enable | -mode on |
| network disable | -mode off |
| scan <IPv4> | <IPv4> -Sc; -Sc <IPv4> |
| syn preflight | preflight; informational only |
| syn <IPv4> | <IPv4> -Syn; -Syn <IPv4>; preserve existing restrictions, do not run in tests |
| wifi help | wifi |
| wifi status / capabilities / interfaces | each accepts no additional arguments |
| wifi scan [INTERFACE] | --band B, --channel N, --live; active OS scan |
| wifi aps | --channel N |
| wifi clients | --channel N, --bssid MAC, --live, --anonymize |
| wifi signal | --live |
| wifi inspect <MAC or BSSID> | colon, hyphen, or 12-hex-digit MAC parsing |
| wifi analyze <PCAP> | quoted paths handled by existing tokenizer; pcap/pcapng |
| wifi export [FILE] | --anonymize, --channel N; default nero-wifi-export.csv |
| wifi clear | stops capture and resets in-memory observations |
| wifi monitor [INTERFACE] | --duration N, --channel N, --link, --no-scan; foreground |
| wifi monitor start [INTERFACE] | background capture; --link supported |
| wifi monitor stop | stops current process's capture |
| wifi monitor status | current process's session snapshot |
| wifi monitor enable [INTERFACE] | Linux interface-mode change with interactive confirmation |
| wifi monitor disable [INTERFACE] | Linux restore with confirmation and documented fallback |
| wifi monitor --hop [INTERFACE] | --dwell MS, --bands 2.4,5,6; foreground only; Linux radio mutation |

The monitor parser currently consumes options before branching on the action; some irrelevant options can be accepted then ignored (for example duration on a background start). Preserve documented syntax; capture actual behavior with characterization tests before tightening accidental combinations. Do not create aliases for syntax not observed in the source. There is no `sim` keyword in the inspected catalog.

## Architectural findings and exact migration paths

1. **Network code in frontend:** `src/cli/commands.c::tcp_port_check` performs socket I/O and `cmd_scan` invokes it after terminal port selection. Reuse `include/nero/scan_session.h` for scanning; move any retained selected-port check into a terminal-independent module and test its result contract.
2. **Multiple catalogs:** `CLI_KEYWORDS`, `CLI_GROUPED_HELP`, `CLI_HELP_ENTRIES` and `wifi_command.c::print_wifi_help` are separately maintained. Generate help/discovery and typed alias resolution from one registry; aliases must not duplicate executors.
3. **State and safety gates are frontend-owned:** `commands.c::scan_enabled` gates TCP and legacy operations. `cmd_wifi` is separate and does not consult this variable. Define explicit per-operation policy before claiming a global network-disable switch; moving this behavior without a contract would be a semantic change.
4. **False success risk:** invalid/unknown commands and several unavailable/failing handlers return 0/CONTINUE. Define structured operation status independently of shell “continue versus exit”; the CLI process must map actual operation failure to a nonzero exit code.
5. **Export is not core-owned:** `wifi_command.c::cmd_wifi_export` owns CSV, pseudonymization and destination writes. Extract a shared exporter with explicit overwrite policy, private-file handling and write-error results. Preserve observation provenance and missing-field flags.
6. **Radio mutation is frontend-owned orchestration:** `cmd_wifi_monitor_mode`, saved interface state, `active_hop`, and OS scan assistance live in `wifi_command.c`. Move policy/execution into the service; keep only presentation and consent interaction in adapters. The default monitor can perform active OS scans; `--no-scan` must remain strictly passive.
7. **Session APIs already exist:** reuse `nero_scan_*` and `nero_wifi_session_*`. Their lifetimes are process-local. Do not introduce a daemon or claim cross-process sharing. Retain separate OS-survey and passive-capture provenance.
8. **PCAP scope is limited:** `include/nero/wifi.h` offers frame parsers and aggregate tracker/session results; it does not provide retained packet indexing for a packet-list/detail/byte GUI. Extend the core first; that view is PLANNED.
9. **Preflight is not a reusable service yet:** `src/cli/workload.c::preflight_screen` combines hardware facts with a blocking terminal menu. Extract facts; preserve no-network semantics.
10. **Legacy laboratory path needs review:** comments claiming “no network engine” in `commands.c` contradict the live `SYN_Flood` call. Do not treat comments or warning text as authorization enforcement. Keep new frontend execution blocked until existing checks, cancellation and confirmation can be proven equivalent. No traffic-generation changes belong in this migration.
11. **Documentation drift:** `docs/command-reference.md` says wireless never changes channel before describing Linux hopping. Reconcile wording with active OS scan and opt-in radio mutations when migrating the catalog.
12. **Missing governance baseline:** the audited tree has no AGENTS.md, .agents or .codex directory. The eight named domain skills are absent, so none can truthfully be described as “updated.” The new repository-wide rule applies when they are introduced.

## Verification and next milestone

Use [the operation architecture](unified-operations.md) for the Phase B contract. Preserve the existing reliability roadmap in `ROADMAP.md` and `docs/engineering/quality-plan.md`.

Phase A source inventory is recorded here. B–F remain BLOCKED on an execution-capable checkout; no registry, adapter, Qt UI or parity CI implementation is claimed by this documentation milestone. Governance files are proposed in the same change; their runtime loading still needs verification.

First executable slice: shared version/help/capability contracts, canonical + legacy resolution, CLI/Console adapters and a Qt controller contract test. Then migrate logs/doctor/history, network state/TCP sessions, Wi-Fi/PCAP/export, and finally review restricted legacy exposure. Each slice needs all applicable adapters before it can become IMPLEMENTED.

Required tests: compare normalized IDs, targets/options, typed data, failures, cancellation and cleanup across core and all adapters. Use deterministic capture/PCAP fixtures and loopback-only TCP; never external scans or experimental transmission. Real-hardware evidence is separate. Non-GUI CI must remain buildable without Qt. A dedicated Qt CI job and a registry-to-adapter behavior gate are outstanding; existing CI does not enforce parity.

Update this matrix with commit, platform/build, exact test command and outcome for each implemented slice. A missing runner, omitted test or skipped hardware check is not a pass.
