# Experimental laboratory compatibility

NERO IS AN EXPERIMENTAL, LABORATORY-ONLY NETWORK TESTING AND RESEARCH PROJECT.
IT IS NOT INTENDED FOR GENERAL PUBLIC USE OR PRODUCTION DEPLOYMENT.
Testing is restricted to isolated environments or explicitly authorized systems.

## Initial analysis

The checkout was clean on `main`; development uses `feat/macos-support`.
README, Makefile and all C/header files were inspected before implementation.
No existing project-specific AGENTS.md was found.

| Files | Classification and dependencies |
| --- | --- |
| `src/main.c`, `include/nero/terminal.h` | Portable entry point: main → terminal_code. |
| `src/cli/terminal.c` | POSIX termios/read/write; ANSI terminal UI, history, ascii_function and SYN_Flood. Needed bounded input, EOF handling and nonrecursive clear. |
| `src/cli/ascii.c`, `include/nero/ascii.h` | POSIX pthread mutex, ANSI/UTF-8 display, system("clear"); no Linux-only API. Graphics retained. |
| `include/nero/printing.h` | Portable string macros; header-only (no `printing.c`). |
| `src/platform/linux/select_interface.c`, `include/nero/select_interface.h` | Linux sysfs enumeration and numeric type mapping. Implementation retained byte-for-byte; standalone function is not called by the current CLI. |
| `src/modules/syn_flood_powered.c`, `include/nero/syn_flood_powered.h` | Linux TCP/IP layouts (`linux/tcp.h`, `struct iphdr`), raw sockets, IP_HDRINCL, Linux ping options, pthreads/signals. Packet construction, transmission and run state stay here; UI, settings, preflight and report wording live in `src/modules/syn/`. Deliberately excluded on macOS. |
| `src/modules/syn/syn_*.c`, `src/modules/syn/syn_internal.h` | Private -Syn support modules: settings prompt, animation, privilege/target preflight and user-facing messages. No network engine or packet logic; no new globals. |
| `src/modules/portscanner.c`, `include/nero/portscanner.h` | POSIX socket/connect/select/fcntl/pthreads; active network scanning. Excluded on macOS and in tests; both files retained unchanged. |
| `third_party/cjson/cJSON.c`, `third_party/cjson/cJSON.h` | Bundled portable cJSON 1.7.18; only standard C dependencies; no current CLI callers. Retained unchanged. |

Linux call graph: main → terminal_code → ascii_function / SYN_Flood;
SYN_Flood → nero_syn_preflight_privileges, nero_syn_prompt_settings,
port_get → portscanner → scan_ports_thread / select_port_menu,
nero_syn_target_is_online, nero_syn_animation_main and flood workers.
The animation borrows the engine-owned stop flag and cursor row through
nero_syn_animation_view and uses row_location_function, windows and ekran_mutex.
Interface selection and cJSON have no incoming calls from the current CLI.

macOS replaces only the SYN_Flood implementation with a diagnostic adapter and
selects a separate interface source at build time. No socket, DNS, ping or scan
operation exists in that adapter. The interface adapter uses AF_LINK records,
skips invalid/duplicate names, bounds its fixed storage, frees getifaddrs data,
and returns a static selected name, matching the original lifetime contract.
The Linux interface source is not rewritten to use getifaddrs.

## Build scope

Makefile remains the build system. Linux retains its original source list,
POSIX feature macro and library flags. Darwin uses SDK headers and pthreads only;
there are no hard-coded Homebrew paths. CC, CPPFLAGS, CFLAGS and LDFLAGS may be
overridden on the make command line. Header dependency files are generated.
The original run/install targets remain; none was executed during validation.
Use clean before changing compiler/platform or overriding build flags.

No-argument declarations in the shared CLI headers and definitions now use
`(void)` to satisfy Apple Clang's strict C11 diagnostics without disabling warnings.
The engine header's unused animation declaration also receives `(void)`; the
engine implementation is unchanged. Existing command matching semantics remain.
Terminal reads handle EOF and EINTR; non-TTY input avoids uninitialized termios;
input and right-arrow positions are bounded. `clear` redraws without recursively
entering another shell. History remains in-memory, with the original 256 entries.

