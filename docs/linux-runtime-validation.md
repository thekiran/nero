# Linux runtime validation — 2026-11-03

## Assessment

Actual Nero scanner worker networking executed successfully on ARM64 Linux.
This is a partial validation, not certification of the full traffic engine.
UDP transmission exists only in the prohibited flood worker and was not run.
TCP payload exchange is not implemented by the scanner. Timeout, high descriptor,
allocation/thread failure and active-operation signal coverage remain incomplete.

## Environment and reproduction

Host: macOS 27.2 ARM64. Docker Desktop was installed but stopped. Initial Docker
API inspection was sandbox-blocked; approved retry reported daemon unavailable.
Approved `docker desktop start` returned 0. No host packages or security settings
were changed. No Lima/Colima/Podman/Multipass/QEMU/orb command was found in PATH.
No remote machine was used.

Guest: Ubuntu 24.04.4, aarch64, Linux 7.0.12-linuxkit; GCC 13.3.0,
Clang 18.1.3, CMake 3.28.3. Loopback UP, IPv4 127.0.0.1/8, IPv6 ::1;
other tunnel interfaces DOWN. No external container networking or capabilities.
Normal TCP socket/connect/select work. Raw-socket privileges were deliberately
absent; no CAP_NET_RAW or CAP_NET_ADMIN was added. ARM64 compilation and executed
scanner paths passed; packet-layout/alignment behavior in flood code is unverified.

Build image: `docker build -t nero-validation-local .build/linux-validation`
(exit 0), using the Dockerfile also retained at `tests/linux/Dockerfile`.
Packages were installed only inside the image. For reproduction:

```sh
docker build -t nero-validation-local tests/linux
docker run --rm --network none --cap-drop ALL \
  --mount type=bind,src="$PWD",dst=/source,readonly \
  nero-validation-local sh /source/tests/linux/run.sh
```

The run script copies source into disposable `/tmp/nero`; it does not overwrite
host binaries/build directories. It records COMMAND and EXIT for each operation;
its wrapper continues after failure, so inspect individual statuses, not just
Docker exit 0. Both recorded container runs exited 0.

## Repository findings and document verification

`src/cli` implements the POSIX terminal, input, commands and rendering;
`src/core` and platform_info provide the platform report. Headers live in
`include/nero`; cJSON is bundled in `third_party`. Linux interface enumeration
uses sysfs; macOS uses getifaddrs. Neither selection function binds engine sockets
or retrieves an interface IPv4 address for transmission.

Make is authoritative for the complete Linux executable: main → CLI → SYN_Flood
→ portscanner → scan_ports_thread, then ping and flood workers. CMake builds
only the safe adapter, interface tester and foundation targets, even on Linux.
macOS replaces SYN_Flood with an unavailable diagnostic.

Scanner workers use nonblocking TCP connect, one-second select, SO_ERROR and
close. Results are protected by ports_mutex. The full scanner launches 200
threads and scans all ports; that entry point was not invoked. Flood starts
512 raw TCP/UDP workers, spoofs sources, loops without pacing, ignores send
errors and has unchecked allocations. It was not executed. There is no normal
UDP receiver, TCP application data protocol, or UDP timeout API.

Existing reports correctly distinguished fixture traffic from Nero traffic.
Older compatibility text about missing terminal signal handling and history
alignment is stale relative to the existing fixes. Linux-not-tested statements
are superseded only for the paths explicitly recorded here. CI workflows cover
foundation matrices (including ARM Ubuntu/Windows), compatibility and static
analysis; full Linux Make build is compile-only in CI. No CI was dispatched.

## Executed results and evidence

Exact flags, commands, syscall results and exit codes are in
[initial Linux log](linux-validation-logs/linux-first.log) and
[final Linux log](linux-validation-logs/linux-final.log).

