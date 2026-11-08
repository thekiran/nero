# Distribution paths

## Normal application installation

Releases after v0.1.3 add native `.pkg` (macOS), `.msi` (Windows) and `.deb`
(Ubuntu-compatible Linux) installers alongside the existing portable archives.
Users open the installer file and complete the normal OS installation flow;
they do not need compiler or terminal setup commands. See
[installation](installation.md#recommended-standard-operating-system-installers).
macOS installs the global command in `/usr/local/bin`; Linux uses `/usr/bin`.
Windows Installer owns its Program Files payload, Start menu shortcut and
system PATH entry, including upgrade and removal. Restart existing Windows
Terminal processes once to inherit the environment update.

The release matrix installs actual `.pkg`/`.deb` payloads on disposable native
runners and resolves `nero` from unrelated directories in normal and sudo
shells. Windows runs the real MSI install/upgrade/remove sequence and checks
fresh PowerShell/CMD resolution and unrelated system PATH preservation.

## macOS Homebrew

The repository now contains `Formula/nero.rb`, pinned to the published v0.1.3
macOS ARM64/Intel archives and their SHA-256 digests. It requires macOS 15+.
It is a third-party tap formula, not a claim of inclusion in Homebrew Core:

```sh
brew tap thekiran/nero https://github.com/thekiran/nero.git
brew install thekiran/nero/nero
brew test thekiran/nero/nero
brew reinstall thekiran/nero/nero
brew uninstall thekiran/nero/nero
```

These commands install the published v0.1.3 binary. Unreleased source features
such as disk command history require a source build until a new release and
formula update. macOS binaries remain unsigned and unnotarized. The Homebrew
CI job installs, tests, reinstalls, retests and removes the actual formula.

## Ubuntu-compatible Debian packages

`scripts/release/package_deb.py` packages a native Linux amd64/arm64 ELF built
on the matching host. It checks architecture and CLI version, infers runtime
dependencies with `dpkg-shlibdeps`, includes license/build metadata and verifies
the extracted executable before writing a SHA-256 sidecar:

```sh
make -j2 NERO_RELEASE_VERSION=v0.0.0-ci
python3 scripts/release/package_deb.py --binary ./nero --version v0.0.0-ci
```

The Distribution packages workflow creates test-only `.deb` artifacts on
Ubuntu 24.04 amd64/arm64. On disposable runners it installs a first package,
builds and upgrades to a second version, checks the CLI version after each
installation, removes the package, and checks all sidecar hashes. These
artifacts are not a stable release or an APT repository. The Release packages
workflow separately builds reviewed-version `.deb` assets for releases after
v0.1.3 and verifies their real installation before draft creation. Never install CI
versions as a substitute for a reviewed public release.

Use `sudo apt install ./nero_VERSION_ARCH.deb` for a reviewed local package and
`sudo apt remove nero` to remove it. User history and application logs remain
in the user's profile. Packages built on Ubuntu 24.04 are not claimed to work
on every Debian release; dependencies and libc compatibility must match.

## Source and archive installation

POSIX archive installers honor `PREFIX` and `DESTDIR`; `uninstall.sh` honors
the same values and removes only the installed executable. Source checkouts
support `make install` and `make uninstall` with the same `PREFIX`, `DESTDIR`
and `INSTALL_DIR`. Archive upgrades stage the new executable before replacing
the destination, preserving the previous executable if staging fails.

New Windows archives include `uninstall.ps1`, accepting the installer's
`-InstallDir` and `-PathScope User|Machine|Process`. Machine removal requires
an Administrator PowerShell. The uninstaller removes `nero.exe` and its exact
PATH entry while retaining other files and user data. Older v0.1.3 archives
do not contain these new uninstallers; use the existing documented manual
removal steps for those archives. Published assets are not overwritten.

## Signing and publication prerequisites

Apple Developer ID/notarization needs the owner's Apple Developer account,
signing identity and notarization authorization. Windows Authenticode needs
an owner-controlled code-signing certificate or signing service. These
credentials have not been supplied; neither signing process is claimed as
complete. Never generate a self-signed substitute and present it as trusted.
APT repository hosting/signing and Homebrew Core submission are separate
publication steps. Native installer and archive distribution does not require
either service.
