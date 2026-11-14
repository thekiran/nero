# Changelog

Release status is stated per version. The older v0.1.0–v0.1.2 entries remain
preparation records for drafts; v0.1.3 is publicly published. No release is
advertised as stable or security-certified.

## Unreleased

- Extract the Wi-Fi table renderer into a shared, capability-gated table engine
  (`nero/table.h`, `src/cli/table.c`) used by every Wi-Fi surface — the static
  `wifi aps`/`wifi clients`/capture-statistics tables and the live monitor alike.
  One responsive grid now measures Unicode display cells, grows/shrinks flex
  columns, drops low-priority columns on narrow terminals with an honest note,
  and falls back to an ASCII grid under `NERO_ASCII`/`NO_COLOR`. A single
  `nero_terminal_width()` is the source of truth for terminal width.
- Make `wifi monitor` a flicker-free, interactive live table on a TTY: it renders
  into the alternate screen and rewrites only the rows that changed each frame
  (no full-screen clear), with keyboard controls to sort access points, filter to
  real clients, pause auto-refresh, scroll, and quit without `Ctrl+C`. In a pipe,
  file or CI log it degrades automatically to a plain, scriptable transcript. The
  same engine renders both paths, so the airodump-style survey and the static
  tables share one look.
- Add opt-in channel hopping (`wifi monitor --hop [--dwell MS] [--bands 2.4,5,6]`).
  The default survey remains strictly read-only and never changes the radio; the
  new mode tunes a monitor-mode interface across a channel plan via nl80211
  `SET_CHANNEL` so a survey can see every channel. It is Linux-only, needs
  root/`CAP_NET_ADMIN` and an interactive `yes` consent, and reports itself as
  unavailable (with no radio change) on macOS, Windows and offline builds. The
  new channel-set path is protocol-tested with mock netlink exchanges; the table
  engine has deterministic unit tests. `wifi capabilities` gains a channel-hopping
  row.
- Add the `wifi` command family: adapter/interface status, macOS CoreWLAN
  active survey, passive AP/client tracking, live monitor dashboard, offline
  pcap/pcapng analysis, CSV export with optional pseudonymization, and bounded
  in-memory clearing. Passive capture is read-only; live capture dynamically
  loads libpcap/Npcap and is unavailable in offline-safe builds. No monitor-mode
  delivery is assumed: zero-frame capture is reported explicitly, and macOS
  SSID/BSSID redaction is preserved rather than invented.
- Make the `wifi` tables terminal-aware: widths follow `COLUMNS`/the live TTY,
  flexible columns grow or shrink, low-priority columns are hidden on narrow
  screens with an explicit narrow-view note, and cell widths are measured in
  Unicode display cells so accented or wide values cannot shear the grid.
  Rendering still falls back to an ASCII grid under `NERO_ASCII` or `NO_COLOR`.
- Render `wifi monitor` as an airodump-ng-style live survey: a compact
  access-point table (BSSID, PWR, BEACONS, PROBES, CH, BAND, SECURITY, CLIENTS,
  ESSID) and a station table (BSSID, STATION, PWR, CH, FRAMES, NOTES, ESSID),
  sorted by signal and grouped by BSSID. The explicit zero-frame warning for
  adapters that accept monitor mode without delivering frames is preserved.
- Add Linux nl80211 interface control: `wifi monitor enable [INTERFACE]` and
  `wifi monitor disable [INTERFACE]` query the wiphy, require an explicit `yes`
  confirmation, save the previous interface type best-effort and verify the
  kernel's reported type after the change. Disable restores the saved type or
  reports a station/managed fallback. `wifi status` and `wifi capabilities`
  show nl80211 type, monitor support, bands and channel counts on Linux. The
  implementation is protocol-tested with mock netlink exchanges; real Linux
  hardware validation is still required, and macOS/Windows/offline builds
  report the control as unavailable without changing state.
- Add `wifi capabilities` with a runtime capability matrix: active-scan and
  passive-capture backends, monitor-mode request support, observed frame
  delivery, interface count and explicit per-platform limitations. Probes are
  read-only and offline builds report unavailable instead of guessing.
- Add `wifi signal [--live]`, a wavemon-style panel for RSSI, channel, band,
  frequency and a short RSSI history. Noise floor, SNR and link bitrate are
  reported as not exposed when the backend omits them; no values are invented.
- Extend `wifi scan` with `--band`, `--channel` and `--live` (client-side
  filtering and a five-second rescan loop), and `wifi clients` with `--bssid`
  filtering and `--live` refresh. Empty filtered results carry the
  adapter/regulatory-domain caveat.
- Add portable Wi-Fi table-data verification for macOS, Linux and Windows:
  a deterministic pcap is rendered through `wifi analyze`, `wifi aps`,
  `wifi clients` and `wifi inspect`, and the test asserts that observed AP,
  client, filter and inspection fields reach the tables. It avoids pty/termios
  APIs so the same fixture runs under Make and CTest on all three platforms.
- Add deterministic Wi-Fi parser/tracker/pcap fixtures to Make and CTest,
  including modern classic-pcap timestamps, little- and big-endian pcapng,
  radiotap fields, association confidence and history privacy coverage.
