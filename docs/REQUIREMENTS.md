# System requirements specification (SRS)

**Version:** 0.2 · **Status:** PoC with ST runtime and remote I/O · **Scope:** virtual scientific cooling loop.

## Stakeholders

Automation engineer (requirements/control), controls software engineer (C++/ST), OT/IT engineer (network integration), commissioning engineer (acceptance), operator (supervision and fault acknowledgment), quality/test engineer (traceability).

## Functional requirements and traceability

| ID | Priority | Verifiable requirement | Component / proof |
|---|---|---|---|
| FR-001 | Must | Start with pump OFF, no active fault | `Controller`, integration status initial |
| FR-002 | Must | Command START enters STARTING and RUNNING only after flow ≥5 L/min | `Controller`, test_01 |
| FR-003 | Must | Run 20 Hz periodic logical scans in Linux | `src/main.cpp`, tick and jitter metrics |
| FR-004 | Must | Enforce thermal setpoint 24–38°C and PI cooling output 0–100% | `Controller`, Modbus validation, test_06 |
| FR-005 | Must | Expose process and diagnostics via Modbus TCP FC03/04 | register bank, test_09 |
| FR-006 | Must | Accept writable control words via FC06; FC16 is atomic on valid ranges | `RegisterBank`, protocol tests |
| FR-007 | Must | Trip to latched FAULT upon low coolant flow while running | `Controller`, test_02 |
| FR-008 | Must | Trip to FAULT upon invalid temperature sensor | `Controller`, test_03 |
| FR-009 | Must | Trip to FAULT on measured temperature ≥45°C | `Controller`, test_04 |
| FR-010 | Must | Emergency stop simulation enters latched ESTOP | `Controller`, test_05 |
| FR-011 | Must | Stop/reset commands cannot silently bypass latched safety trips | `Controller`, test_02/test_05 |
| FR-012 | Must | Detect absence of communication heartbeat >3 seconds | `Controller`, test_08 |
| FR-013 | Must | Raise low-flow fault if STARTING fails to get flow in 2 seconds | `Controller`, test_07 |
| FR-014 | Must | Publish JSON telemetries and alarms to MQTT | `Gateway._poll` (requires configured broker) |
| FR-015 | Must | Expose local dashboard and REST command/status endpoints | Python HTTP gateway, browser UI |
| FR-016 | Must | Expose numeric Prometheus-style metrics | `GET /metrics` |
| FR-017 | Must | Enable synthetic fault injection in the UI | gateway/Modbus registers 3,4,6 |
| FR-018 | Must | Separate control-domain and supervisory-domain execution | C++ process, Python process |
| FR-019 | Must | Maintain requirements, interface contracts and tests | `docs/`, `tests_python/` |
| FR-020 | Should | Document equivalent interlock logic in IEC 61131-3 ST | `st/FB_Interlocks.st`, compiled by stvm (`test_project_st_block_compiles`) |
| FR-021 | Must (v0.2) | Execute the ST interlock block in the scan cycle and prove equivalence with the C++ engine | `CONTROL_ENGINE=st`, `lib/stvm`; `test_stvm` differential (200 000 random steps + closed loop); `StEngineIntegrationTest`; live `ENGINE_MISMATCHES` register. *OpenPLC/matiec execution still future (FR-021b)* |
| FR-022 | Must (v0.2) | Plant sensors/actuators live in an independent remote I/O node reached over Modbus TCP | `src/io_node`, `RemoteIo`; `RemoteIoIntegrationTest` |
| FR-023 | Future | Connect real instruments with qualified safety controls | roadmap P3 |
| FR-024 | Must (v0.2) | Trip latched `IO_COMM_LOSS` when the remote I/O link drops or samples are stale > `IO_TIMEOUT_MS`; block START/RESET while unhealthy | `Controller`, `test_io_comm_loss_*`, remote test_02/03/04 |
| FR-025 | Must (v0.2) | Remote I/O node de-energizes outputs when the controller output watchdog is silent > 500 ms | `IoNodeDevice::tick`, `test_io_node_output_watchdog_failsafe`, remote test_99 |
| FR-026 | Must (v0.2) | Tolerate moderate network latency (60 ms/response) without trip; trip on excessive latency | remote test_04 |
| FR-027 | Must (v0.2) | Publish scan lateness p50/p95/p99/max and execution p99/max over a 60 s window | input registers 14–19, `/metrics`, `test_10_timing_and_shadow_diagnostics`, `lab.py bench` |
| FR-028 | Should (v0.2) | Optional real-time tuning of the scan thread (`ICL_RT=1`) with explicit report of the outcome | `apply_realtime_tuning`, register 25 |
| FR-029 | Should (v0.2) | MQTT: edge-triggered alarm topic and gateway last-will/online status | `icl/thermal/alarm`, `icl/thermal/gateway/status` |
| FR-030 | Must (v0.2) | Build and run natively on Windows (MinGW-w64) and Linux/WSL from the same sources | `lib/net`, `scripts/pio_host_libs.py`, CI matrix |
| FR-021b | Future | Compile the same ST with matiec / run in OpenPLC Runtime and repeat the differential matrix | roadmap P1.2 |

