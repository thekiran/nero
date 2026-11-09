# Quality plan and acceptance gates

Updated 2026-11-09. Starting point: `56598f0` on main. A score is not a release
criterion: NERO earns a stronger assessment through reproducible evidence for
its existing TCP and CLI contracts. This work does not certify the raw engine.

| Stage | Work / observable gate | Current status |
| --- | --- | --- |
| 0 Baseline | Record source SHA, compiler, commands and failures before editing | Linux baseline recorded; native interface test exposed a host/sysfs assumption |
| 1 TCP correctness | Every controlled known listener found; closed, filtered, unknown and unscanned remain distinct; full runs account for 65,535 ports | Synthetic faults and real loopback checks implemented; known listener asserted in 3 full headless runs, plus 2 CLI runs; isolated drop/reject lab still pending |
| 2 Session ownership | Terminal-independent production core; copied configuration; independent simultaneous sessions; explicit test I/O boundary | Implemented in scan_session.c; CLI adapts it; fixtures link production objects without including .c files |
| 3 Lifecycle | Cancellation/early stop stay distinct, pending sockets close, workers join, errors stay bounded | Synthetic clock/resource/thread faults, live stop, busy/repeat rejection, PTY handoff and descriptor baseline checks implemented; sanitizer outcomes recorded separately |
| 4 Installation/platforms | Native build, install/upgrade/failure/uninstall, normal/sudo lookup, fresh Windows PATH on every claimed package | Staged lifecycle covered locally; five native package/installer jobs remain required in PR CI; Linux-only results do not certify macOS/Windows |
| 5 Diagnostics/docs | Explain a report using version, operation ID, settings, status and grouped errors; distinguish historical evidence | Start log includes version/profile/timeouts/budget; worker failures counted; early selection logs SCAN_PARTIAL; historical reports marked |
| 6 Release | All required checks on the immutable candidate, reviewed checksums/installers, no open high/critical correctness defects | Pending platform CI and final review; no new tag or release publication requested by this change |

## Work order

Correctness regressions precede the architecture refactor. The same production
core is used by the CLI, headless tests and real-loopback fixture. Platform
integration checks remain separate from deterministic selection/parser tests.
Do not weaken or skip required native gates to make this environment green.

Next: run the branch's five native package jobs, inspect failures against the
actual tested SHA, then run an isolated Linux drop/reject network lab and a real
LAN/mobile-device test. For those tests record the known listener set, profile,
network topology and uncertainty; no benchmark can promise zero misses on an
arbitrary changing network. External review and repeated user-facing releases
remain necessary maturity evidence.

## Assessment rubric

| Area | Weight |
| --- | ---: |
| TCP correctness and honest uncertainty | 30% |
| Cancellation, resource ownership and concurrency | 20% |
| Meaningful tests and native CI evidence | 20% |
| Maintainable session/platform interfaces | 15% |
| Installation, diagnostics and documentation | 15% |

A 9/10 assessment also requires all release gates: documentation points cannot
offset a known critical correctness defect. No 10/10 or defect-free claim is
made. See [this implementation's validation record](quality-validation.md).

## Technical references

- [connect(2)](https://man7.org/linux/man-pages/man2/connect.2.html): after writable readiness, SO_ERROR determines asynchronous connection success.
- [poll(2)](https://man7.org/linux/man-pages/man2/poll.2.html): readiness and hangup/error events are separate; waits can overrun under scheduling.
- [pthread_join(3)](https://man7.org/linux/man-pages/man3/pthread_join.3.html): joining workers establishes their completion before queue storage is released.

The POLLOUT + SO_ERROR=0 + POLLHUP regression is an application of these
contracts; HUP alone and POLLERR without success evidence remain inconclusive.
