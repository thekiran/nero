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
* **Description:** Prints `version development` in source builds, or the package tag (such as `version v0.1.3`) in release builds.
* **Platforms:** macOS, Linux, Windows.
* **Output:**
  ```text
  version development
  ```

### `history`
* **Syntax:** `history`
* **Description:** Displays the latest 256 entries (`CLI_HISTORY_CAPACITY`). New source builds after v0.1.3 restore recognized commands from private disk history in interactive sessions. Published v0.1.3 packages still have session-only history. See persistence details below.
* **Platforms:** macOS, Linux, Windows.

### `history -clear`
* **Syntax:** `history -clear`
* **Description:** Clears session history and, when enabled, persistent history. A disk failure is reported explicitly; the CLI does not claim persistent clearing succeeded.
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

## Offline Workload Simulation

### `sim`
* **Syntax:** `sim`
* **Classification:** **OFFLINE SIMULATION.** This screen performs **zero network I/O**. It opens no sockets, resolves no names, sends no packets, and never reaches the legacy `-Syn` engine, `THREAD_COUNT`, `portscanner()`, or `scan_session.c`.
* **Description:** Reads basic **local** hardware facts using read-only native APIs — POSIX `sysctl`/`sysconf` (`/proc/meminfo` on Linux) and `GetSystemInfo`/`GlobalMemoryStatusEx` on Windows — and presents a conservative, documented simulation-only profile recommendation. The user always makes the final selection; nothing is auto-selected or auto-started.
* **Detected data:** Logical processor count, physical core count (where the platform exposes it), total RAM, and available RAM (where exposed). Values that cannot be measured are shown as `Unavailable` rather than guessed. Displayed limitations note that logical cores differ from physical cores and that container/virtualization quotas can make host totals misleading.
* **Recommendation rule (deterministic):**
  * CPU or total RAM unavailable → **no recommendation** (manual selection only).
  * `cpu_logical <= 2` **or** total RAM `<= 4 GiB` → **Low**.
  * Otherwise → **Balanced**.
* **Menu:** `[1] Low`, `[2] Balanced`, `[3] Custom`, `[0] Cancel`. The suggested profile is marked `(Recommended)`. `Custom` accepts a simulated worker count (`1`–`512`) and duration in seconds (`1`–`60`); out-of-range, signed, blank, or non-numeric input is rejected, never clamped silently.
* **Simulation output:** Reports simulated worker/duration values, an explicit `Packets generated: 0` / `Sockets opened: 0`, and a note that the live `-Syn` engine is unchanged. The numbers are display data only and are **not** a safe real-network transmission rate.
* **Platforms:** macOS, Linux, Windows.
* **Exit status:** Remains in terminal session (`0`); `Ctrl+C`, `Esc`, `q`, or `0` cancel without selecting a profile.

### `preflight`
* **Syntax:** `preflight`
* **Classification:** **READ-ONLY.** Opens no sockets, sends no packets, resolves no names, starts no subprocess, and does not escalate privileges. It never enters or alters the `-Syn` execution path.
* **Description:** Reports what the experimental `-Syn` engine in **this build** would attempt, so a user can understand the expected resource use before deciding to run it. It does not configure anything — there is no way to change the worker count, packet rate, or packet volume from this screen.
* **Reported facts:**
  * Whether the live `-Syn` engine is **compiled into this target**. Offline and safe builds state plainly that live raw mode is **unsupported** and that no workers, sockets, or packets can be created.
  * The fixed, compile-time worker count (`NERO_SYN_THREAD_COUNT`, currently `512`), described as an **upper bound, never a guarantee**. Thread and raw-socket creation can fail, so the actual number of workers is only known at run time and is reported by the engine itself.
  * The raw-socket privilege requirement (root / `CAP_NET_RAW`) and a **read-only** check (`geteuid()`) of whether the current process is privileged.
  * Qualified upper-bound estimates for threads, raw sockets per worker, and file descriptors (`512 × 2 + 64`, raised via `setrlimit` only when permitted).
