<!-- nero-native-installers -->
## Install NERO

Download the installer for your system, open it and complete the normal installation screens. No compiler, terminal setup command or shell profile edit is needed.

| System | Installer |
| --- | --- |
| macOS 15+, Apple Silicon (M-series) | [Download .pkg](https://github.com/thekiran/nero/releases/download/{version}/nero-{version}-macos-arm64.pkg) |
| macOS 15+, Intel | [Download .pkg](https://github.com/thekiran/nero/releases/download/{version}/nero-{version}-macos-x86_64.pkg) |
| Windows 10/11 x86-64 | [Download .msi](https://github.com/thekiran/nero/releases/download/{version}/nero-{version}-windows-x86_64.msi) |
| Ubuntu-compatible Linux, Intel/AMD | [Download .deb](https://github.com/thekiran/nero/releases/download/{version}/nero_{deb_version}_amd64.deb) |
| Ubuntu-compatible Linux, ARM64 | [Download .deb](https://github.com/thekiran/nero/releases/download/{version}/nero_{deb_version}_arm64.deb) |

After setup, open a terminal in any folder and run `nero`. macOS/Linux also support `sudo nero` for privileged operations. Windows adds NERO to Start and system PATH; fully restart any existing Windows Terminal process once to receive the new environment.

Portable archives remain available as an advanced option. All ten payloads have SHA-256 sidecars and are covered by `SHA256SUMS`.

macOS installers are unsigned/unnotarized; Windows installers are unsigned. OS publisher/security prompts are possible. No execution policy or security setting is changed. Experimental network functions are for authorized laboratories; Windows does not include the raw packet engine.

Source commit: `{commit}`.

<!-- generated-changelog -->
