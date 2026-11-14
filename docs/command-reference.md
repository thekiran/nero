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

## Read-Only Screens

### `preflight`
* **Syntax:** `preflight`
* **Classification:** **READ-ONLY.** Opens no sockets, sends no packets, resolves no names, starts no subprocess, and does not escalate privileges. It never enters or alters the `-Syn` execution path.
* **Description:** Reports what the experimental `-Syn` engine in **this build** would attempt, so a user can understand the expected resource use before deciding to run it. It does not configure anything — there is no way to change the worker count, packet rate, or packet volume from this screen.
* **Reported facts:**
  * Whether the live `-Syn` engine is **compiled into this target**. Offline and safe builds state plainly that live raw mode is **unsupported** and that no workers, sockets, or packets can be created.
  * The run-time-selectable worker count (`1`–`NERO_SYN_THREAD_HARD_MAX`, absolute safety bound) and UDP packet size (`NERO_SYN_PACKET_SIZE_MIN`–`NERO_SYN_PACKET_SIZE_MAX`), described as an **upper bound, never a guarantee**. The `-Syn` prompt shows a hardware-derived suggested maximum and uses it as the default; there is no fixed 512 ceiling. Both values are chosen in the `-Syn` settings prompt right before a run; this screen only reports their bounds. Thread and raw-socket creation can fail, so the actual number of workers is only known at run time and is reported by the engine itself.
  * The raw-socket privilege requirement (root / `CAP_NET_RAW`) and a **read-only** check (`geteuid()`) of whether the current process is privileged.
  * Qualified upper-bound estimates for threads at the suggested worker count, raw sockets per worker, and file descriptors (suggested workers × 2 + 64, raised via `setrlimit` only when permitted).
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

## Wireless Monitoring Commands

Wireless capture is read-only. NERO never joins a network, injects custom
802.11 frames, deauthenticates clients, changes the adapter's channel, or
decrypts payloads. The optional macOS active survey asks CoreWLAN to scan;
macOS may transmit normal probe requests, but NERO does not construct or inject
frames. Passive capture requires an interface and driver that actually deliver
IEEE 802.11/radiotap frames; a successful monitor-mode request alone does not
prove delivery.

### `wifi status` / `wifi capabilities` / `wifi interfaces`
* **Syntax:** `wifi status` | `wifi capabilities` | `wifi interfaces`
* **Description:** Reports adapter metadata, capture backend availability,
  monitor-mode support, and session state; on Linux it also reports the
  nl80211 interface type, supported bands and enabled/disabled/no-IR channel
  counts. `capabilities` expands this into a runtime capability matrix with
  explicit limitations, and `interfaces` lists visible Wi-Fi interfaces without
  scanning or transmitting.
* **Platforms:** macOS (CoreWLAN metadata), Linux (read-only interface
  discovery), Windows (`GetAdaptersAddresses`). Offline builds report that live
  capture is unavailable.

### `wifi capabilities`
* **Syntax:** `wifi capabilities`
* **Description:** Prints the runtime capability matrix for the current build
  and platform: active-scan availability, passive-capture backend, monitor-mode
  request support, whether 802.11 frame delivery has actually been observed,
  interface count, and explicit limitations. On Linux it queries nl80211 for
  interface type, wiphy monitor support, bands and channel counts. Capability
  probes are read-only and never change the adapter mode, channel or regulatory
  settings.
* **Platforms:** macOS, Linux, Windows, and offline builds. Offline builds
  report that capture and scanning are unavailable instead of guessing.

### `wifi scan [--band 2.4|5|6] [--channel N] [--live] [INTERFACE]`
* **Syntax:** `wifi scan [--band 2.4|5|6] [--channel N] [--live] [INTERFACE]`
* **Description:** Performs an active CoreWLAN survey without joining a
  network. `--band` and `--channel` filter the results client-side; an empty
  match is reported together with the adapter/regulatory-domain caveat rather
  than being presented as proof of absence. `--live` repeats the active survey
  every five seconds until `Ctrl+C`. macOS may redact SSID/BSSID unless Location
  Services is granted to the terminal; redacted values are displayed as
  `<redacted>`, never invented. Active survey results are separate from
  passive-session data. An active scan may interrupt an in-progress passive
  capture.
* **Platforms:** macOS only; Linux and Windows report that active scanning is
  unavailable.

### `wifi aps [--channel N]` / `wifi clients [--channel N] [--bssid MAC] [--live] [--anonymize]`
* **Description:** Shows access points or client devices observed by the current
  passive session or imported capture. Client observations are not proof of a
  live association. `--bssid` limits clients to the AP relationship that was
  actually observed; `--live` refreshes the table every second until `Ctrl+C`.
  `--anonymize` applies a process-local pseudonym to client and BSSID addresses
  in terminal output. Tables adapt to the terminal width (or `COLUMNS`); when a
  column cannot fit it is hidden and a narrow-view note names it, so piped
  output never silently drops data.

