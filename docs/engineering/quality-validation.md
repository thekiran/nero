# Scanner session validation — 2026-11-09

Baseline: main `56598f0`. Candidate: the commit containing this record; use the
PR head SHA and CI links to identify native platform results. Environment:
Debian 13 x86-64, GCC 14.2, CMake 4.4.4 installed in a temporary user directory.
No remote targets or raw packet engine were exercised.

## Changes under test

- Production connect/poll engine extracted to `src/modules/scan_session.c`.
  Each session owns target/profile copies, result ledger, work storage, counters,
  mutex, cancellation and completion. The CLI remains interactive and owns its
  terminal, optional discovery and banner UI.
- Public `include/nero/scan_session.h` supports one-shot synchronous runs,
  concurrent snapshots, cancel and early stop. Destroy requires all callers to
  have returned. The CLI's signal flag is an adapter input; headless sessions
  neither install signal handlers nor share that flag.
- Test-only I/O injection is per session. All three former source-inclusion
  fixtures now link the production core and adapter as ordinary object files.
- Successful writable/SO_ERROR=0 connections survive an accompanying hangup.
  HUP-only and error events do not become open. A temporary old-policy build
  fails the new regression (exit 1); the corrected build passes it.
- Worker-creation failures increment diagnostics while the caller still drains
  the queue. Monotonic clock failure stops and fails the run, rather than using
  an uninitialized timestamp. Early stop remains partial in both output/logs.
- Linux enumeration falls back from missing/empty sysfs to getifaddrs, releases
  the list, deduplicates names, and reports failure if native APIs are denied.
- Removed unused curl/external-cJSON/ldns/ncurses link dependencies after source
  inspection and successful native linkage using libc/pthreads and bundled code.

## Checks

Final local outcomes: Make regression PASS; CMake/CTest **14/14 PASS**;
ASan/UBSan PASS with the explicit LeakSanitizer limitation; TSan PASS for
session/scanner/real-loopback/PTY/logger fixtures; GCC static analysis PASS.
The Linux production binary and SIGINT/SIGTERM (130/143) checks passed.

| Check | Scope / acceptance |
| --- | --- |
| `make test` | CLI/parser/history/logging, staged installer, deterministic Linux metadata, core/session faults, real loopback and PTY regressions |
| `cmake --preset linux`, build and `ctest --preset linux` | Same production core through CMake, all registered test groups |
| `ASAN_OPTIONS=detect_leaks=0 make test-sanitize` | ASan/UBSan; LeakSanitizer explicitly excluded for the restriction below |
| `make test-tsan` | Session, synthetic scanner, real loopback, PTY and concurrent logger fixtures |
| GCC `-fanalyzer` | Core, adapter and Linux enumeration with strict warnings |
| Native production build + version/EOF/signal smoke | Actual Linux executable, without invoking raw traffic |

The real runtime fixture guards every connect against non-loopback addresses.
It checks a known listener in 3/3 full public-API scans and performs two complete
CLI scans, including partial thread-creation failure, plus a known closed port,
menu EOF/selection, terminal restoration and final descriptor count.

## Environment limits (not passes)

- Real interface enumeration cannot run here: `/sys/class/net` is absent and
  getifaddrs/if_nameindex return EPERM. Deterministic production-adapter fixtures
  exercise selection and fallback. The unchanged native behavior is separately
  asserted by `tests/linux/interface_regression.py` in both Linux CI paths.
- LeakSanitizer itself fails under this environment's process tracing. The
  initial default sanitizer run records that failure. ASan/UBSan are rerun with
  leak detection explicitly disabled; socket-counter and descriptor-baseline
  assertions remain enabled. This is not a LeakSanitizer certification.
- Native `.deb` generation was attempted but blocked because this minimal host
  lacks dpkg dependency metadata for its installed libc; dpkg-shlibdeps correctly
  refuses to infer dependencies. No package was declared valid or installed.
  Native installer lifecycle remains mandatory on disposable platform CI.
- Clang is unavailable locally; GCC static analysis is additional evidence, not
  a substitute for mandatory Clang CI checks. No Windows/macOS native result is
  inferred from Linux. Signing/notarization, real mobile devices, isolated packet
  drop/reject rules and arbitrary remote network reliability remain unverified.
