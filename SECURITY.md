# NERO security policy

NERO is experimental laboratory software, **not a certified production security
product**. No release currently carries a security-support or response-time SLA.

## Report a vulnerability privately

Do not publish credentials, exploitable proof-of-concept details, third-party
targets, or live operational attack instructions in an issue or discussion.

- If the repository's GitHub **Private vulnerability reporting** feature is
  enabled, use **Security → Report a vulnerability** to open a private report.
- Otherwise, contact the repository owner privately through a contact method
  listed on their [GitHub profile](https://github.com/thekiran). If no private
  method is available, request one publicly **without vulnerability details**.

A report should include the affected commit or release, OS/architecture,
reproduction prerequisites, expected versus observed behavior, and an offline
or isolated-loopback minimal reproducer when feasible. Include a sanitized log
and the possible security impact; redact addresses, tokens and personal data.
The maintainer may request more evidence before triage. No fixed remediation
or disclosure schedule is promised.

## Scope and safe verification

- The interactive terminal, parsers, native installers, socket adapters and
  bounded TCP diagnostics are in scope for vulnerability reports.
- The experimental raw-packet engine must only be examined in a controlled
  and explicitly authorized environment. Do not demonstrate issues by
  generating unsolicited traffic or loads against external infrastructure.
- A passing CI run indicates offline regressions and package integrity,
  **not** an audit or certification of raw packet modules or bundled
  third-party code.
- Platform support limitations and known risks are recorded in
  [platform support](docs/platform-support.md) and
  [platform compatibility](docs/platform-compatibility.md).

For non-sensitive reproducible bugs, use the normal GitHub issue template.