### `wifi signal [--live]`
* **Syntax:** `wifi signal [--live]`
* **Description:** Shows adapter signal diagnostics: RSSI, a qualitative
  quality label derived from RSSI, channel, band, frequency and adapter state.
  `--live` refreshes once per second and keeps a short RSSI history graph until
  `Ctrl+C`. Noise floor, SNR and link bitrate are printed as "not exposed" when
  the platform backend does not supply them; no metric is fabricated.
* **Platforms:** macOS supplies RSSI/channel through CoreWLAN. Linux and
  Windows backends currently expose interface metadata only, so signal fields
  are reported as not exposed. Offline builds report the panel as unavailable.

### `wifi inspect <MAC|BSSID>`
* **Description:** Prints the observed AP or client record, including channel,
  band, RSSI, security, first/last seen, association confidence, and per-client
  RSSI history.

### `wifi monitor [INTERFACE]` / `wifi monitor --hop` / `wifi monitor start|stop|status|enable|disable`
* **Description:** `wifi monitor` runs a foreground passive survey and stops on
  `Ctrl+C`; `--duration N` bounds it. On an interactive terminal it renders a
  flicker-free, full-screen live table (alternate screen, line-level diff) with
  keyboard controls — `s` cycles the access-point sort (power/essid/bssid), `f`
  toggles a real-client filter, `space` pauses auto-refresh, the arrow/PageUp/
  PageDown/Home/End keys scroll, and `q`/`Esc`/`Ctrl+C` quit. When output is a
  pipe, file or CI log it degrades to a plain, scriptable transcript redrawn
  once per second. Both paths share one responsive table engine. The
  access-point table lists signal, BSSID, beacon/probe counts, channel, band,
  security, observed clients and ESSID; the station table lists BSSID, station,
  signal, channel, frame count, notes and resolved ESSID. `start`, `stop`, and
  `status` manage a background session. Capture is read-only and uses dynamic
  libpcap on POSIX or Npcap on Windows. A warning is shown after three seconds
  without frames.
* **`--hop [--dwell MS] [--bands 2.4,5,6]`:** Opt-in channel hopping. The default
  survey is strictly read-only and stays on the adapter's current channel; this
  separate mode tunes the radio across a channel plan so the survey can see
  every channel. It changes the radio, so it is **Linux only** (nl80211
  `SET_CHANNEL`), requires root/`CAP_NET_ADMIN`, requires the interface to
  already be in monitor mode (`wifi monitor enable`), and requires interactive
  consent (`yes`). `--dwell` sets the per-channel dwell time (default 300 ms) and
  `--bands` the bands to sweep (default 2.4 GHz). On macOS, Windows and offline
  builds `--hop` reports that in-NERO channel control is unavailable and the
  survey stays single-channel; no radio change is made.
* **Platforms:** Live macOS/Linux/Windows builds. Permission errors, missing
  libpcap/Npcap, non-monitor-capable drivers, and zero-frame delivery are
  reported explicitly; no data is fabricated.
* **`enable` / `disable`:** Linux only. `wifi monitor enable [INTERFACE]` first
  queries nl80211 for the current interface type and advertised monitor support,
  then requires an interactive confirmation of the exact word `yes`. It saves
  the previous interface type best-effort (mode `0600`, `O_NOFOLLOW`) and only
  reports success after re-reading the interface and verifying the new type.
  `wifi monitor disable [INTERFACE]` restores that saved type when it matches the
  interface, otherwise it clearly reports a fallback to station/managed mode.
  Changing type requires root or `CAP_NET_ADMIN`. NERO does not modify
  NetworkManager, service state, regulatory settings or the channel. On macOS,
  Windows and offline builds these actions report unavailable and make no
  state changes.
* **Apple Silicon note:** some adapters accept the monitor-mode request and
  report radiotap link type but still deliver zero frames. Use
  `wifi analyze` with a genuine capture in that case.

### `wifi analyze <PCAP_FILE>`
* **Description:** Analyzes classic pcap or pcapng files offline. Supported
  link types are `105` (IEEE802_11) and `127` (radiotap); Ethernet captures are
  rejected because they do not contain 802.11 headers. Every length is
  bounds-checked, malformed frames are skipped and counted, and no live capture
  API is used.
* **Platforms:** macOS, Linux, Windows.

### `wifi export [--anonymize] [--channel N] [FILE]`
* **Description:** Exports the current passive data set to CSV. MAC addresses
  are written only because the user explicitly requested an export;
  `--anonymize` stores deterministic process-local pseudonyms instead.

### `wifi clear`
* **Description:** Stops any active capture, then discards captured AP/client
  data and counters from memory.

---

## Experimental Packet Engine

