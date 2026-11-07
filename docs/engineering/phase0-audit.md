# Phase 0 audit — NERO reliability

Audited commit: `a1a01ed493a077eacd4990d7450b4e19f0106dd9` (`origin/main` at audit
time). Host: Darwin arm64, Apple clang 21.0.0, Python 3.9.6. Date: 2026-11-08.

Method: source inspection plus a real build and the full offline test suite,
including ASan/UBSan and ThreadSanitizer. Source inspection is not treated as
runtime verification; every "verified" claim below is backed by an executed
test. No external network traffic was generated (loopback-only fixtures).

## Original findings (hypotheses) — disposition

| # | Hypothesis | File/evidence | Disposition |
| --- | --- | --- | --- |
| 1 | Host probe gates scanning | `portscanner.c:1101-1125` discovery is advisory; full fast phase runs regardless | ALREADY_FIXED |
| 2 | Deep Scan drops candidates past a cutoff | `run_deep_scan` uses `deep_max_candidates` as a work *window*, not a cutoff; `collect_retry_candidates` sweeps 1..65535 | ALREADY_FIXED |
| 3 | Worker-creation failure loses work | caller-as-worker (`run_scan_phase:677-682`) + shared atomic queue (`scan_work.next`) | ALREADY_FIXED (test: partial/all `pthread_create` failure) |
| 4 | Linear result searches (quadratic) | `results_list[1..65535]` indexed ledger, O(1) record/summary | ALREADY_FIXED |
| 5 | A port attempt records no state | `record_result` + final accounting at `portscanner.c:1137-1150` | ALREADY_FIXED |
| 6 | `tests/linux/scanner_runtime.c` references removed symbols | it `#include`s `portscanner.c` and injects faults via macros; no stale symbols | NOT PRESENT |
| 7 | `-mode off` not enforced consistently | `cmd_scan` gated on `scan_enabled` (`commands.c:310`); `cmd_syn` live path did **not** | PRESENT → FIXED_NOW |
| 8 | CLI version vs release metadata mismatch | `printing.h` `"version 1.0"` is intentionally decoupled; `docs/releases.md` states the release version lives in the archive/`BUILD-INFO.json` manifest, CLI text "unchanged" | NOT A DEFECT (VERIFIED intentional) |
| 9 | Draft releases unpublished | `gh` unavailable; cannot re-verify live state | UNKNOWN (blocked) |
| 10 | CI / branch protection effective | workflows present (`ci`, `compatibility`, `release`, `static-analysis`, `wiki-sync`); live protection not inspectable here | PARTIAL/UNKNOWN |

## State-classification correctness (verified)

`classify_connect_error` (`portscanner.c:474`): refusal → CLOSED, timeout →
FILTERED, everything else → UNKNOWN. A timeout is never reported as CLOSED; an
unknown socket error is never reported as OPEN or CLOSED. `state_evidence`
(`:171`) prevents a later retry from downgrading stronger prior evidence
(OPEN > CLOSED > FILTERED > unknown). Host silence yields `HOST_UNKNOWN`, never
`HOST_OFFLINE` (enum has no offline value). Cancellation leaves unfinished slots
unscanned and the completion status is reported as `cancelled`, distinct from
`complete`/`incomplete` (`:1139-1149`). All confirmed by the reliability suite.

## Resource bounds (verified)

Threads ≤ `MAX_THREADS` (16); per-call sockets ≤ `MAX_BATCH` (512); global FD
budget from `RLIMIT_NOFILE` minus a 64-FD reserve (`scan_socket_budget:419`);
retry windows halve concurrency on local resource exhaustion (`run_deep_scan`),
backing off rather than spinning. A port receives at most `1 + deep_rounds`
probes. ASan/UBSan and TSan runs are clean (see below).

## Executed verification (this host)

```
make                       # clean build, -Wall -Wextra -Werror -pedantic: no warnings
make test                  # exit 0
make test-sanitize         # ASan+UBSan, exit 0
make test-tsan             # ThreadSanitizer, exit 0, zero data races
```

Suite results: `test_logging` 14 OK, `test_safe` 11 OK, `test_terminal_quality`
23 OK, `operation_layout` PASS, `portscanner_reliability` PASS, `scanner_runtime`
PASS (loopback-only guard active, descriptor baseline restored).

## Change made in this pass

`cmd_syn` now returns the "Network diagnostics are disabled." notice and exits
before any transmission path when `scan_enabled` is false, mirroring `cmd_scan`.
This gates the CLI command handler only; `src/modules/syn_flood_powered.c` (the
legacy engine) is unchanged (policy rule 9). Regression coverage added:
`test_safe.py::test_mode_off_gates_syn_at_dispatch` and
`test_terminal_quality.py::test_mode_off_blocks_both_sc_and_syn`.

## Independent review (read-only, Phase 8)

Reviewed the diff for coverage/retry correctness, races, leaks, classification,
UI/terminal regressions and network-operation safety. The change adds an early
return on an existing, already-synchronized flag; it introduces no allocation,
thread, socket or terminal state and cannot affect scan coverage. No blocking
issues. Non-blocking follow-ups: Phase 6 additive features and live release/CI
re-verification (needs `gh` and the CI platform matrix).
