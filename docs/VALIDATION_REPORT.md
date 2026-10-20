# Validation report

## v0.2.0 — 2026-10-19

### Hosts

| Host | Toolchain | Notes |
|---|---|---|
| Windows 11 Pro (10.0.26300), 6 logical CPUs | PlatformIO Core 6.1.18, MinGW-w64 GCC 15.2.0 (posix threads, no static winpthread), Python 3.10 | the author's VS Code environment; project on a Google Drive virtual disk |
| WSL2 Ubuntu 24.04, kernel 5.15.167.4, 4 vCPUs | GCC 13.3.0, Python 3.12.3 | copy of the sources outside the Drive; no sudo; PlatformIO not installed |
| Docker Desktop 29.8.0 (WSL2 backend) | images `gcc:14-bookworm` → `debian:bookworm-slim`, `python:3.12-slim`, `eclipse-mosquitto:2` | 4-service Compose |

### Directly observed

| Gate | Command | Result |
|---|---|---|
| A — build | `pio run` (Windows): envs `native` + `io_node`; `./scripts/build_native.sh` (WSL) | **PASS**, no compiler warnings with `-Wall -Wextra` |
| C — unit | `pio test -e test` (Windows) | **PASS — 28/28** (`test_native` 10, `test_stvm` 10, `test_modbus` 8) |
| B — integration | `python -m unittest discover -s tests_python -v` | **PASS — 22/22** on Windows and on WSL Ubuntu |
| CI | GitHub Actions run [39452498373](https://github.com/Yuri-Fernando/Industrial-Control-Lab/actions/runs/39452498373) (2026-10-20) | **PASS** on `ubuntu-24.04` and `windows-latest`: `pio run`, 28/28 Unity, plain GCC build (Linux), 22/22 integration |
| D — HMI/acceptance | `scripts/lab.py run --io remote --engine st` + `scripts/lab.py demo` | **PASS — 6/6**, `engine_mismatches = 0` ([evidence](evidence/acceptance_remote_st_2026-10-19.json)) |
| E — de-energize on faults | integration suites (local, ST, remote) | **PASS** — LOW_FLOW, SENSOR, OVERTEMP, WATCHDOG, ESTOP, IO_COMM_LOSS, I/O-node fail-safe |
| F — Docker + MQTT | `docker compose up --build -d`, demo against `:8088`, `mosquitto_sub -t 'icl/thermal/#'` | **PASS — 6/6**; topics telemetry/state/alarm/gateway-status received ([capture](evidence/mqtt_capture_docker_2026-10-19.txt), [acceptance](evidence/acceptance_docker_2026-10-19.json)); gateway container cannot resolve/reach `io-node` (field network isolation) |
| G — ST + distributed I/O | `test_stvm` differential (200 000 random + 6 000 closed-loop scans), `StEngineIntegrationTest`, `RemoteIoIntegrationTest` | **PASS** |
| Timing | `scripts/lab.py bench --seconds 15` | recorded in [timing_windows](evidence/timing_windows_2026-10-19.json), [timing_linux_wsl](evidence/timing_linux_wsl_2026-10-19.json) |

### Defects found and fixed during validation

1. Windows build failure (POSIX headers; `std::clamp` with 32-bit `long`).
2. ST block let STOP clear a latched FAULT.
3. ST adapter passed unsaturated `dt` to the PI (caught by the differential test at step 319).
4. Windows scan lateness p50 ≈ 8–12 ms (default timer); fixed with an absolute-deadline high-resolution timer.
5. v0.1 Docker image could not start (`GLIBCXX_3.4.32` missing in `bookworm-slim`); fixed with static C++ runtime.
6. Mosquitto bind mount fails from a Google Drive folder; replaced with an inline Compose config.
7. `pio test -e native` wrote the Unity runner to `.pio/build/native/program.exe`, so the stack and the integration
   tests launched the test binary instead of the controller. Unit tests now live in a dedicated `test` env.
8. "Access denied" on `program.exe`: when an integration `setUpClass` failed, `unittest` skipped `tearDownClass`
   and the spawned I/O nodes kept running, locking the executable. Fixed with `addClassCleanup`; `lab.py` now puts
   the stack in a kill-on-close Job Object (Windows) / `PR_SET_PDEATHSIG` (Linux). The checkout also lives on a
   Google Drive folder, whose sync removed `.pio/build/native` once; build outputs moved to a local disk through
   the optional, gitignored `platformio_local.ini` (`build_dir`).
9. The HMI displayed last-known flow/temperature as live values during `IO_COMM_LOSS` (found in the evidence
   screenshot); values from an unhealthy I/O link are now flagged `STALE` and the mimic stops animating.

### Not validated in this revision

- `st/FB_Interlocks.st` under matiec or OpenPLC Runtime.
- SCHED_FIFO with privilege (WSL user lacks `CAP_SYS_NICE`; the controller reports `rt_elevated = 0`), PREEMPT_RT.
- Physical I/O, physical safety, interoperability with third-party Modbus masters/slaves.

No capability absent from the "directly observed" table is asserted to be tested.

## v0.1.0 — 2026-10-17 (original package)

Linux x86_64 container (Debian 13, GCC, Python 3.13.5): GCC build PASS, 11 integration tests PASS, 5/5 REST
acceptance scenarios PASS ([sample](ACCEPTANCE_EVIDENCE_SAMPLE.json)). PlatformIO, Docker, MQTT and ST were not run.
In v0.2 it became clear that the v0.1 Docker image would not have started (defect 5 above).