> [!WARNING]
> The raw packet engine is experimental software designed exclusively for isolated research laboratories and explicitly authorized testing environments. Generating unsolicited network traffic against unauthorized third-party networks or systems is strictly prohibited. See [SECURITY.md](../SECURITY.md) and [laboratory-safety.md](laboratory-safety.md).

### `<network IP> -Syn`
* **Syntax:** `<network IP> -Syn` or `-Syn <network IP>`
* **Description:** Initiates raw TCP/UDP packet transmission routines against the specified target.
* **Run-time settings prompt:** After the target and raw-socket checks pass, the engine probes local CPU/RAM and shows the measured hardware plus a suggested maximum worker count, then asks for the values to use for this run before spawning any worker. Pressing Enter keeps the default; input must be a whole number within the shown bounds (rejected, never silently clamped). `Esc`, `q` or `Ctrl+C` cancels the run without starting anything.
  * **Worker thread count:** `1`–`65535` (`NERO_SYN_THREAD_HARD_MAX`, an absolute safety bound for input and allocation). The default is the hardware suggestion: **64** on a Low-resource machine (`<= 2` logical CPUs or `<= 4 GiB` RAM) or **256** otherwise, falling back to `512` (`NERO_SYN_THREAD_COUNT`) when probing is unavailable. There is no fixed 512 ceiling. The real count can be lower if thread or raw-socket creation fails.
  * **Packet size (bytes):** `60`–`4096` (default `4096`). This sets the **UDP datagram** size on the wire (payload = packet size − 28 bytes of IP/UDP headers). The **TCP** leg is always a 40-byte header-only SYN and is not affected.
  * The selected values apply to this invocation only. There is no separate command or stored profile for them; running `-Syn` again prompts again.
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
| `wifi status` / `wifi capabilities` / `wifi interfaces` | Supported | Supported | Supported | Metadata only |
| `wifi scan` (Active CoreWLAN, `--band`/`--channel`/`--live`) | Supported | **Unavailable** | **Unavailable** | **Unavailable** |
| `wifi signal` | Supported (CoreWLAN) | Metadata only | Metadata only | **Unavailable** |
| `wifi analyze` / `export` / `aps` / `clients` | Supported | Supported | Supported | Supported |
| `wifi monitor` (Passive capture) | Supported* | Supported* | Supported* | **Unavailable** |
| `wifi monitor enable/disable` (nl80211 type control) | **Unavailable** | Supported (`sudo`) | **Unavailable** | **Unavailable** |
| `<target> -Syn` (Raw Flood Engine) | Supported (`sudo`) | Supported (`sudo`) | **Unavailable** | Simulated |

\* Requires permission to open the capture device, a monitor-capable driver and
libpcap/Npcap. Delivery must be verified with `wifi monitor`; see the Apple
Silicon zero-frame limitation above. Offline tests never open an interface.


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

### Offline environment diagnostics

Run `doctor` in the shell, or `nero --doctor` for a standalone report that bypasses
session logging/history and Windows socket initialization. Both paths are read-only
and never transmit packets, escalate privileges, fix files or read log/history
contents. Personal paths, addresses and environment values are omitted by default.
The report shows build/OS/architecture, redacted installation location, PATH
resolution, terminal support, user storage-root availability and compiled network
features. Capabilities and privileges are observations, not successful socket tests.
Build tools are not runtime dependencies. Missing loader libraries can prevent the
program itself from starting, so doctor cannot diagnose them from inside the program.

Plain output is used for redirects, `NO_COLOR`, `TERM=dumb` and missing terminal
capabilities. Consult [installation](installation.md) for a missing/global command,
and [troubleshooting](troubleshooting.md) for terminal or storage diagnoses. The
existing interactive shell still owns its usual session history/log lifecycle;
use `--doctor` when collecting a report without any session storage changes.

### Readable command names

`help` groups commands under GENERAL, HISTORY, LOGGING, DIAGNOSTICS, NETWORK,
WIRELESS and LABORATORY. `help --all` adds the detailed scope notice and legacy
syntax table. Existing commands remain valid:

| Readable command | Existing equivalent |
| --- | --- |
| `help --all` | `help -All` |
| `history clear` | `history -clear` |
| `logs [errors\|path\|clear]` | `log [-errors\|-path\|-clear]` |
| `network status` | `-mode` |
| `network enable` / `network disable` | `-mode on` / `-mode off` |
| `scan <IP>` | `<IP> -Sc` / `-Sc <IP>` |
| `syn preflight` | `preflight` |
| `syn <IP>` | `<IP> -Syn` / `-Syn <IP>` |

These aliases reuse the same handlers, validation, mode gate, port selection and
hardware settings prompt. `syn <IP>` starts the existing packet-flood operation;
it does not mean an offline simulation. Use only the documented authorized lab
scope. Tests use safe/mock builds or disabled network mode. `sim` remains absent.
Recognized benign history aliases and canonical `scan <IP>` persist under the
existing bounded privacy allowlist; malformed/secret arguments do not.
