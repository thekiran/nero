# Release and installation guide

## Standard graphical installers (v0.1.4 and later)

For normal installation, choose the native installer offered by the release:

| System | Installer | Registers |
| --- | --- | --- |
| macOS 15+ ARM64/Intel | `nero-vX.Y.Z-macos-ARCH.pkg` | `/usr/local/bin/nero` through Apple Installer |
| Windows 10/11 x86-64 | `nero-vX.Y.Z-windows-x86_64.msi` | Program Files, system PATH, Start menu and Installed apps |
| Ubuntu-compatible amd64/arm64 | `nero_X.Y.Z_ARCH.deb` | `/usr/bin/nero` and an application menu entry through the package manager |

Open the file and complete the operating system's normal installation screens.
No developer tools, shell configuration or terminal installation commands are
required. Existing Windows Terminal processes must be restarted to inherit
the updated PATH. Afterwards `nero` works from any folder; POSIX systems also
support `sudo nero` for privileged operations. Windows elevation uses an
Administrator terminal. MSI upgrades/removal are managed by Windows Installer
and remove only NERO's PATH entry, leaving other entries and user data intact.

A newer macOS `.pkg` upgrades an older one in place. macOS has no `.pkg`
uninstaller; remove the payload recorded by the `com.thekiran.nero` receipt,
then forget the receipt (user history and logs are retained):

```bash
sudo rm -f /usr/local/bin/nero
sudo rm -rf /usr/local/share/doc/nero
sudo pkgutil --forget com.thekiran.nero
```

Native installers and portable archives each have a `.sha256` sidecar and
are included in `SHA256SUMS`. There are five of each across the native matrix.
macOS installers are unsigned/unnotarized and Windows installers are unsigned;
publisher/security prompts are possible. No security or execution policy is
changed. Published v0.1.3 assets remain unchanged and have only portable archives.
The remaining sections describe that optional advanced installation path.

## Packages

