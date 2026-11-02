# Security policy

Nero is experimental laboratory software, not a production security product.
No version is advertised as security-certified or suitable for public deployment.

For sensitive findings, contact the repository owner privately through a contact
method published on their GitHub profile. If no private channel is available,
request one without posting exploit details. Do not include credentials, third-party
targets or operational attack instructions in public issues.

Include the affected commit, platform, expected behavior and an offline/minimal
reproducer. Do not validate reports by generating network load. Known risks and
untested areas are documented in docs/platform-compatibility.md and
[platform-support.md](docs/platform-support.md); migration does not certify the
unchanged legacy engine or bundled third-party code as safe.
