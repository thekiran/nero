# Wiki publication setup

The technical docs in `docs/` remain the canonical source. The GitHub Wiki
is a generated mirror, not a second independent source of truth.

## Configure once as repository owner

1. Initialize the [Wiki](https://github.com/thekiran/nero/wiki) by creating
   its first page if the Wiki Git repository does not exist yet.
2. Create an expiring fine-grained personal access token restricted to this
   repository with only Contents read/write permission. Never commit or
   print the credential.
3. In **Settings → Secrets and variables → Actions**, add the repository
   secret `WIKI_PUSH_TOKEN`.
4. Run **Actions → Documentation and Wiki Sync → Run workflow** on `main`.
5. Confirm both the link validation and the publish step, then inspect the
   generated Wiki pages. The publish job summary states whether changes were
   pushed, the Wiki was already current, or publication failed.

See [Wiki publishing credentials](wiki-publishing-security.md) for how the
token is handled, migrating from a classic token, rotation and revocation.

A green validation workflow means the docs could be built; it does not alone
prove they were published. When the secret is absent or the Wiki repository
cannot be cloned, the publication job fails. Validation still archives the
`wiki-pages` artifact. Pull requests only validate and never publish.
Re-run after correcting the configuration.

Do not grant the workflow permissions to arbitrary repositories or use a
personal token with unnecessary access.
