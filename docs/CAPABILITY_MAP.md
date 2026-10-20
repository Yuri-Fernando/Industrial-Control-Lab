# Capability map — automation engineering topic → evidence

What each engineering topic covered by the lab is backed by, and how far that evidence goes (v0.2, 2026-10-19).

| Topic | Evidence in the repository | Honest status |
|---|---|---|
| Control-system architecture (software, network, distributed I/O) | `docs/ARCHITECTURE.md` (diagram, ADR-001…011, failure table); `io_node`; Compose networks `fieldnet` / `controlnet` | **Delivered in software**: real process/container boundary; no physical hardware |
| Control software in C/C++ | `lib/control`, `src/main.cpp`, `test/test_native` | **Executed and tested** on Windows and Linux |
| Control software in IEC 61131-3 Structured Text | `st/FB_Interlocks.st` executed by `lib/stvm`; `test/test_stvm`; `StEngineIntegrationTest` | **Executed and tested**, equivalence with C++ proven by differential test and live shadow engine; not yet run in OpenPLC/matiec |
| Soft-PLC / vPLC runtime concepts | process image, 50 ms scan, selectable engine, I/O scanning thread, watchdogs | **Own runtime, functional**; not a certified or commercial runtime |
| Industrial protocols over TCP/IP | Modbus TCP server and client (C++), client (Python), FC03/04/06/16, exceptions, fuzz test | **Demonstrated**; in-repo stack, no third-party interoperability test yet |
| Distributed I/O and fault tolerance | stale-sample detection, link loss, latency injection, output watchdog fail-safe | **Demonstrated** across processes and containers |
| IIoT telemetry | Modbus → MQTT (QoS, retained, edge-triggered alarms, last-will), REST, Prometheus | **Demonstrated** (MQTT captured from the Docker stack) |
| Real-time concepts | absolute-deadline sleep, high-resolution timers, `SCHED_FIFO` / `TIME_CRITICAL`, p50/p95/p99, deadline misses, benchmark under CPU stress | **Soft real-time measured**; no PREEMPT_RT, no hard bound claimed |
| Requirements engineering | `docs/REQUIREMENTS.md` (FR-001…030, NFR-001…014, gates A–H) with requirement → test traceability | **Delivered** |
| Software engineering | 6 C++ libraries behind interfaces (`ControlEngine`, `IoChannel`, `ModbusDevice`), 28 Unity + 22 integration tests, CI matrix Linux + Windows, Git | **Delivered** |
| Technical English | `docs/` and `docs/DEMO_SCRIPT.md` | documentation only |

## Design questions the repository answers

* Why keep deterministic control (C++/ST) apart from supervision (Python)? → ADR-003.
* Communication watchdog vs I/O output watchdog vs physical E-stop? → `ARCHITECTURE.md`, failure table.
* How do we know the ST and C++ engines behave identically? → differential test + shadow engine (ADR-009/010).
* What happens when the field network is slow or down? → stale samples, `IO_COMM_LOSS`, dedicated I/O thread (NFR-011).
* Why did Windows add ~8 ms of lateness per scan, and how was it fixed? → ADR-011, `docs/evidence/timing_windows_2026-10-19.json`.
* What are the risks of unauthenticated Modbus TCP? → `DEPLOYMENT_SECURITY.md`.

## Claims this repository does **not** support

Certified PLC, hard real-time, physical industrial sensors, execution in OpenPLC/CODESYS, ST compiled by matiec.
