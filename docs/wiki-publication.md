# Wiki publication setup

The technical docs in `docs/` remain the canonical source. The GitHub Wiki
is a generated mirror, not a second independent source of truth.

## Configure once as repository owner

1. Initialize the [Wiki](https://github.com/thekiran/nero/wiki) by creating
   its first page if the Wiki Git repository does not exist yet.
2. Create an owner-controlled, revocable token authorized to push to
   `thekiran/nero.wiki.git`. Keep permissions as narrow as your GitHub
   token type allows. Never commit or print the credential.
3. In **Settings → Secrets and variables → Actions**, add the repository
   secret `WIKI_PUSH_TOKEN`.
4. Run **Actions → Documentation and Wiki Sync → Run workflow** on `main`.
5. Confirm both the link validation and the publish step, then inspect the
   generated Wiki pages. The run summary explicitly reports
   successful synchronization or a failed publication job.

A green validation workflow means the docs could be built; it does not alone
prove they were published. When the secret is absent or the Wiki repository
cannot be cloned, the publication job fails. Validation still archives the
`wiki-pages` artifact. Pull requests only validate and never publish.
Re-run after correcting the configuration.

Do not grant the workflow permissions to arbitrary repositories or use a
personal token with unnecessary access.
