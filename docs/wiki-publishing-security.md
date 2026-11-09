# Wiki publishing credentials

PR validation generates and audits pages using read-only repository permission;
only the trusted main publish job receives WIKI_PUSH_TOKEN. Checkout does not
persist credentials. Publication fails if the secret is absent, clone fails, or
sync/push fails; generated pages remain available in the validation artifact.

The token is supplied to Git through an ephemeral askpass helper's environment.
The remote URL is credential-free. Git helpers, global configuration and tracing
are isolated; transport output is discarded rather than depending on GitHub's
literal masking. No credential file or diagnostic artifact is written. The
helper script contains no secret and the temporary clone is removed on exit.
A trusted runner and trusted main code remain part of the security boundary:
environment credentials are not a defense against malicious same-user processes.

Owner migration: replace any broad classic PAT with an expiring fine-grained PAT
restricted to this repository and Contents read/write (Wiki publishing requires
repository write access). Store it only as the Actions secret WIKI_PUSH_TOKEN;
do not paste its value into commands, issue comments or documentation. Confirm
Wiki access with one trusted-main dispatch and inspect the honest publish status.
Revoke the old credential after successful migration; rotate immediately if an
exposure is suspected. Review existing Wiki remotes privately and replace any
credential-bearing URL with https://github.com/thekiran/nero.wiki.git. Never upload
old Git configs or transport traces as evidence. GitHub App/GITHUB_TOKEN Wiki
support must be verified before substituting a token type; do not assume it.

Validation: `python3 -m unittest tests/test_wiki_sync.py`; this uses dummy secrets
and local Git only. Successful local tests do not verify owner credential scope
or successful live Wiki publication; the trusted-main workflow supplies that evidence.
