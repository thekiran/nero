# Installation

<div align="center">

[**Download Source (.zip)**](https://github.com/thekiran/nero/archive/refs/heads/main.zip) &nbsp;·&nbsp;
[**Clone Repository**](https://github.com/thekiran/nero) &nbsp;·&nbsp;
[**GitHub Releases**](https://github.com/thekiran/nero/releases)

<sub>Versioned native packages for macOS, Linux and Windows are prepared by GitHub Actions.
Use a published release; drafts are visible only to collaborators.</sub>

</div>

<br>

Choose the native installer for your operating system below. Terminal commands
later in this guide are optional instructions for portable archives and developers.

### Recommended: standard operating-system installers

[v0.1.4](https://github.com/thekiran/nero/releases/tag/v0.1.4) includes native
installers for all five supported OS/architecture targets.

| System | Download | Installation |
| --- | --- | --- |
| macOS 15+, Apple Silicon | `nero-vX.Y.Z-macos-arm64.pkg` | Open in Finder and complete Installer |
| macOS 15+, Intel | `nero-vX.Y.Z-macos-x86_64.pkg` | Open in Finder and complete Installer |
| Windows 10/11, x86-64 | `nero-vX.Y.Z-windows-x86_64.msi` | Open the setup wizard and complete installation |
| Ubuntu-compatible, Intel/AMD | `nero_X.Y.Z_amd64.deb` | Open with the desktop's package/software installer |
| Ubuntu-compatible, ARM64 | `nero_X.Y.Z_arm64.deb` | Open with the desktop's package/software installer |

No compiler, shell profile edit or terminal installation command is needed.
Accept the normal administrator prompt when required. macOS installs the CLI
in `/usr/local/bin`; Linux packages install it in `/usr/bin`. Windows installs
into Program Files, registers system PATH, adds a Start menu shortcut and
appears in Installed apps for repair/removal. Restart existing Windows Terminal
processes once so they inherit the saved PATH.

After setup, open a terminal in **any folder** and run `nero`. macOS/Linux also
support `sudo nero` for operations needing administrator privileges. Windows
uses an Administrator terminal for elevation. Downloading or extracting a
portable archive alone does not register a global command; the following
manual instructions are for that optional distribution path.

macOS has no graphical uninstaller for `.pkg` files. The NERO package installs
`/usr/local/bin/nero` and `/usr/local/share/doc/nero` under the receipt
`com.thekiran.nero`; `pkgutil --files com.thekiran.nero` lists them. To remove
it, delete both paths and then forget the receipt:

```bash
sudo rm -f /usr/local/bin/nero
sudo rm -rf /usr/local/share/doc/nero
sudo pkgutil --forget com.thekiran.nero
```

Installing a newer `.pkg` over an older one upgrades in place. Command history
and logs are retained.

The installers currently have no Developer ID/notarization or Authenticode
signature. Operating-system publisher/security prompts can appear; no
security setting or execution policy is changed by these packages. Native
Linux packages need a compatible distribution and package-manager frontend.

### Prebuilt release packages

Open **[GitHub Releases](https://github.com/thekiran/nero/releases)** and select a
published version. Download the package matching your OS and CPU, plus its
`.sha256` file. Use the standard installers above for normal installation.

| Your system | Package |
| :--- | :--- |
| macOS 15+, Apple Silicon | `nero-vX.Y.Z-macos-arm64.tar.gz` |
| macOS 15+, Intel | `nero-vX.Y.Z-macos-x86_64.tar.gz` |
| Ubuntu 24.04 compatible, Intel/AMD | `nero-vX.Y.Z-linux-x86_64.tar.gz` |
| Ubuntu 24.04 compatible, ARM64 | `nero-vX.Y.Z-linux-arm64.tar.gz` |
| Windows 10/11, Intel/AMD | `nero-vX.Y.Z-windows-x86_64.zip` (native `nero.exe`) |

Verify the downloaded archive before extracting:

```bash
# macOS (replace the filename with your downloaded package)
shasum -a 256 -c nero-vX.Y.Z-macos-arm64.tar.gz.sha256
# Linux / WSL2
sha256sum -c nero-vX.Y.Z-linux-x86_64.tar.gz.sha256
```

Extract the selected archive. On macOS/Linux, run `sudo sh install.sh` inside
its extracted folder to install `nero` into `/usr/local/bin`. You can then run
`nero` or `sudo nero` from any directory. Linux packages require runtime libraries; macOS packages are
unsigned and use system frameworks. See the **[package installation guide](releases.md)**
for complete copyable commands, dependencies and supported OS versions.

### macOS — native

**Requirements:** macOS, Apple Command Line Tools, and Git.

<details>
<summary>First time on macOS? Install developer tools</summary>

Run this once in Terminal if the tools are missing:

```bash
xcode-select --install
```

</details>

**1. Download and build:**

```bash
git clone https://github.com/thekiran/nero.git && cd nero && make CC=clang -j4
```

**2. Install once, then run from any directory:**

```bash
sudo make install
nero
# For operations that require administrator privileges:
sudo nero
```

Works with the macOS native CLI. Some experimental network functions may need
additional permissions and may behave differently across OS versions.

### Linux — native

**Recommended:** Ubuntu or Debian-based distributions. Other Linux
distributions may require different dependency package names.

**1. Install build dependencies:**

```bash
sudo apt update && sudo apt install -y git build-essential python3 clang
```

**2. Download and build:**

```bash
git clone https://github.com/thekiran/nero.git && cd nero && make -j"$(nproc)"
```

**3. Install once, then run from any directory:**

```bash
sudo make install
nero
# For operations that require administrator privileges:
sudo nero
```

The Linux build includes experimental network modules. A successful build is
not a guarantee that every feature works in your environment.

### Windows — native x86-64

**Recommended:** download the `.msi` from [v0.1.4](https://github.com/thekiran/nero/releases/tag/v0.1.4),
open the setup wizard, and complete installation. Launch NERO from Start or run
`nero` from any folder in a fresh terminal. Use Installed apps to uninstall.

The following ZIP/script route is optional for advanced users.

Download `nero-vX.Y.Z-windows-x86_64.zip` and its `.sha256` file from a published
[release](https://github.com/thekiran/nero/releases). The ZIP contains a real
MinGW-w64 Windows CLI, statically linked to its compiler runtime; no MSYS2 or
WSL installation is needed to run the downloaded executable.

In PowerShell, replace the example version with your downloaded version:

```powershell
$package = "nero-vX.Y.Z-windows-x86_64"
$expected = ((Get-Content "$package.zip.sha256").Trim() -split '\s+')[0]
$actual = (Get-FileHash "$package.zip" -Algorithm SHA256).Hash
if ($actual -ne $expected) { throw "Checksum mismatch" }
Expand-Archive "$package.zip" -DestinationPath .
& ".\$package\install.ps1"
nero
```

The installer copies `nero.exe` to `%LOCALAPPDATA%\Programs\Nero\bin` and
adds that directory to your user PATH. Open a new terminal after installation;
`nero` then works from any directory. For all Windows users, run
`install.ps1 -PathScope Machine` from Administrator PowerShell; this uses
`%ProgramFiles%\Nero\bin` and the machine PATH. Windows elevation uses an
Administrator terminal; `sudo` is not a requirement of the native Windows CLI.
See [manual Windows installation](releases.md#native-windows-command-installation)
if your PowerShell script policy blocks the installer.

Use Windows Terminal / PowerShell for the interactive CLI. The binary is
unsigned. Native Windows supports the terminal, interface selection and TCP
code paths; the raw packet engine (`-Syn`) explicitly reports unsupported.
The release tests exercise offline CLI behavior and adapter selection, not
live scanning or raw traffic. Windows ARM64 has no native package.

To build from source in an **MSYS2 UCRT64** shell:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python
git clone https://github.com/thekiran/nero.git
cd nero
cmake -S . -B .build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
cmake --build .build/windows --parallel 4
ctest --test-dir .build/windows --output-on-failure
powershell.exe -NoProfile -File scripts/install.ps1 -BinaryPath .build/windows/nero.exe
nero
```

### Windows — optional WSL2 route

In Administrator PowerShell, run `wsl --install -d Ubuntu-24.04`, complete the
Ubuntu setup, then follow the Linux installation steps inside Ubuntu. Choose
the Linux package matching `uname -m`. WSL2 networking differs from native Linux.
See the [complete installation guide](releases.md#windows-11--wsl2).

### For developers

Prefer Git? This single command downloads the complete repository on any
system with Git installed:

```bash
git clone https://github.com/thekiran/nero.git
```

Prefer a ZIP? Use **[Download Source ZIP](https://github.com/thekiran/nero/archive/refs/heads/main.zip)**,
then extract it and follow the relevant platform's build steps.

For CMake, static analysis, sanitizers and test presets, consult the
[development and build guide](building.md).

### Automated releases

Version tags such as `v0.1.0` trigger native tests, ASan/UBSan checks, builds and
packaging for all five platforms. Each archive includes build metadata and a
SHA-256 sidecar; `SHA256SUMS` covers installers and archives. Tag builds and manual
runs default to drafts; an explicit publication request publishes only after all
native package checks pass. A reviewed change to `.github/release-intent.json`
on protected main can also request publication. See the [maintainer release procedure](releases.md#maintainer-release-procedure).