## Validation performed locally

Environment: Darwin arm64, Apple Clang 21.0.0 (clang-2100.4.1.1).

| Check | Result |
| --- | --- |
| macOS default compiler/linker build | PASS with -Wall -Wextra -Werror -pedantic -std=c11. Linker emits a history-array alignment warning; see risks below. |
| `make test` | PASS: 9 tests, no traffic engine/scanner linked. |
| `make test-sanitize` | PASS: same 9 tests under AddressSanitizer and UndefinedBehaviorSanitizer. |
| `make analyze` | PASS: Clang analyzer on main, terminal, ASCII, unavailable-network adapter and macOS interface adapter. |
| Default macOS executable | PASS: help/version/exit smoke check. |
| Test executable imports | PASS: nm check rejects socket/connect/sendto/sendmsg/name-resolution imports. |
| CLI behavior | help/help -All, version, history/navigation, unknown command, exit, repeated clear, backspace, long input, cursor bounds, EOF and truncated escape sequences checked. |
| Real local interfaces | List/valid selection and invalid input/EOF checked; no probing or traffic. |
| Mock getifaddrs | Duplicates, missing address/name, oversized name, empty list, API failure, capacity boundary and cleanup checked. |
| PTY terminal behavior | Raw mode and restoration after clear/exit checked. BSD's transient PENDIN bit is excluded from attribute equality. |
| Linux engine, scanner, interface source integrity | Unchanged from initial checkout. |
| Linux build/runtime regression | NOT TESTED: local Docker API access denied. Source preservation alone is not a Linux runtime result. |
| GitHub Actions | Configured for Linux/GCC and macOS/Clang; NOT TESTED remotely. No commit/push performed. |
| Actual load, scan, packet transmission | NOT TESTED intentionally; no real traffic tests performed. |
| Intel macOS, other SDK versions, Linux Clang full build | NOT TESTED. |
| cJSON sanitizer validation | NOT COMPLETED: SDK deprecation errors for existing sprintf calls with sanitizer flags. cJSON is compiled in the normal build but omitted from CLI test/analysis sources because the CLI does not call it. |

Initial PTY test attempts timed out when sending commands across clear without
waiting for prompts, then detected BSD's PENDIN bit. The harness now synchronizes
with prompts and checks all persistent terminal settings. These initial failures
were not treated as passes. Sanitizer validation first failed in unused cJSON;
its excluded scope is explicit above. No compiler warning suppression was added.

## Remaining risks and limitations

- No macOS load engine or port scanner support is implemented or planned here.
- Interface type is Unknown except loopback: inferring Wi-Fi from an `en` prefix
  would be misleading. Enumeration is bounded at 128 entries and reports truncation.
- Existing Linux engine uses shell interpolation in ping, unchecked allocation,
  unsafe signal-handler stdio and thread lifecycle patterns. These are documented,
  not repaired or executed, to honor the engine freeze.
- Existing scanner has thread lifecycle/FD_SETSIZE concerns and a legacy
  non-prototype header declaration. Full Linux Clang strict-build support is not
  claimed. Linux CI uses GCC; portable CLI static analysis uses Clang.
- Signal-interrupted CLI terminal restoration (e.g. SIGINT/SIGTERM), full cursor
  editing semantics, and Unicode grapheme editing remain outside this change.
  Normal exit and EOF restoration are covered; this is not a complete security audit.
- Apple linker reports reduction of the existing large history array's common
  section alignment from 0x8000 to 0x4000. Tests pass; the warning is retained and
  has not been hidden or attributed to a test failure.
- Existing cJSON is not upgraded and is not certified as memory-safe by these tests.
- README's original content is retained; the new bilingual warning and development
  section define the laboratory-only scope. CLI help displays a concise bilingual
  warning without introducing prompts or changing command names.

The CI workflow builds the full Linux executable but never runs it. Tests link
only the diagnostic adapter. Package setup is limited to disposable CI runners;
no system-wide package installation or privilege change occurred on this Mac.
