# TCP capacity diagnostics

Example: `127.0.0.1 -Syn --mode connections --fixture --duration 5 --ramp 2`

Existing `-Syn` defaults to connections on port 80. Production no longer links the legacy raw-packet engine.
Options: --port (1–65535), --concurrency (1–256), --rate (1–200 attempts/s),
--duration (1–60 seconds), --timeout-ms (1–10000), --ramp (0–duration seconds),
--hold-ms (0–10000), --listen-backlog (1–128), --fixture, --authorized-lab.
Defaults: port 80, concurrency 32, rate 50, duration 5, timeout 1000 ms,
ramp 2 seconds, hold 100 ms, listener backlog 16. Duration stops new attempts;
pending attempts and held connections drain afterward. Ctrl+C closes sockets.
Non-loopback active targets require explicit --authorized-lab affirmation.

`127.0.0.1 -Syn --mode backlog --duration 5` is read-only and local.
Linux prints namespace-wide TCP extension counters and IPv4/IPv6 SYN_RECV counts.
Before/after snapshots accompany connection tests; unrelated traffic can affect
these counters. macOS kernel queue observations are currently BLOCKED.
Neither ListenDrops nor ListenOverflows proves SYN backlog exhaustion.
The fixture continuously accepts and closes connections: it tests connection
establishment, not sustained application sessions or accept-queue saturation.
No payload throughput is measured. Latency includes event-loop scheduling.

Validation: macOS production compiler and repeated fixture runs pass. Fixture
checks descriptor counts across three invocations and rejects unauthorized remote
targets. Linux execution, deterministic timeout injection, macOS counter collection,
and queue saturation validation remain BLOCKED/unimplemented.
