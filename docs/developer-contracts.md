# Developer contracts

| Module | Owns | Boundary |
| --- | --- | --- |
| `terminal.c` | Signal lifecycle, raw-mode snapshot, cleanup, dispatch loop | Handlers set flags only; normal execution restores terminal state |
| `input.c` | Byte editing and the bounded session-history ring | Maximum line is 255 bytes; overflow never dispatches a prefix |
| `history.c` | Persistent recognized-command history | Disk import is untrusted; storage errors leave session operation available |
| `parser/parser.c` | Quote/escape decoding into caller storage | Borrowed token pointers, explicit capacities, final NULL slot; only success publishes argc |
| `commands.c` | Command syntax, mode gates and dispatch | At most 64 tokens plus the NULL sentinel; invalid input cannot reach network routines |
| `operation_ui.c` | Operation-local menus and rendering | UI does not decide scanner result classifications |
| Platform adapters | OS handles, readiness and interface enumeration | Windows SOCKET values remain pointer-sized across all boundaries |
| `core/log.c` | Bounded event logs and severity | Raw commands are not logged; events and command history are separate stores |

CLI-private functions live in `src/cli/cli_internal.h`. Applications should
include public `include/nero/*.h` contracts. Adding a command requires updating
the dispatcher/catalog, syntax tests and command reference. Disk history uses
an explicit allowlist: adding a command must not implicitly allow arbitrary
arguments or credentials to be persisted.

The parser's argv capacity includes its terminating NULL slot; the dispatcher
allocates 65 pointers to accept 64 arguments. Its output storage belongs to the
caller and must outlive all borrowed token pointers. Do not read tokens after
a non-success parser return.

## Verification scope

`parser-boundaries` exercises exact input/storage/argv capacities, missing NUL,
quotes, escapes and invalid arguments. `history-persistence` runs a real native
CLI across restarts and concurrent sessions; POSIX adds permissions, link,
special-file and lock checks. All tests use disposable history locations.
Terminal regressions disable persistent history to avoid modifying developer
profiles. The deterministic SIGHUP fault is injected between the termination
flag check and `pselect`; masking plus atomic wait handoff prevents the lost
wakeup. Repeated ordinary signals alone cannot prove that race is fixed.

Scanner synthetic tests exercise errors that cannot reliably be induced on
every host. Production scanner fixtures use real loopback listening/refused
ports, full ledger accounting, cancellation and worker-failure recovery, with
a guard rejecting non-loopback connections. Tests do not send raw traffic or
scan a third-party address. A passing synthetic or loopback fixture is not
evidence for Wi-Fi isolation, sleeping mobile devices or arbitrary LANs.

Run `make test`, `make test-sanitize` and the matching CMake presets. A host
that forbids loopback bind cannot complete the runtime fixture; report that
restriction and use an isolated runner without removing the fixture or
claiming a local pass. [Distribution](distribution.md) describes installation,
upgrade/removal and package-manager contracts and their remaining credentials.
