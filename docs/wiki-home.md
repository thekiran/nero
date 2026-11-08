# NERO Documentation

**Native C Terminal & Network Research**

NERO is an experimental C11 terminal project with an interactive command line,
command history, platform adapters and network diagnostics. This Wiki is the
starting point for installation, architecture, development and project planning.

[Repository](https://github.com/thekiran/nero) · [Releases](https://github.com/thekiran/nero/releases) · [Issues](https://github.com/thekiran/nero/issues) · [Roadmap](https://github.com/thekiran/nero/blob/main/ROADMAP.md)

## Terminal walkthrough

A 23-second replay of actual NERO terminal output on macOS ARM64: startup,
`help`, `version`, `history`, `history -clear`, `clear` and `exit`.
These terminal frames are rendered from a real PTY recording.

### Video

https://github.com/user-attachments/assets/3130879d-2cd6-49b9-a2d9-3dda86975f76

### Animated preview

![NERO terminal walkthrough](https://github.com/user-attachments/assets/ce906954-b458-4535-ae05-d2d62e649834)

### Startup

The native terminal opens with the NERO banner and interactive prompt.

![NERO startup](https://github.com/user-attachments/assets/6df681e8-b65d-4fe5-b93f-0e2c20a8466b)

### Command discovery

Run `help` to display command syntax and available options.

![NERO help menu](https://github.com/user-attachments/assets/461d0611-245e-47c3-9204-36afdfdfb979)

### Version and command history

Run `version` to inspect the CLI version string, then `history` to review
commands from the session. `history -clear` clears the session history.

![NERO version and history](https://github.com/user-attachments/assets/872cbf51-4d77-4a28-ac04-069d5d697368)

## Start here

| Your goal | Guide |
| --- | --- |
| Install and launch NERO | [Installation](https://github.com/thekiran/nero/blob/main/docs/installation.md) |
| Learn commands and syntax | [Command reference](https://github.com/thekiran/nero/blob/main/docs/command-reference.md) |
| Check operating system support | [Platform compatibility](https://github.com/thekiran/nero/blob/main/docs/platform-support.md) |
| Download and verify a package | [Release and installation guide](https://github.com/thekiran/nero/blob/main/docs/releases.md) |
| Resolve a build or runtime problem | [Troubleshooting](https://github.com/thekiran/nero/blob/main/docs/troubleshooting.md) |

## Platform overview

| Platform | Package targets | Build path |
| --- | --- | --- |
| macOS 15+ | Apple Silicon ARM64, Intel x86-64 | Make; CMake offline tests |
| Ubuntu 24.04 compatible | x86-64, ARM64 | Make; CMake offline tests |
| Windows 10/11 | Native x86-64 | CMake + MinGW UCRT64 |
| Windows with WSL2 | Matching Linux architecture | Linux build and package |

Package targets describe the build matrix. For current runtime limitations and
validation evidence, consult the platform compatibility guide and the linked CI
runs. Windows ARM64 has no native package. The experimental raw packet engine is
unavailable on Windows and is not exercised by CI on any platform.

## First session

After following the installation guide, launch `nero` and discover the built-in
commands:

```text
help
help -All
version
history
exit
```

Use the release manifest to identify a package version; the built-in version
string is documented separately in the command reference.

## Development and quality

| Topic | Documentation |
| --- | --- |
| Components and platform boundaries | [Architecture](https://github.com/thekiran/nero/blob/main/docs/architecture.md) |
| Toolchains and local builds | [Development guide](https://github.com/thekiran/nero/blob/main/docs/building.md) |
| Regression suites and sanitizers | [Testing](https://github.com/thekiran/nero/blob/main/docs/testing.md) |
| Proposing and reviewing changes | [Contributing](https://github.com/thekiran/nero/blob/main/CONTRIBUTING.md) |
| Milestones and acceptance criteria | [Roadmap](https://github.com/thekiran/nero/blob/main/ROADMAP.md) |
| Reporting vulnerabilities | [Security policy](https://github.com/thekiran/nero/blob/main/SECURITY.md) |

CI covers offline regression tests, platform adapters, static analysis and native
package verification. Windows additionally uses a bounded local TCP fixture.
Passing these checks does not establish full scanner or raw engine behavior.
See the testing guide for the exact scope and commands.

## Project status and feedback

NERO remains experimental. Follow the roadmap for planned work and the Releases
page for published packages. A draft release is available only to collaborators
until it is reviewed and published. Check the issue tracker before reporting a
problem, and include your OS, architecture, build method and reproduction steps.

Use network diagnostics only on systems you own or are authorized to test.
Report suspected vulnerabilities through the security policy.

## Documentation maintenance

The repository documentation is the source of truth. Wiki pages are generated
from version-controlled Markdown with `scripts/docs/wiki_sync.py`; propose
lasting documentation changes through a repository pull request. This home page
links directly to canonical guides so they remain accessible while the Wiki is
being populated.

Automatic publication runs only for `main` pushes or manual runs on `main`,
after documentation validation. An owner must initialize the Wiki by creating
its first page, then configure the repository Actions secret `WIKI_PUSH_TOKEN`
with a credential authorized to push to `thekiran/nero.wiki.git` (for a classic
personal access token, use the `repo` scope). Do not commit the credential.
Pull requests validate and archive generated pages without publishing.

On `main`, a missing credential, clone failure or push failure fails the
publication job. The `wiki-pages` artifact remains available from validation
when publication is blocked. Check the publication job and its synchronization
summary before claiming that the Wiki is current.

NERO is distributed under the [MIT License](https://github.com/thekiran/nero/blob/main/LICENSE.txt).