* **Not throughput:** Every figure is an attempt ceiling, explicitly **not** measured throughput and **not** a safe network transmission rate.
* **Safety:** Links to [laboratory-safety.md](laboratory-safety.md).
* **Platforms:** macOS, Linux, Windows (renders on all CLI targets; identifies unsupported live raw modes clearly).
* **Exit status:** Remains in terminal session (`0`); `[1]` Return, `[0]` Cancel, or `Ctrl+C`/`Esc`/`q` leave the screen without starting anything.

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
* **Description:** Runs TCP connect diagnostics across ports 1–65535 on the specified IPv4 target. Discovery does not gate the scan. Results distinguish open, closed, filtered, error/unknown and unscanned ports.
* **Execution behavior:**
  * **Live production builds (macOS, Linux, Windows):** Uses nonblocking `connect()` with per-socket deadlines. First-pass/retry timeouts are 500/1000 ms for Hizli, 1000/2000 ms for Orta (default), and 1500/3000 ms for Yavas. Manual first-pass timeouts range from 200–5000 ms; retries use twice that timeout, capped at 5000 ms. Every filtered or error/unknown result receives one retry; later profile-defined rounds retry only remaining error/unknown results. Silent targets can take many minutes.
  * **Interactive controls:** Esc cancels. Once an open port appears, Enter stops scanning and opens port selection using the results found so far. Early selection reports partial coverage and leaves unfinished ports unscanned.
  * **Offline test builds (`NERO_SAFE_TEST` / `NERO_NETWORK_UNAVAILABLE`):** Simulates port state in memory without opening remote network sockets.
* **Platforms:** macOS, Linux, Windows (native MinGW).
* **Restrictions:** Target must be a valid IPv4 address in dotted-decimal notation.

See [Port scanner reliability](portscanner-reliability.md) for retry rounds,
resource limits, result meanings and cancellation behavior.

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
* **Maximum command line length:** 255 bytes (`CLI_LINE_CAPACITY` is 256 including the terminating NUL). This is a byte limit, not a Unicode character count. Longer input produces a `Command too long` warning and is rejected safely without buffer overflow.
* **Maximum token count:** 64 tokens (`CLI_MAX_TOKENS`). Commands exceeding 64 tokens are rejected cleanly.
* **Unrecognized commands:** Commands not matching the catalog report `Unknown command: <token>` and exit code `0` to keep the shell interactive.

### Persistent command history (source builds after v0.1.3)

Interactive sessions load and save at most 256 recognized commands in
`~/.nero_history` (Windows: `%USERPROFILE%\\.nero_history`). Entries are written
after each accepted line; a clean exit is not required. Piped sessions and
privileged POSIX sessions default to memory only. `NERO_HISTORY_FILE` selects
an explicit file and also enables persistence for piped input;
`NERO_HISTORY_FILE=:memory:` disables disk history.

Persistence uses a conservative allowlist: `help`, `help -All`, `version`,
`clear`, `history`, `-mode` with its documented on/off forms, `log`,
`log -errors`, `log -path`, and exact numeric IPv4 `-Sc` commands. Unknown
commands, extra arguments, raw-engine commands, credentials and arbitrary
input are excluded from disk history. Session memory still contains typed
commands; use `history -clear` to remove them. Target addresses in recognized
scan commands are retained unless disk history is disabled.

POSIX files must be owned by the current user with no group/other access and
are created as 0600. Links, special files and busy locks are rejected. Windows
rejects reparse points and special files, inherits the parent directory ACL,
and uses an exclusive file handle during each update. Reads are bounded to
64 KiB, malformed records are discarded, and each write reloads disk state
under the lock so an older shell cannot restore previously cleared entries.
Concurrent writes do not merge session-only or excluded entries. Storage
failure leaves the session usable, displays a warning and records an event in
`log -errors` without raw input. A lock conflict drops that persistence update
rather than waiting. Writes are bounded in-place updates, not crash-atomic
transactions; a crash during rewriting may lose recent history.

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


Log recovery and clearing behavior:

- A truncated final line is separated from the next event before appending, so
  its severity cannot absorb a later error record.
- Special files are rejected (POSIX opens are nonblocking and descriptors are
  checked as regular files); unavailable storage falls back to memory.
- Oversized existing current/backup files retain complete recent records in a
  private temporary file, then replace the original only after a successful copy.
- `log -clear` retries configured disk files after a prior write failure. It
  reports failure and retains recent memory records if the persistent clear is
  unsuccessful. A successful clear also restores disk logging when possible.
- Session end records contain the actual exit status and stdout error flag.
