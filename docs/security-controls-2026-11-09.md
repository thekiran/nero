# Security controls audit — 9 November 2026

Audited baseline main: `1dceac505ebc77d6003306ec6a1ac37150298768`.
A checked-in workflow/policy is configuration intent, not proof of enforcement.

| Control | Observed state | Evidence / limitation |
| --- | --- | --- |
| Main protected | Enabled | GET `/repos/thekiran/nero/branches/main`: protected=true |
| Required check names | Enabled, partial visibility | Same response lists Release checks, clang (ubuntu-latest), clang (macos-latest), msvc, app_id 15368, enforcement everyone |
| Strict/up-to-date and admin enforcement | Enabled | Authenticated Settings branch rule 85993164: require PR, up-to-date checks, conversation resolution and no bypass are checked |
| Force push / deletion / review | Force push and deletion disabled; approval not required | Authenticated branch rule: allow force pushes/deletions unchecked; require approvals unchecked, stale approvals dismissed |
| Repository rulesets | None returned | GET `/repos/thekiran/nero/rulesets` returns [] |
| CodeQL baseline setup | Disabled | Authenticated Advanced Security page offers Set up; no configured CodeQL workflow |
| New advanced CodeQL | Proposed, execution pending | security.yml, C/C++ manual production Linux build and Actions analysis; must inspect first run/SARIF |
| Dependency graph / Dependabot alerts / security updates | Disabled | Authenticated Advanced Security page: Off for all three |
| Private vulnerability reporting | Disabled | Authenticated Advanced Security page: Enable private vulnerability reporting |
| Secret scanning / push protection | Enabled | Authenticated Advanced Security page: Secret Protection Disable button, Push protection On |
| Code scanning failure threshold | High or higher | Authenticated Advanced Security protection rules (standard alerts: only errors) |
| Scorecard | Proposed, execution pending | Trusted-main bounded JSON artifact; no public-results upload, OIDC, PAT or extra write token |
| SHA-pinned Actions | Enabled in reviewed source | Existing and new external uses carry full 40-character commit hashes |
| Repository workflow permissions | Read-only default | Authenticated Actions settings selects Read repository contents and packages; create/approve PR unchecked |
| Fork execution approval | First-time contributors | Authenticated Actions settings; not all external contributors |
| Server SHA pin enforcement | Disabled | Require actions pinned to full SHA unchecked; reviewed source pins remain intact |
| Workflow token permissions | Read-only default | All reviewed workflows default contents:read; trusted release publish alone grants contents:write; CodeQL grants security-events:write only for analysis |
| PR publication secrets | Excluded in source | No pull_request_target; Wiki publisher excludes PRs and requires main; tag release publication is gated; checkout credentials are disabled |
| Fuzz/CodeQL required checks | Not enabled by this change | Administrator must compare observed job names and add mandatory checks without replacing stronger existing rules |

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