[NERO v0.1.4](https://github.com/thekiran/nero/releases/tag/v0.1.4) is publicly
available with five native installers, five portable archives and SHA-256 checksums. Older v0.1.0–v0.1.2
drafts remain visible only to repository collaborators. Source installation is
also available in the README.

| Asset suffix | Native build host / minimum supported environment |
| --- | --- |
| `macos-arm64.tar.gz` | Apple Silicon, macOS 15 or newer |
| `macos-x86_64.tar.gz` | Intel, macOS 15 or newer |
| `linux-x86_64.tar.gz` | x86-64, Ubuntu 24.04 (glibc 2.39) or compatible |
| `linux-arm64.tar.gz` | ARM64, Ubuntu 24.04 (glibc 2.39) or compatible |
| `windows-x86_64.zip` | Windows 10/11 x86-64, native MinGW-w64 CLI |

macOS/Linux packages are dynamically linked native Make builds, not static universal binaries.
Windows uses CMake with MinGW-w64 and statically links the compiler runtime;
only Windows system DLLs are allowed by the package import check.
macOS uses system frameworks; no Homebrew runtime is required. macOS packages
are not Developer ID signed or notarized. Linux runtime dependencies can be
installed on Ubuntu 24.04 with:

```bash
sudo apt update
sudo apt install -y libcurl4t64 libcjson1 libldns3 libncurses6
```

For other distributions or older systems, use a source build. Every archive
contains `bin/nero`, `LICENSE.txt`, `README.md`, `INSTALL.md` and `BUILD-INFO.json`.
The build manifest records the version, source commit, binary hash and actual
linked libraries. The CLI reports the same version tag as the release archive and `BUILD-INFO.json`.
Unversioned source builds report `version development`.

## Download, verify and install

Download the archive for your CPU and its matching `.sha256` file from the same
release into an empty directory. Replace the example version and platform below
with the selected release. Verify before extracting.

macOS example (Apple Silicon):

```bash
VERSION=v0.1.4
PACKAGE="nero-$VERSION-macos-arm64"
shasum -a 256 -c "$PACKAGE.tar.gz.sha256"
tar -xzf "$PACKAGE.tar.gz"
sudo sh "$PACKAGE/install.sh"
nero
# Administrator privileges when needed, from any directory:
sudo nero
```

Linux example (x86-64; use `linux-arm64` for ARM64):

```bash
VERSION=v0.1.4
PACKAGE="nero-$VERSION-linux-x86_64"
sha256sum -c "$PACKAGE.tar.gz.sha256"
tar -xzf "$PACKAGE.tar.gz"
sudo sh "$PACKAGE/install.sh"
nero
# Administrator privileges when needed, from any directory:
sudo nero
```

The default system installation directory is `/usr/local/bin`, available to
normal shells and the standard sudo PATH on macOS/Ubuntu. No shell alias or
working directory is required. For custom systems where that directory is not
in sudo's configured PATH, use `sudo /usr/local/bin/nero`; no sudo configuration
is changed by this installer. Uninstall with `sudo rm /usr/local/bin/nero`.
A source checkout uses `make` followed by `sudo make install`. Packaging tools
can stage files with `make install DESTDIR=/path/to/staging PREFIX=/usr/local`.
`INSTALL_DIR` remains available as an explicit override.

`SHA256SUMS` verifies all ten release payloads together;
per-archive `.sha256` files are convenient when downloading just one platform.
Checksums detect damaged downloads; they are not a separate code signature.

## Native Windows command installation

After checksum verification and extraction, run `install.ps1` in the extracted
folder. The default installs for the current user and adds the directory to the
persistent user PATH and the current PowerShell session. New terminal windows
can run `nero` from any directory. The ZIP includes this installer.

For all users, open Administrator PowerShell and run
`.\install.ps1 -PathScope Machine`. The default destination is
`%ProgramFiles%\Nero\bin`; the installer updates machine PATH.
Windows does not require a `sudo` command: open Terminal as Administrator when
elevated permissions are needed. WSL2 uses the Linux `sudo nero` installation.

If PowerShell policy prevents running a downloaded script, these manual commands
perform the user installation without changing the execution policy. Run them
inside the extracted package folder:

```powershell
$bin = Join-Path $env:LOCALAPPDATA 'Programs\Nero\bin'
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item .\bin\nero.exe (Join-Path $bin 'nero.exe') -Force
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = @($userPath -split ';' | Where-Object { $_ })
if ($entries -notcontains $bin) {
    [Environment]::SetEnvironmentVariable('Path', (($entries + $bin) -join ';'), 'User')
}
$env:Path = "$bin;$env:Path"
nero
```

To uninstall, remove the installed `nero.exe` and remove its directory from your
user or machine PATH using Windows Environment Variables settings.

## Windows 11 / WSL2

A native Windows x86-64 ZIP is available as described in the README. It contains
`bin/nero.exe`, build metadata and the same documentation and license files.
The extracted binary is smoke-tested with a Windows system-only PATH to ensure
it runs without MSYS2 DLLs. It is unsigned. The Visual Studio CMake preset
remains foundation-only; native CLI builds use MinGW-w64.

WSL2 remains an alternative, especially when the Linux environment is preferred. In an Administrator PowerShell:

```powershell
wsl --install -d Ubuntu-24.04
wsl --list --verbose
```

Restart if requested and complete the Ubuntu account setup. Use Windows Terminal
with the Ubuntu profile. Inside Ubuntu, check `uname -m`: `x86_64` selects the
Linux x86-64 package, `aarch64` selects ARM64. Install the Linux runtime libraries
above, then follow the Linux download and verification steps. Keep the executable
in the Linux home directory. WSL2 networking and raw-socket behavior can differ
from a native Linux installation.

For a source build inside Ubuntu:

```bash
sudo apt update
sudo apt install -y git build-essential libcurl4-openssl-dev libcjson-dev libldns-dev libncurses-dev python3 clang cmake
git clone https://github.com/thekiran/nero.git
cd nero
make -j4
make test
make test-sanitize
cmake --preset linux
cmake --build --preset linux --parallel 4
ctest --preset linux
sudo make install
nero
```

## Maintainer release procedure

The authoritative application target is `make`. CMake provides separate offline
regression targets; `nero-safe` and `nero-platform-info` are never distributed as
the CLI. Existing CI and static-analysis workflows remain in place. Static analysis is
reported by its own workflow; it is not a release-package gate.

1. Merge the release workflow and documentation into `main`.
2. On a clean checkout of the intended commit, create and push a version tag:

   ```bash
   git tag -a v0.1.0 -m "NERO v0.1.0"
   git push origin v0.1.0
   ```

   Alternatively, for the first release open **Actions → Release packages → Run
   workflow**, select `main`, and enter `v0.1.0`. If the tag does not exist, the
   workflow builds that main commit and creates the tag only after all five
   platforms pass. Manual runs default to a draft; enable the explicit `publish` input only when public publication is intended.

3. Watch **Actions → Release packages**. Four native runners execute Make tests,
   Make ASan/UBSan tests, CMake tests and CMake sanitizer tests.
   A failing check prevents package publication. CTest logs are retained as
   artifacts, including on failure; Make test output is in the job logs.
4. Each macOS/Linux runner builds the real CLI, checks its architecture and dependencies,
   verifies executable contents in the archive, and generates a SHA-256 sidecar.
   Windows additionally runs its native CLI tests (commands, history, EOF,
   bounded input, unsupported raw engine) and local interface selection tests,
   checks the PE architecture and DLL imports, and tests the extracted ZIP.
   POSIX PTY and ASan/UBSan suites run on macOS/Linux; they are not claimed for
   the native Windows toolchain.

5. The final job requires all five successful builds, checks all hashes, generates
   `SHA256SUMS`, then creates a **draft** release. Draft-only runs remain private. Explicit publication requests publish only after native installers, portable archives and aggregate checksums have passed. Review the source and publication intent through a PR before requesting this mode.

Tags support `vMAJOR.MINOR.PATCH` and prerelease suffixes such as `v0.2.0-rc.1`.
Prerelease tags are also marked as prereleases on GitHub. For a failed run, rerun
the workflow or use **Run workflow** with the same tag. A rerun may replace
assets only while the release remains a draft; published assets are never
overwritten. Tag resolution is frozen to one commit for every matrix job.
Do not move a published tag; make a new version instead.

On pull requests touching build, source, tests or release files, the same five
platforms build test packages (`v0.0.0-ci`) without creating a release or granting
write permission. Release creation alone receives `contents: write`. Third-party
actions are pinned to commit hashes. No personal access token is required.

CI exercises offline fixtures and mocks. It compiles the live network modules
but does not validate their runtime behavior or generate live network traffic.

## Explicit release intent

A protected-main PR can request a release by changing `.github/release-intent.json`.
Its `version` must be a valid new tag, `publish` controls draft/public status,
and an optional `pull_request` identifies the same-repository merged release
branch for cleanup. The current intent requests public v0.1.4 after all five
native installer and archive checks plus aggregate SHA-256 verification pass.
Pull request events always build test-only v0.0.0-ci artifacts and cannot
publish. Tag builds and manual runs default to drafts unless publication is
explicitly requested. Non-PR release runs are serialized, published assets
are never overwritten, and an existing tag cannot be moved by a main intent.
Cleanup only deletes an already-merged feature branch still at its reviewed
PR head; new work and main are retained. Native-installer download links and
signing status are added to release notes before publication.
