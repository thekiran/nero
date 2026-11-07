# Main branch protection

`.github/main-protection.json` is a reviewable GitHub REST branch-protection
configuration. Committing this file does not activate protection. It requires a
repository administrator to apply it through Settings or an authenticated GitHub
CLI with repository administration permission.

It requires a pull request, an up-to-date branch, resolved conversations and
successful `Release checks`, `clang` and `msvc` checks. Force pushes and deletion
are blocked; administrators are also subject to the rules. The aggregate Release
checks job always runs and fails when any supported platform fails.

The approval count is zero so a sole maintainer is not locked out of their own
repository. Review is still expected before merging. Once an independent reviewer
is available, set `required_approving_review_count` to 1 to enforce peer review.

After the stability workflow has reported its checks, apply from the repository
root with an administrator-authorized GitHub CLI session:

```sh
gh api --method PUT repos/thekiran/nero/branches/main/protection \
  --input .github/main-protection.json
```

Verify the actual server settings rather than assuming success:

```sh
gh api repos/thekiran/nero/branches/main/protection \
  --jq '{checks: .required_status_checks.contexts, strict: .required_status_checks.strict, admins: .enforce_admins.enabled, pr: .required_pull_request_reviews, force_push: .allow_force_pushes.enabled, deletion: .allow_deletions.enabled}'
```

Review existing repository rules before applying this payload; do not replace a
stronger policy with it. Check the API response and Settings to confirm that the
policy is active.
