> Historical record: this describes an earlier source revision, not current certification. See [current quality validation](quality-validation.md).

# NERO reliability workplan

Status date: 2026-11-08. Branch: `fix/scanner-reliability`.
Audited HEAD: `a1a01ed493a077eacd4990d7450b4e19f0106dd9` (equal to `origin/main`
at audit time; working tree clean before this change).

This plan tracks the multi-phase reliability, cross-platform and release work.
It records what was already fixed on main (verified, not re-implemented), what
was changed in this pass, and what remains blocked by unavailable platforms or
privileges. Coverage completeness and port-state certainty are tracked as two
separate properties throughout.

## Agent ownership

The repository already defines eight project agents under `.claude/agents/`
that cover the eight requested engineering roles one-to-one. They were reused
rather than duplicated under new names (no correct, existing artifact was
re-implemented):

| Requested role | Existing agent | Scope |
| --- | --- | --- |
| audit-architect | `nero-auditor` | read-only architecture/security/correctness review |
| scanner-engineer | `nero-network` | scanner correctness, sockets, testability |
| concurrency-engineer | `nero-network` + `nero-qa` | threads, atomics, FD budgets |
| test-engineer | `nero-qa` | offline regression, sanitizers, reproducibility |
| platform-engineer | `nero-platform` | macOS/Linux adapters, OS capability boundaries |
| cli-engineer | `nero-cli` | command registry, parser, dispatch contracts |
| network-features | `nero-network` | safe diagnostics, additive features |
| release-engineer | `nero-build` + `nero-docs` | build systems, CI, packaging, docs |

Investigations may run in parallel; conflicting edits to shared files must not.
This pass made a single, isolated edit to `src/cli/commands.c` plus its tests.

## Phase status

| Phase | Owner | Status | Evidence |
| --- | --- | --- | --- |
| 0 Audit | auditor | DONE | `docs/engineering/phase0-audit.md` |
| 1 Scanner correctness | network | VERIFIED (already fixed) | indexed ledger, advisory discovery, chunked retry; see audit |
| 2 Concurrency/perf | network/qa | VERIFIED (already fixed) | TSan clean, shared atomic queue, caller-as-worker, bounded budgets |
| 3 Test engineering | qa | VERIFIED + extended | full suite + ASan/UBSan/TSan pass; 2 mode-off regressions added |
| 4 Cross-platform | platform | PARTIAL | macOS arm64 TESTED here; Linux/Windows = CI cross-build only (BLOCKED natively) |
| 5 CLI/logging | cli | FIXED_NOW | `-mode off` now gates `-Syn` at dispatch (was `-Sc` only) |
| 6 Additive features | network | NOT STARTED | gated behind core correctness; scoped as follow-up |
| 7 CI/release/docs | build/docs | PARTIAL | version model verified intentional; release draft state not re-checkable (no `gh`) |
| 8 Final review | auditor | DONE (read-only self-review) | see audit "Independent review" |

## Dependencies

- Phase 2 runs only after Phase 1 is stable — satisfied (no scanner logic changed).
- Phase 6 starts only after core correctness tests pass — not started by choice;
  the features are net-new surface area and are left as a reviewable follow-up
  rather than bundled into a reliability fix.
- Release (Phase 7) must never publish after failed tests and is never triggered
  from this environment (policy rule 11).

## Blockers recorded

- Native Linux x86-64/ARM64 and native Windows execution: not available in this
  macOS environment. CI (`.github/workflows/`) is the authority for those; this
  pass did not run them.
- `gh` CLI is not installed, so GitHub release draft/published state and Issue #9
  linkage could not be re-verified against the live repository.
- Physical Wi-Fi / mobile-device scan validation: out of scope for offline CI;
  remains an explicit limitation.