| Check / command | Status | Exit and evidence |
|---|---|---|
| uname, os-release, gcc/clang/cmake versions, ip addr | PASS | Each exit 0; environment above |
| `make -B CC=gcc BUILD_DIR=/tmp/gcc-build TARGET=/tmp/nero-gcc` | PASS | 0, actual engine compiled/linked, no adapter substitution |
| Same build with Clang and clang paths, before fixes | FAIL | 2, strict prototype and EOF newline errors |
| Same Clang build after fixes | PASS | 0, full engine linked |
| Strict harness compile | PASS | 0, -Wall -Wextra -Werror -pedantic -std=c11 -pthread |
| `timeout 15 strace -f -e trace=socket,connect,getsockopt,close,select,pselect6 /tmp/scanner-runtime` | PASS | 0; actual scan_ports_thread compiled directly from production source |
| Ten sequential one-port connections | PASS | SOCK_STREAM, EINPROGRESS, pselect6 ready, SO_ERROR=0, close; server observed EOF |
| Closed local port | PASS | SO_ERROR=ECONNREFUSED, close; no false open result |
| Invalid IPv4 | PASS | worker returns without recording an open port |
| Descriptor cleanup | PASS | /proc/self/fd count equals initial count after all workers joined and listener closed |
| Scanner ASan/UBSan | PASS | compile/run 0; same worker paths; no diagnostics |
| Linux parser before fix, input 1junk | FAIL | Returned 0 and selected interface 1 |
| Parser regression after fix | PASS | 0; valid input accepted, six invalid/overflow/EOF inputs rejected |
| Production `/tmp/nero-gcc`, version/exit | PASS | 0; actual full Make executable, no network command invoked |
| Production SIGINT/SIGTERM while idle | PASS | test 0, process exits 130/143 |
| `ss -tanup` after harness | PASS | 0; only kernel TIME-WAIT entries, no listening/established test sockets |
| macOS `make test` | PASS | 0; safe and terminal suites |
| macOS normal/sanitize build `-j4` | PASS | 0 each |
| macOS normal/sanitize CTest | PASS | 0 each, 4/4 each |

Full Make flags are -Wall -Wextra -Werror -pedantic -std=c11
-D_POSIX_C_SOURCE=199309L -g -DUNICODE_SUPPORT, repository include paths;
link uses curl, cjson, ldns, pthread, ncurses. Sanitizer harness adds
-fsanitize=address,undefined -fno-omit-frame-pointer. The harness calls Nero's
worker directly; its server is only a controlled endpoint, not a substitute
client implementation. Full CLI invocation of the scanner/flood chain is excluded.

## Fixes

1. Reproduced Linux scanf prefix acceptance. Ported the existing bounded line/
strtol validation from the macOS adapter, rejecting overflow and trailing junk.
`tests/linux/interface_regression.py` covers valid and invalid input.
2. Reproduced strict Clang build failure. Corrected the portscanner declaration
to match its parameter, made no-argument definitions explicit `(void)`, and added
missing final newline. Full GCC and Clang builds now pass without warning suppression.
3. Historical integrity manifests remain intact; the existing local-fixes manifest
was extended with before/after hashes for these intentional changes. macOS
foundation tests verify those hashes. Prior user modifications were preserved.

## Incomplete coverage and risks

| Check | Status | Reason / command / exit |
|---|---|---|
| TCP connection timeout | NOT TESTED | No deterministic blackhole injected; no command, exit N/A |
| Unexpected peer reset | NOT TESTED | Only normal EOF measured; N/A |
| TCP data exchange | BLOCKED | No such scanner API; N/A |
| UDP exchange/destination/timeout/cleanup | BLOCKED | Only flood worker transmits UDP; real execution prohibited; N/A |
| Flood non-transmitting fault injection | NOT TESTED | No mock harness implemented for that worker; N/A |
| Interface enumeration failure, IPv4 binding | NOT TESTED | sysfs failure not injected; selection is not binding; N/A |
| Active-operation signals | NOT TESTED | Idle production CLI only; N/A |
| High FD, allocation/thread failure, integer boundary faults | NOT TESTED | Not exercised; N/A |
| Valgrind / full-engine leak proof | NOT TESTED | Sanitizers cover scanner harness only; N/A |
| x86_64 Linux / Windows | NOT TESTED | No builds/runtime on those architectures; N/A |

Source concerns remain: FD_SET above FD_SETSIZE, unchecked worker creation before
join, global non-reentrant scanner state, menu EOF loop, signal-handler stdio,
shell-interpolated ping, and unchecked flood allocations. These are inspection
findings, not reproduced/fixed runtime claims. Sanitizer success does not prove
complete memory safety. No new network features were invented.

Containers used --rm and finished; no background test service was launched.
Kernel TIME-WAIT is expected and not a leaked process descriptor. Docker Desktop
and the reusable build image remain available. No host firewall changes,
third-party probes, full-range scans, commits or pushes occurred.

Final macOS evidence: [log](linux-validation-logs/macos-final.log).