## Nonfunctional requirements

| ID | Statement | Verification / limits |
|---|---|---|
| NFR-001 | Base scan period = 50 ms | read code and instrument cycle timing; no hard-RT guarantee |
| NFR-002 | Timestamp telemetry in UTC ISO 8601 | inspect MQTT payload/REST |
| NFR-003 | Expose communication/sensor quality states | REST JSON and input register 13 |
| NFR-004 | Reject invalid Modbus registers/values | protocol negative tests |
| NFR-005 | Command operations are serialized by FIFO, max 32 outstanding commands | unit/integration plus source review |
| NFR-006 | Runtime services separable and containerizable | `compose.yaml` (validate Docker on target) |
| NFR-007 | All Python gateway operations work without third-party packages when MQTT is disabled | `requirements.txt` only MQTT; local Python tests |
| NFR-008 | Avoid public binds by default in local and host-exposed Docker mode | `127.0.0.1` port binds and deployment instructions |
| NFR-009 | Technical documentation and demonstration script in English | `docs/`, `docs/DEMO_SCRIPT.md` |
| NFR-010 | Simulated safety only; cannot substitute physical safety engineering | warnings in code/docs/UI |
| NFR-011 | A slow or dead field network never blocks the control scan | remote I/O scanned by a dedicated thread; scan reads the latest sample (remote tests keep the controller responsive) |
| NFR-012 | Modbus parser survives malformed frames | `test_fuzz_random_frames_never_crash` (20 000 random frames), session quota (16) |
| NFR-013 | ST runtime faults are fail-safe | `test_st_runtime_error_fails_safe` |
| NFR-014 | One ST scan executes a bounded number of statements (no loops in the supported subset) | `lib/stvm/stvm.hpp` design note |

## Acceptance criteria

* Gate A: `./scripts/build_native.sh` completes on Linux with GCC/C++17.
* Gate B: `python3 -m unittest discover -s tests_python -v` passes real TCP integration.
* Gate C: `pio run -e native` and `pio test -e test` pass in a PlatformIO-configured Linux/WSL environment.
* Gate D: dashboard shows telemetry and responds to START, STOP and fault injections while gateway remains connected.
* Gate E: remote stop and connectivity faults de-energize virtual outputs according to state-machine rules.
* Gate F: when Docker is available, `docker compose up --build` exposes only host-loopback management ports and publishes telemetry to broker.
* Gate G: ST executed in the scan with differential evidence; remote I/O faults and latency quantified (v0.2, see `docs/VALIDATION_REPORT.md`).
* Gate H (future): same ST compiled by matiec / executed by OpenPLC Runtime.

Check `docs/VALIDATION_REPORT.md` for which gates were observed, on which host and date.
