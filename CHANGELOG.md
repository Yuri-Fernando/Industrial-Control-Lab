# Changelog

## 0.2.0 — 2026-10-19

### Fixed
- Build: project did not compile on Windows (POSIX-only sockets) nor with 32-bit `long` (`std::clamp(long long, long, long)` in `src/main.cpp`).
- ST: `FB_Interlocks.st` allowed STOP to clear a latched FAULT (`StopRequest AND State <> 4`), violating invariant 2. ST now also implements the startup flow timeout, safety-trip counter and I/O health, matching the C++ engine.
- Gateway Modbus client opened a new TCP connection for every request (3 per 0.5 s); now persistent with reconnection.
- Windows scan lateness p50 ≈ 8 ms caused by default timer granularity; absolute-deadline high-resolution sleep brings it to ≈ 0.3 ms.
- Docker image could not start (`GLIBCXX_3.4.32`); C++ runtime now linked statically in the image.
- `pio test` overwrote the controller executable; unit tests moved to a dedicated `test` env.
- Integration fixture leaked processes when `setUpClass` failed (`addClassCleanup`); `lab.py` children die with it.
- HMI showed stale field values as live during `IO_COMM_LOSS`; now flagged `STALE`.

### Added (additive modules; v0.1 register offsets unchanged)
- `lib/net`: portable socket layer (POSIX / Winsock2); PlatformIO hook `scripts/pio_host_libs.py`.
- `lib/stvm`: IEC 61131-3 ST subset interpreter + `StController`; `CONTROL_ENGINE=cpp|st`; shadow engine with live mismatch counter.
- `lib/io` + `src/io_node`: remote I/O node over Modbus TCP (`IO_MODE=remote`), stale-sample detection, output watchdog fail-safe, freeze/latency test bench; fault code 6 `IO_COMM_LOSS`.
- `lib/timing`: sliding p50/p95/p99/max lateness and execution time, `ICL_RT=1` tuning, `sleep_until_precise`.
- Input registers 14–27; MQTT alarm topic and gateway LWT; HMI process mimic, runtime and timing panels;
  `GET /api/history` (server-side trend preloaded by the HMI).
- `scripts/plot_timing.py`, HMI screenshots and timing chart in `docs/img/`; `docs/CAPABILITY_MAP.md`, `docs/DEMO_SCRIPT.md`.
- `scripts/lab.py` (build / run / test / demo / bench), VS Code tasks, Compose with separate `io-node` service and field network, CI matrix Linux + Windows.
- Tests: Unity suites `test_native` (10), `test_stvm` (10), `test_modbus` (8); integration suites for ST engine and remote I/O (22 total).

## 0.1.0 — 2026-10-17

- Linux C++ virtual controller + PI thermal model.
- Cyclic scheduler, heartbeat, safety interlocks and status metrics.
- Modbus TCP server with register map and atomic multi-register write.
- Python gateway with REST supervisor, MQTT publish adapter and metrics.
- IEC Structured Text interlock reference source (not runtime validated).
- CI configuration, requirements, architecture, tests, deployment and roadmap.
