# Release and installation guide

## Packages

The [Releases page](https://github.com/thekiran/nero/releases) contains published
versions. Drafts are visible only to repository collaborators. Until the first
draft is reviewed and published, use the source installation in the README.

| Asset suffix | Native build host / minimum supported environment |
| --- | --- |
| `macos-arm64.tar.gz` | Apple Silicon, macOS 15 or newer |
| `macos-x86_64.tar.gz` | Intel, macOS 15 or newer |
| `linux-x86_64.tar.gz` | x86-64, Ubuntu 24.04 (glibc 2.39) or compatible |
| `linux-arm64.tar.gz` | ARM64, Ubuntu 24.04 (glibc 2.39) or compatible |

These are dynamically linked native Make builds, not static universal binaries.
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
linked libraries. The CLI's built-in version text is unchanged; the release
version is recorded in the manifest and archive name.

## Download, verify and install

Download the archive for your CPU and its matching `.sha256` file from the same
release into an empty directory. Replace the example version and platform below
with the selected release. Verify before extracting.

macOS example (Apple Silicon):

```bash
VERSION=v0.1.0
PACKAGE="nero-$VERSION-macos-arm64"
shasum -a 256 -c "$PACKAGE.tar.gz.sha256"
tar -xzf "$PACKAGE.tar.gz"
mkdir -p "$HOME/.local/bin"
install -m 755 "$PACKAGE/bin/nero" "$HOME/.local/bin/nero"
"$HOME/.local/bin/nero"
```

Linux example (x86-64; use `linux-arm64` for ARM64):

```bash
VERSION=v0.1.0
PACKAGE="nero-$VERSION-linux-x86_64"
sha256sum -c "$PACKAGE.tar.gz.sha256"
tar -xzf "$PACKAGE.tar.gz"
mkdir -p "$HOME/.local/bin"
install -m 755 "$PACKAGE/bin/nero" "$HOME/.local/bin/nero"
"$HOME/.local/bin/nero"
```

Add `$HOME/.local/bin` to your shell's PATH to run `nero` by name. Alternatively,
install system-wide with `sudo install -m 755 ... /usr/local/bin/nero`. Remove the
installed file to uninstall. `SHA256SUMS` verifies all four archives together;
per-archive `.sha256` files are convenient when downloading just one platform.
Checksums detect damaged downloads; they are not a separate code signature.

## Windows 11 / WSL2

No native Windows CLI package or `.exe` is released. The CMake Windows foundation
is not an equivalent application. In an Administrator PowerShell:

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
./nero
```

## Maintainer release procedure

The authoritative application target is `make`. CMake provides separate offline
regression targets; `nero-safe` and `nero-platform-info` are never distributed as
the CLI. Existing CI and static-analysis workflows remain in place.

1. Merge the release workflow and documentation into `main`.
2. On a clean checkout of the intended commit, create and push a version tag:

   ```bash
   git tag -a v0.1.0 -m "NERO v0.1.0"
   git push origin v0.1.0
   ```

   Alternatively, for the first release open **Actions → Release packages → Run
   workflow**, select `main`, and enter `v0.1.0`. If the tag does not exist, the
   workflow builds that main commit and creates the tag only after all four
   platforms pass. No release is published automatically.

3. Watch **Actions → Release packages**. Four native runners execute Make tests,
   Make ASan/UBSan tests, static analysis, CMake tests and CMake sanitizer tests.
   A failing check prevents package publication. CTest logs are retained as
   artifacts, including on failure; Make test output is in the job logs.
4. Each runner builds the real CLI, checks its architecture and dependencies,
   verifies executable contents in the archive, and generates a SHA-256 sidecar.
5. The final job requires all four successful builds, checks all hashes, generates
   `SHA256SUMS`, then creates a **draft** release. Nothing is automatically made
   public. Review the four archives, sidecars, checksum manifest and notes, then
   publish manually.

Tags support `vMAJOR.MINOR.PATCH` and prerelease suffixes such as `v0.2.0-rc.1`.
Prerelease tags are also marked as prereleases on GitHub. For a failed run, rerun
the workflow or use **Run workflow** with the same tag. A rerun may replace
assets only while the release remains a draft; published assets are never
overwritten. Tag resolution is frozen to one commit for every matrix job.
Do not move a published tag; make a new version instead.

On pull requests touching build, source, tests or release files, the same four
platforms build test packages (`v0.0.0-ci`) without creating a release or granting
write permission. Release creation alone receives `contents: write`. Third-party
actions are pinned to commit hashes. No personal access token is required.

CI exercises offline fixtures and mocks. It compiles the live network modules
but does not validate their runtime behavior or generate live network traffic.
