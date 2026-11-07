# Troubleshooting and Operational Diagnostics

[Documentation Index](README.md) · [Installation Guide](installation.md) · [Command Reference](command-reference.md) · [Platform Support](platform-support.md)

This guide documents common operational issues, environment anomalies, diagnostic procedures, and verified solutions across Windows, Linux, and macOS.

---

## Windows Diagnostics and Environment Fixes

### 1. "Sistem belirtilen yolu bulamıyor" / Exit Code 1 on Session Launch

* **Symptom:** Every new SSH session, PowerShell execution, or command prompt window displays `Sistem belirtilen yolu bulamıyor.` (The system cannot find the path specified) before running any command, and exits with code `1`.
* **Root Cause:** An orphaned `AutoRun` entry remains in the Windows Command Processor registry. Whenever `cmd.exe` or an SSH process spawns, it attempts to execute a non-existent binary configured in `AutoRun`.
* **Resolution:**
  Open PowerShell and check if an `AutoRun` value is configured:
  ```powershell
  Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Command Processor' -Name AutoRun -ErrorAction SilentlyContinue
  ```
  If it references a missing path (for example, a removed banner utility or stale script), delete the property:
  ```powershell
  Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Command Processor' -Name AutoRun -ErrorAction SilentlyContinue
  ```
  Verify that `cmd.exe /c echo OK` now returns `OK` with exit code `0`.

---

### 2. Empty PATH / Standard Commands Not Found in OpenSSH Sessions

* **Symptom:** Running naked commands like `hostname`, `powershell`, or `gcc` fails with `'<command>' is not recognized as an internal or external command`, but specifying full paths (e.g., `C:\Windows\System32\HOSTNAME.EXE`) works.
* **Root Cause:** Total combined length of System PATH and User PATH exceeds the Win32 environment buffer threshold (typically 2,048 or 4,096 characters). This occurs when installers or scripts repeatedly append duplicates of the system path into the user profile. OpenSSH discards or truncates the invalid environment block upon session token creation.
* **Diagnosis:**
  ```powershell
  $sys = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').Path
  $usr = (Get-ItemProperty 'HKCU:\Environment').Path
  Write-Host "System PATH length: $($sys.Length), User PATH length: $($usr.Length)"
  ```
  If the combined length exceeds 2,048 characters, Windows fails to expand `%PATH%` reliably.
* **Resolution:**
  1. Export a safety backup:
     ```powershell
     @{
         System = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').Path
         User = (Get-ItemProperty 'HKCU:\Environment').Path
     } | ConvertTo-Json | Set-Content "$env:USERPROFILE\path_backup.json"
     ```
  2. Deduplicate entries and remove non-existent directories.
  3. Keep machine-wide paths in System PATH and user-specific toolchains (such as MSYS2 and Nero) in User PATH.
  4. Ensure `C:\Users\<User>\AppData\Local\Programs\Nero\bin` is listed in your User PATH.

---

### 3. PowerShell Script Execution Policy Blocks Installer

* **Symptom:** Running `.\scripts\install.ps1` produces `File ... cannot be loaded because running scripts is disabled on this system`.
* **Resolution:**
  Bypass the execution policy for the installer process only:
  ```powershell
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\install.ps1 -BinaryPath .build\windows\nero.exe
  ```
  Or install manually by copying `nero.exe` to `%LOCALAPPDATA%\Programs\Nero\bin` and adding that directory to your user environment.

---

### 4. MSYS2 UCRT64 Build Toolchain Setup

* **Symptom:** `cmake` reports `No C compiler could be found` or Ninja is unavailable when building native Windows sources.
* **Resolution:**
  Ensure you install the UCRT64 packages inside MSYS2:
  ```bash
  pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc \
      mingw-w64-ucrt-x86_64-cmake \
      mingw-w64-ucrt-x86_64-ninja \
      mingw-w64-ucrt-x86_64-make
  ```
  Add `C:\msys64\ucrt64\bin` to your User PATH.

---

## Linux Runtime and Compilation

### 1. Missing Development Libraries during Make

* **Symptom:** Build fails with errors such as `curl/curl.h: No such file or directory` or `cjson/cJSON.h: No such file or directory`.
* **Resolution:**
  Install all native development libraries on Ubuntu/Debian:
  ```bash
  sudo apt update && sudo apt install -y build-essential libcurl4-openssl-dev \
      libcjson-dev libldns-dev libncurses-dev python3 clang
  ```

---

### 2. "Operation not permitted" (EPERM) on Packet Engine

* **Symptom:** Invoking `-Syn` outputs an error indicating socket creation failed.
* **Root Cause:** Opening raw IP sockets (`SOCK_RAW`) requires root privileges or Linux capabilities.
* **Resolution:**
  Run NERO with administrator privileges:
  ```bash
  sudo nero
  ```
  Or grant the `CAP_NET_RAW` capability to the binary:
  ```bash
  sudo setcap cap_net_raw+ep /usr/local/bin/nero
  ```

---

## macOS Operational Diagnostics

### 1. Gatekeeper Quarantine on Downloaded Package

* **Symptom:** macOS displays `"nero" cannot be opened because the developer cannot be verified` or silently terminates execution.
* **Root Cause:** Downloaded release archives carry the macOS quarantine extended attribute (`com.apple.quarantine`).
* **Resolution:**
  Remove the quarantine attribute from the extracted binary:
  ```sh
  xattr -d com.apple.quarantine /usr/local/bin/nero
  ```

---

### 2. Developer Tools Not Installed

* **Symptom:** `make` or `clang` command not found on a fresh macOS installation.
* **Resolution:**
  Install the official Command Line Tools:
  ```sh
  xcode-select --install
  ```

---

## Terminal State and Display Recovery

### 1. Terminal Left in Raw Mode / Keystrokes Not Displayed

* **Symptom:** If NERO is terminated abnormally (e.g., killed with `SIGKILL` or unhandled debugger pause), your shell does not show typed characters or produces distorted carriage returns.
* **Root Cause:** Interactive terminal sessions switch stdin to raw mode (`ICANON` and `ECHO` disabled). Normal exit restores original flags via signal traps, but hard process termination bypasses userspace cleanup.
* **Resolution:**
  * **On POSIX (macOS / Linux):** Type `reset` or `stty sane` and press Enter:
    ```sh
    stty sane
    ```
  * **On Windows:** Close the current terminal tab or run `cmd.exe /c echo off && echo on`.

---

### 2. Garbled ANSI Sequences or Color Distortions

* **Symptom:** Escape codes like `\033[38;5;...` are displayed literally instead of rendering styled colors.
* **Resolution:**
  * Ensure your terminal emulator supports ANSI / VT100 standards (e.g., macOS Terminal, iTerm2, Windows Terminal, Kitty, Alacritty).
  * If operating in an automated log or dumb terminal, export standard environment flags:
    ```sh
    export NO_COLOR=1
    export TERM=dumb
    ```
    NERO detects `NO_COLOR` and `TERM=dumb` and automatically falls back to plain ASCII rendering without ANSI styling.
