# NERO Command Reference

[Documentation Index](README.md) · [Architecture](architecture.md) · [Platform Support](platform-support.md) · [Testing](testing.md)

NERO provides a native interactive terminal shell written in C11. This document defines all available interactive commands, diagnostic flags, syntax rules, platform constraints, and execution boundaries.

---

## Command Discovery

Inside the interactive terminal, you can list all registered commands at any time:

```text
► help
```

To display full usage help with detailed parameter notes:

```text
► help -All
```

---

## Core Terminal Commands

The core commands control session state, screen rendering, and input history. They are compiled into all platform targets (macOS, Linux, and Windows) and do not initiate any network I/O.

### `help`
* **Syntax:** `help` or `help -All`
* **Description:** Displays the banner, usage guidelines, and the catalog of available commands.
* **Platforms:** macOS, Linux, Windows.
* **Exit status:** Remains in terminal session (`0`).

### `version`
* **Syntax:** `version`
* **Description:** Prints the current version string (`version 1.0`).
* **Platforms:** macOS, Linux, Windows.
* **Output:**
  ```text
  version 1.0
  ```

### `history`
* **Syntax:** `history`
* **Description:** Displays chronological command history for the active session. History entries are stored in-memory during the session and written on clean exit.
* **Platforms:** macOS, Linux, Windows.

### `history -clear`
* **Syntax:** `history -clear`
* **Description:** Clears all command history recorded in the active session.
* **Platforms:** macOS, Linux, Windows.

### `clear`
* **Syntax:** `clear`
* **Description:** Emits ANSI/VT escape sequences (`\033[H\033[J` on POSIX, native VT console sequences on Windows 10/11) to clear the terminal viewport and move the cursor to the origin.
* **Platforms:** macOS, Linux, Windows.

### `exit`
* **Syntax:** `exit`
* **Description:** Exits the interactive shell, restoring the terminal to its original state (restores `termios` flags on POSIX; restores full console input/output modes and code page on Windows).
* **Platforms:** macOS, Linux, Windows.

### `exit -All`
* **Syntax:** `exit -All`
* **Description:** Clears the viewport, prints the exit notice, restores original console modes, and terminates the interactive shell.
* **Platforms:** macOS, Linux, Windows.

---

## Network Diagnostics Commands

Network diagnostics commands examine session configuration and perform controlled TCP reachability checks.

### `-mode`
* **Syntax:** `-mode` or `<target> -mode`
* **Description:** Displays the current diagnostic status of the interactive session.
* **Platforms:** macOS, Linux, Windows.
* **Output:**
  ```text
  Network diagnostics: ON
  ```
  *(or `Network diagnostics: OFF`)*

### `-mode on` / `-mode off`
* **Syntax:** `-mode on` | `-mode off`
* **Description:** Enables or disables network diagnostics for subsequent port-checking routines.
* **Platforms:** macOS, Linux, Windows.

### `<target> -Sc`
* **Syntax:** `<target> -Sc`
* **Description:** Runs TCP port scanner diagnostics against the specified IPv4 target address for standard service ports (e.g., 21, 22, 23, 25, 53, 80, 110, 143, 443, 3306, 3389, 8080).
* **Execution behavior:**
  * **Live production builds (macOS, Linux, Windows):** Performs standard non-blocking `connect()` handshakes with timeout limits (1 second per port).
  * **Offline test builds (`NERO_SAFE_TEST` / `NERO_NETWORK_UNAVAILABLE`):** Simulates port state in memory without opening remote network sockets.
* **Platforms:** macOS, Linux, Windows (native MinGW).
* **Restrictions:** Target must be a valid IPv4 address in dotted-decimal notation.

---

## Experimental Packet Engine

> [!WARNING]
> The raw packet engine is experimental software designed exclusively for isolated research laboratories and explicitly authorized testing environments. Generating unsolicited network traffic against unauthorized third-party networks or systems is strictly prohibited. See [SECURITY.md](../SECURITY.md) and [laboratory-safety.md](laboratory-safety.md).

### `<network IP> -Syn`
* **Syntax:** `<network IP> -Syn` or `-Syn <network IP>`
* **Description:** Initiates raw TCP/UDP packet transmission routines against the specified target.
* **Platform Availability & Runtime Requirements:**
  * **Linux (native):** Supported in live Make builds. Requires `root` privileges (`sudo nero`) and `CAP_NET_RAW` capability to open `AF_INET, SOCK_RAW` sockets.
  * **macOS (native):** Supported in live Make builds. Requires `root` privileges (`sudo nero`) for Berkeley Packet Filter (BPF) and raw IP socket access.
  * **Windows (native):** **Explicitly unavailable** (`NERO_SYN_UNAVAILABLE`). The Windows desktop networking subsystem does not permit arbitrary raw TCP/UDP injection from non-driver user-space applications. Executing `-Syn` on Windows outputs:
    ```text
    -Syn: unsupported in this build (no network traffic is generated).
    ```
  * **CI & Offline Builds:** Excluded via `NERO_SAFE_TEST`. The offline mock handler reports syntax validity without transmitting traffic.

