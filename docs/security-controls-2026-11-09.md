# Security controls audit — 9 November 2026

Audited baseline main: `1dceac505ebc77d6003306ec6a1ac37150298768`.
A checked-in workflow/policy is configuration intent, not proof of enforcement.

| Control | Observed state | Evidence / limitation |
| --- | --- | --- |
| Main protected | Enabled | GET `/repos/thekiran/nero/branches/main`: protected=true |
| Required check names | Enabled with the original 4 checks; 8-check proposal **not saved** server-side | Live state lists Release checks, clang (macos-latest), clang (ubuntu-latest), msvc (app_id 15368, enforcement everyone). The 8-check `.github/main-protection.json` proposal awaits owner re-authentication; do not describe it as active |
| Strict/up-to-date and admin enforcement | Enabled | Authenticated Settings branch rule 85993164: require PR, up-to-date checks, conversation resolution and no bypass are checked |
| Force push / deletion / review | Force push and deletion disabled; approval not required | Authenticated branch rule: allow force pushes/deletions unchecked; require approvals unchecked, stale approvals dismissed |
| Repository rulesets | None returned | GET `/repos/thekiran/nero/rulesets` returns [] |
| CodeQL baseline setup | Disabled | Authenticated Advanced Security page offers Set up; no configured CodeQL workflow |
| New advanced CodeQL | Enabled in source; trusted-main runs succeeded | security.yml C/C++ manual production Linux build and Actions analysis uploaded on main in [run 42652688683](https://github.com/thekiran/nero/actions/runs/42652688683) (`2a1804a`) and [run 42656860302](https://github.com/thekiran/nero/actions/runs/42656860302) (`0161f6f`) |
| Dependency graph / Dependabot alerts / security updates | Disabled | Authenticated Advanced Security page: Off for all three |
| Private vulnerability reporting | Disabled | Authenticated Advanced Security page: Enable private vulnerability reporting |
| Secret scanning / push protection | Enabled | Authenticated Advanced Security page: Secret Protection Disable button, Push protection On |
| Code scanning failure threshold | High or higher | Authenticated Advanced Security protection rules (standard alerts: only errors) |
| Scorecard | Enabled in source; trusted-main runs succeeded | Bounded JSON artifact from [run 42652688683](https://github.com/thekiran/nero/actions/runs/42652688683) (`2a1804a`) and [run 42656860302](https://github.com/thekiran/nero/actions/runs/42656860302) (`0161f6f`); no public-results upload, OIDC, PAT or extra write token |
| SHA-pinned Actions | Enabled in reviewed source | Existing and new external uses carry full 40-character commit hashes |
| Repository workflow permissions | Read-only default | Authenticated Actions settings selects Read repository contents and packages; create/approve PR unchecked |
| Fork execution approval | First-time contributors | Authenticated Actions settings; not all external contributors |
| Server SHA pin enforcement | Disabled | Require actions pinned to full SHA unchecked; reviewed source pins remain intact |
| Workflow token permissions | Read-only default | All reviewed workflows default contents:read; trusted release publish alone grants contents:write; CodeQL grants security-events:write only for analysis |
| PR publication secrets | Excluded in source | No pull_request_target; Wiki publisher excludes PRs and requires main; tag release publication is gated; checkout credentials are disabled |
| Fuzz/CodeQL required checks | Proposed, not saved | Listed in the 8-check JSON proposal; `tests/test_workflow_security.py` asserts every proposed context matches a real job. Owner must save it without replacing stronger existing rules |

CodeQL uses a real `make CC=gcc -B` compilation, covering Linux production parser,
history, logging, scanner, packet engine and Linux adapters without executing the
binary. It does not cover macOS/Windows conditional branches; existing native
Clang/MSVC/MinGW analysis and tests remain necessary. The Actions query database
covers workflows. security-events permission is limited to uploading analysis;
fork PR permission follows GitHub's read-only token restrictions. No PAT or
publication secret is provided to analysis jobs. If default setup is active,
GitHub may reject advanced uploads; an administrator must resolve that setup
conflict deliberately rather than hiding upload errors.

Scorecard inspects repository metadata and source; it is not a local packet test
and its numeric result is not a quality guarantee. Triage actionable findings
from the artifact with an owner, severity, evidence and tracked fix. Missing
settings visibility must remain unknown rather than "pass". New high/critical
CodeQL findings block release until fixed with a regression or explicitly accepted
by the owner with rationale, mitigation, scope and an expiry/review date. Do not
suppress findings merely to make CI green.

Authenticated owner-session Settings pages were inspected read-only on this date:
Advanced Security, Actions General, and branch rule 85993164. No settings were
changed. API protection details still return 403, but the UI verified the saved
policy above. Remaining verification: compare required check names with current
check-runs, review first CodeQL/SARIF and Scorecard output, and add new checks
after successful runs. Disabled dependency/private-reporting controls are owner
follow-ups, not silently claimed enabled. Preserve the sole-owner review policy.
Do not apply the sample JSON blindly.

References: [CodeQL setup](https://github.com/github/codeql-action),
[Scorecard action](https://github.com/ossf/scorecard-action),
[branch rule verification](repository-rules.md).

## Release permission finding and correction

Source review found that the baseline `draft` aggregation job requested
contents:write even on PRs, although its actual publication step was excluded.
No token disclosure was observed. Package/checksum verification now stays in a
read-only job and hands a verified artifact set to a separate main/tag-only
publication job. The privileged job rechecks SHA256SUMS. The aggregate Release
checks gate requires verification and successful publication for trusted release
runs, or an explicitly skipped publisher for PR validation. Arbitrary branch
workflow_dispatch can validate packages but cannot publish. Security regression
assertions run under the existing required Clang static-analysis checks.

## Verified rollout and remaining administrator step

PR #67 merged as `2a1804a347d641daa937b472675b19e61b512978` after all native,
release, static-analysis, fuzz and CodeQL checks passed. Trusted-main
[run 42652688683](https://github.com/thekiran/nero/actions/runs/42652688683)
then successfully built/uploaded the C/C++ and Actions analyses and produced the
Scorecard JSON artifact. The PR's GitHub Advanced Security `CodeQL` result check
also passed; that result check is PR-specific and is not claimed as a main job.
C/C++ manual mode used a full database; the overlay-base optimization warning
was a fallback to full analysis, not skipped source coverage.

The new eight-check proposal was populated in the owner Settings form with the
correct GitHub Actions / Advanced Security app bindings. **Saving was blocked by
GitHub's Confirm access / sudo-mode reauthentication.** No new access was granted,
no credential was read, and no existing control was removed. The actual server
policy still has the four original required checks, strict/up-to-date enforcement,
required PRs/resolved conversations, admin enforcement, no force pushes and no
branch deletion. The JSON is a proposal until the owner completes GitHub access
verification and the saved server policy is checked again. #55 stays open for
this administrator step; do not describe the eight checks as active.

### Scorecard triage

Observed report: Scorecard v5.3.0, source `2a1804a`, overall 7.6. This is a supply
chain audit score, not a product reliability rating or security certification.
Token-Permissions, Dangerous-Workflow, Fuzzing and CI-Tests each scored 10.

| Finding | Priority / owner | Disposition |
| --- | --- | --- |
| Branch-Protection -1 | P1 / owner | Tool token cannot read classic protection; owner UI verified existing controls. New eight-check save awaits GitHub reauthentication. Unknown is not disabled. |
| Signed-Releases 0 | P2 / owner, #11 | Published artifacts remain unsigned; signing/provenance trust must not be fabricated. Existing owner credential/infrastructure blockers remain. |
| Pinned-Dependencies 8 | P2 / maintainer | Addressed in source: tests/linux/Dockerfile now pins the official multi-arch Ubuntu 24.04 index digest (amd64 and arm64), Dependabot tracks it, and `tests/test_workflow_security.py` rejects unpinned `FROM` lines. Rebuild the local validation container before relying on it; the score updates on the next Scorecard run. |
| Code-Review 0 | P2 / owner | Solo-maintainer policy currently requires zero approvals. Add an independent reviewer before requiring one approval; do not lock the maintainer out. |
| SAST 7 | Informational / maintainer | CodeQL is newly enabled; 4 of the historical 30 commits had recognized scans. Keep it running; do not fabricate historical results or rewrite main for a score. |
| Packaging -1 | Detection limitation / maintainer | Custom native release workflow was not recognized by Scorecard; actual five-platform installer/archive checks and public assets are separate positive evidence. |
| CII-Best-Practices 0 | Optional / owner | Badge participation is optional and separate from essential release requirements. |
| Contributors 0 | Informational / owner | Corporate-contributor diversity is not a correctness guarantee and will not be faked. |

No new actionable high/critical CodeQL finding was reported by the successful
PR result gate. Lower-severity or future findings still require normal triage;
bounded analysis and a Scorecard score are not proofs of absence of defects.
