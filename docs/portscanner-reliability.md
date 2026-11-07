# Port scanner reliability review

## Scope

The TCP connect scanner now accounts for every port in 1–65535. Discovery does
not decide whether the target is scanned. No traffic engine, CLI command name,
ANSI/ASCII theme, speed menu, progress widget or port selection layout changed.

## Historical comparison

Local Git history was inspected at `6ff80a4` (2025-10-19) and `c0d08a7`
(2025-10-20), where the source was `portscanner.c` at the repository root.
The pre-refactor source is the version inherited through PR #14.

| Concern | October 2025 | Before this change | After this change |
| --- | --- | --- | --- |
| Host gating | No host precheck | 16-port probe; interactive default declines scan | No TCP precheck; optional ICMP is advisory |
| Scheduler | Static ranges, 200 threads | Static worker ranges; creation failure loses ranges | Atomic shared queue; caller drains work when threads fail |
| Socket/connect errors | Silently skipped | Several errors omitted; descriptor exhaustion treated as filtered | Every completed attempt has indexed state/error metadata |
| Polling | Per-port select | Batched poll with shared batch deadline | Per-socket absolute deadline; pending readiness rechecked; bounded cancellation waits |
| Retry | None | Entire deep phase skipped above candidate threshold | Every unknown/filtered result gets one retry, in bounded work windows |
| Accounting | Open ports only | Open and filtered list; no full ledger | Open + closed + filtered + unknown + unscanned always accounts for 65535 |
| Resources | 200 threads | Up to 8192 theoretical sockets; raises soft fd limit | At most 16 scan workers and 256 concurrent scan sockets; retry cap 128 |

The 2025 last-worker range included port 65535; the historical problem was not
an established off-by-one at that boundary. Failed worker creation, dropped
errors, host gating and retry suppression are independently reproducible bugs.
This review does not establish why a particular phone was visible in 2025.

## Result model

`PORT_UNSCANNED` means no completed first-pass outcome (including cancelled
in-flight work). A non-cancelled, completed full scan requires:

```
open + closed + filtered + error/unknown = 65535
unscanned = 0
```

Only successful connect completion establishes `open`. `ECONNREFUSED` establishes
`closed`. `ETIMEDOUT` or an exhausted connect deadline establishes `filtered`,
meaning no conclusive response within the observation window, not proof of a
firewall. Socket setup/allocation failures, routing errors, poll errors,
SO_ERROR query errors and invalid readiness stay `error/unknown`.

Readiness with `EINPROGRESS`, `EALREADY`, would-block or EINTR remains pending.
POLLNVAL is an error; HUP/ERR without successful completion evidence is not open.
Ready events are inspected before expiring outstanding deadlines. EINTR does not
restart a fresh timeout. Poll slices are at most 50 ms to allow cancellation.

A once-allocated indexed ledger replaces per-result realloc and linear duplicate
searches. Each queued index is claimed by exactly one worker per phase. If thread
creation fails, the existing workers and caller consume the same remaining queue.
The scan progress count measures unique completed first-pass ports, not retries.

The final ledger supersedes first-pass filtered results when a retry returns
closed or open. The summary reports actual final classifications, completed
retries, and unscanned work. No-open scans containing local/network errors return
PORTSCANNER_FAILED after reporting the inconclusive outcomes. Explicit user
cancellation returns PORTSCANNER_CANCELLED. Invalid IPv4 is rejected before UI or
socket setup. A fully filtered scan can return no selected port, with an explicit
inconclusive warning and host status unknown.

## Discovery and mobile devices

`online` denotes positive TCP open/refused or successful ICMP evidence. Lack of
responses remains `unknown`. Neither silence nor local routing errors proves a
host `offline`; this scanner deliberately does not assert that state.

Set `NERO_SCAN_ICMP=1` to optionally invoke one OS ping (numeric IPv4 only, no
shell). It is bounded to approximately 1.7 seconds and reaps its child process.
Missing ping, insufficient privileges, ICMP filtering and timeout all fall back
to unknown and continue the TCP scan. ICMP is disabled by default. No ARP sweep
or LAN enumeration is added. Automatic device listing is a separate feature.

A phone with no listening ports may produce closed results while still being
online. A sleeping phone, client/AP isolation, filtered TCP, blocked ICMP or an
outdated IPv4 address may produce unknown/filtered results. The scanner cannot
identify the same phone at a new address, infer physical presence from silence,
or remove false negatives caused by packet loss or changing network conditions.

## Resource and cleanup bounds

The scanner never raises RLIMIT_NOFILE. It reserves 64 descriptors when sizing
its budget, and handles exhaustion caused by other process activity as unknown.
One finite retry uses a smaller socket budget and a longer timeout, capped at
5000 ms. Resource failures introduce a bounded 5 ms batch backoff. An entirely
silent /65535-port target can still take many minutes; Esc or session signals
cancel the work. This is a bounded connect scanner, not a packet flood engine.

All workers are joined before the UI reader stops and the ledger is freed. The
caller reads Esc if the animation thread could not start. Banner collection
retains its existing 128-port cap but uses cancellable nonblocking I/O and
reports optional thread-creation failure. Banner metadata does not change port
classification. A banner cap never limits which TCP ports get scanned.

Windows SOCKET values remain pointer-sized; WSAPoll errors now transfer the
actual WSA error into errno instead of leaving a stale connect error behind.
Linux and macOS use native poll/fcntl. Windows uses the existing WSAPoll and
ioctlsocket adapter. Winsock pending/interruption codes are explicitly handled.

## Regression coverage and validation

`tests/portscanner_reliability.c` compiles the production scanner with synthetic
socket, connect, poll, SO_ERROR, nonblocking, clock and thread-creation seams.
Real pthreads exercise concurrent claims. Socket handles above INT_MAX are used
on Windows. No real socket, remote target or raw packet engine is used.

Covered scenarios include silent discovery followed by port 54321 opening;
all ports closed; all ports timing out; delayed success and pending completion;
resource, nonblocking, routing, poll and SO_ERROR failures; HUP/POLLNVAL;
interrupted polls; first and last port; duplicate prevention; complete retry
windows above old limits; partial/all worker-creation failure; cancellation of
first/deep phase work; resource recovery; invalid IPv4; and the production entry
point continuing with unknown discovery. The Windows adapter fixture additionally
checks WSAPoll error propagation.

Local macOS ARM64 commands actually run:

| Check | Result |
| --- | --- |
| Full native Make build in `.build/scan-refactor` | Passed |
| `make test TEST_DIR=.build/scan-tests` | Passed: scanner fixture, 10 safe, 22 terminal, operation layout |
| `make test-sanitize BUILD_DIR=.build/scanner-verify` | Passed: ASan + UBSan |
| `make test-tsan BUILD_DIR=.build/scanner-verify` | Passed: scanner fixture, no race report |
| CMake build and CTest in `.build/scanner-cmake` | Passed: 6/6 |
| Clang Static Analyzer on `src/modules/portscanner.c` | Passed |
| `git diff --check` | Passed |

The CMake target is enabled for POSIX and native MinGW Windows, and is included
in existing package/CTest workflows. MSVC remains foundation-only; this fixture
uses the project's MinGW/pthread implementation. Compatibility CI adds a macOS
Clang ThreadSanitizer step. ASan/UBSan and TSan run separately.

Linux/Windows hosted results will be recorded once their new CI runs finish.
No physical iPhone/Android or real-network scan was performed. Optional ICMP was
not exercised against a real target.