- Add a run-time settings prompt to the live `-Syn` engine: before any worker
  is spawned it probes local CPU/RAM (read-only), shows the measured hardware
  and a suggested maximum worker count, then asks for the worker thread count
  and the UDP packet size (`60`–`4096` bytes, default `4096`). The worker count
  defaults to the hardware suggestion (64 on low-resource machines, otherwise
  256) and is accepted up to the `NERO_SYN_THREAD_HARD_MAX` safety bound, so the
  old fixed 512 ceiling is gone. Enter keeps the default, out-of-range input is
  rejected rather than clamped, and `Esc`/`q`/`Ctrl+C` cancels without starting.
  The TCP leg remains a 40-byte header-only SYN. There is no separate command:
  the values are just part of `-Syn`, so further run-time parameters can be
  added to the same prompt.
- Add the `preflight` read-only `-Syn` preflight screen: it reports, as
  qualified upper bounds, what the experimental engine in the current build
  would attempt (hardware-suggested worker count, absolute worker/descriptor
  bounds, raw-socket privilege requirement) and clearly flags builds where live
  raw mode is not compiled in. It opens no sockets, sends no packets, starts no
  subprocess, escalates no privilege, and never enters the `-Syn` path.
- Remove the `sim` offline simulation screen; its read-only CPU/RAM probing and
  deterministic workload recommendation now feed the live `-Syn` settings
  prompt instead of a separate command.
- Fix macOS CPU detection: `hw.logicalcpu`/`hw.physicalcpu` are 32-bit sysctls
  and were being discarded by a `sizeof(long)` length check, so the `-Syn`
  hardware suggestion reported the processor count as `Unavailable` on working
  Macs.

## 0.1.4 — native installer release

See [release status and downloads](https://github.com/thekiran/nero/releases)
for publication and platform assets.

- Add native macOS Installer `.pkg` and Windows Installer `.msi` wizards,
  plus Ubuntu-compatible `.deb` packages to the release pipeline. Normal
  installation registers the global command without shell setup commands.
- Add machine PATH and Start menu integration with managed MSI upgrades and
  removal; notify Windows about manual user PATH changes.
- Verify native package installation from unrelated folders, including normal
  and sudo POSIX shells and fresh Windows PowerShell/CMD processes.

- Persist recognized command history with bounded private storage, opt-out,
  explicit failures and cross-session clear behavior. Unknown commands and
  arbitrary arguments are excluded from disk history.
- Accept the documented 64 command tokens; add parser capacity/syntax tests.
- Verify history restarts, malformed files, permissions, links, locking and
  concurrent clearing on native CLI targets; exercise ASan/UBSan on POSIX.
- Add staged installer lifecycle tests, POSIX atomic replacement, and POSIX/
  Windows uninstallers that retain user data.
- Add a macOS Homebrew formula pinned to published v0.1.3 archives and native
  Ubuntu amd64/arm64 Debian package creation with inferred ELF dependencies.
  Distribution CI verifies install/reinstall or upgrade/removal; it does not
  publish an APT repository or claim code signing.

## 0.1.3 — published 2026-11-08

[Download v0.1.3](https://github.com/thekiran/nero/releases/tag/v0.1.3).
Built from `f9904830d2caf3d57df16f36e876ebe56d2c0feb`; all five native package jobs
and aggregate SHA-256 checks passed in [release run #58](https://github.com/thekiran/nero/actions/runs/42478613983).

Includes the stability fixes merged through [PR #8](https://github.com/thekiran/nero/pull/8):

- Preserve pointer-sized Windows SOCKET handles through socket and poll adapters.
- Restore original input/output console modes and output code page.
- Add synthetic handle/console regressions, a real console fixture and a bounded
  localhost TCP test.
- Serialize manual/tag workflows for the same version and expose Release checks.
- Correct stale Windows support documentation and clarify audit/test boundaries.
- Make the scrollbar division invariant explicit for static analysis.

- Add full-range TCP accounting, bounded retries and early port selection with partial coverage.
- Add persistent application logs and scanner error diagnostics; command history remains session-only.
- Stamp the CLI with its verified release tag.
- Correct CLI limits, scanner documentation and Wiki publication error reporting.

Repository quality work already merged: contribution forms, action dependency updates, a roadmap,
community conduct expectations and a shorter README with dedicated installation docs.

## 0.1.2 — draft prepared 2026-11-07

- Install `nero` into a system command directory on macOS/Linux.
- Install native Windows `nero.exe` and update user or machine PATH.
- Include installers in archives and verify execution outside the build tree.
- Verify all five native packages and SHA-256 checksums before preparing the draft.

## 0.1.1 — draft prepared 2026-11-07

- Add a real Windows x86-64 MinGW CLI package and native behavioral tests.
- Check PE architecture, DLL imports and extracted standalone execution.
- Keep the Windows raw packet engine explicitly unavailable.

## 0.1.0 — draft prepared 2026-11-07

- Add native macOS ARM64/Intel and Linux x86-64/ARM64 release builds.
- Gate packaging on offline regressions and POSIX ASan/UBSan tests.
- Produce archives, build metadata, SHA-256 sidecars and an aggregate manifest.

### Scanner session reliability (unreleased)

- Extract the production TCP core into independently owned scan sessions and replace source-inclusion tests with per-session I/O injection.
- Preserve successful writable connections that already have a hangup; count worker failures and fail explicitly on clock errors.
- Add simultaneous-session, stop/cleanup, known-listener and Linux interface-fallback regressions.
- Separate deterministic interface tests from mandatory native Linux enumeration and remove unused Linux link dependencies.