---

## Terminal Input and Line Editing

NERO provides a responsive line editor that handles key sequences across platforms:

| Key Sequence | Action |
| --- | --- |
| `Enter` (`\n`, `\r`) | Execute current command line |
| `Ctrl+D` (`EOF`) | Clean exit when prompt buffer is empty |
| `Ctrl+C` (`SIGINT`) | Cancel current input line, display fresh prompt |
| `Up Arrow` (`\033[A`) | Navigate to previous command in history |
| `Down Arrow` (`\033[B`) | Navigate to next command in history |
| `Left Arrow` (`\033[D`) | Move cursor one position left |
| `Right Arrow` (`\033[C`) | Move cursor one position right |
| `Backspace` (`\x7f`, `\x08`) | Delete character preceding cursor |
| `Delete` (`\033[3~`) | Delete character at cursor |

### Tokenization and Limits
* **Maximum command line length:** 4096 bytes. Input exceeding this threshold produces a `Command too long` warning and is rejected safely without buffer overflow.
* **Maximum token count:** 64 tokens (`CLI_MAX_TOKENS`). Commands exceeding 64 tokens are rejected cleanly.
* **Unrecognized commands:** Commands not matching the catalog report `Unknown command: <token>` and exit code `0` to keep the shell interactive.

---

## Command Support Matrix

| Command | macOS (ARM64/Intel) | Linux (x86-64/ARM64) | Windows (Native x86-64) | Offline CI / Tests |
| --- | :---: | :---: | :---: | :---: |
| `help`, `help -All` | Supported | Supported | Supported | Supported |
| `version` | Supported | Supported | Supported | Supported |
| `history`, `history -clear` | Supported | Supported | Supported | Supported |
| `clear` | Supported | Supported | Supported | Supported |
| `exit`, `exit -All` | Supported | Supported | Supported | Supported |
| `-mode`, `-mode on/off` | Supported | Supported | Supported | Supported |
| `<target> -Sc` (TCP Diagnostics) | Supported | Supported | Supported | Simulated |
| `<target> -Syn` (Raw Flood Engine) | Supported (`sudo`) | Supported (`sudo`) | **Unavailable** | Simulated |


## Application logs

| Command | Behavior |
| --- | --- |
| `log` | Show the latest 256 application events, including previous sessions when persistence is available |
| `log -errors` | Show warnings and errors from the same recent history |
| `log -path` | Show the active file path or the reason for memory-only fallback |
| `log -clear` | Clear the recent history, current log file and rotated backup |

After a scan, enter `log -errors` to inspect grouped scanner errors with target,
operation, OS error code/message and affected port count. Socket failures are not
mistaken for closed ports. Scan start/end summaries, terminal lifecycle/errors,
input validation warnings and the selected-port TCP check are recorded. This is
explicit application logging, not interception of arbitrary stdout/stderr from
other modules. Raw command lines are omitted. Target IPs appear in diagnostics.
Timestamps are UTC. Network traffic is never generated by the `log` commands.

| Platform | Default persistent file |
| --- | --- |
| macOS | `~/Library/Logs/Nero/nero.log` |
| Linux | `$XDG_STATE_HOME/nero/nero.log`, or `~/.local/state/nero/nero.log` |
| Windows | `%LOCALAPPDATA%/Nero/Logs/nero.log` |

Each file is capped at 1 MiB with one rotated `.1` backup. The CLI displays the
latest 256 records. `NERO_LOG_FILE` overrides the path; `NERO_LOG_FILE=:memory:`
disables disk persistence. POSIX files/directories are created with modes
0600/0700. A write/permission failure leaves an in-memory log and is shown by
`log`/`log -path`; it never prevents scanning. Records are serialized between
threads within one process. Rotation across independent concurrent NERO
processes is not coordinated.

Logging is not async-signal-safe and is called only outside signal handlers.
Logs reflect events after this logging-enabled build starts; earlier terminal
output cannot be recovered retroactively. Old severity-labelled records are
loaded on restart, bounded to recent data. Control characters are sanitized
before storage/display to avoid injecting terminal escape sequences.
