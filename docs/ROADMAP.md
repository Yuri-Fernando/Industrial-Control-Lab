# Engineering roadmap — from virtual PoC to industrial-grade laboratory demonstrator

## Phase 0 — Reproducible functional baseline

- [x] C++17 control kernel and thermal plant model
- [x] Virtual I/O and 20 Hz cyclic execution
- [x] Modbus TCP read/write interface with register validation
- [x] Python Modbus-to-REST/MQTT gateway, browser supervisor and fault injection
- [x] Watchdog, overtemperature, sensor and flow interlocks
- [x] SRS, register map, architecture, safety boundary, CI config
- [x] PlatformIO CLI build + Unity tests on the target Windows machine (v0.2)
- [x] Linux (WSL Ubuntu, GCC 13) build + integration tests (v0.2)
- [ ] CI pipeline execution after publishing to GitHub

## Phase 1 — Core automation capabilities

- [x] **P1.1 ST execution:** `FB_Interlocks.st` executed in the scan by stvm; static type checks; ST unit tests (v0.2).
- [ ] **P1.1b External ST toolchain:** compile with matiec / OpenPLC Editor (needs Linux with sudo for bison/flex).
- [x] **P1.2 Differential testing:** ST vs C++ step-by-step (random + closed loop) and live shadow engine (v0.2).
- [ ] **P1.2b PLC interoperation:** OpenPLC Runtime running the same block against the remote I/O node.
- [x] **P1.3 Distributed I/O:** separate `io_node` process/container; stale samples, link loss, latency injection, output watchdog fail-safe (v0.2).
- [x] **P1.4 Schedule characterization:** p50/p95/p99/max lateness + execution time; idle vs CPU-stress, RT on/off, Linux and Windows (v0.2).
- [~] **P1.5 Protocol hardening:** frame-level fuzz test, session quota, persistent client with reconnection (v0.2). Pending: libmodbus interoperability, unit-id policy, conformance tests.
- [ ] **P1.6 Security:** authenticated operator commands, TLS/proxy, broker ACLs, OT/IT segmentation, audit trail.

## Phase 2 — Experimentation and scientific engineering

- [ ] PREEMPT_RT kernel + CPU isolation (`isolcpus`, IRQ affinity) and repeat `lab.py bench`.
- [ ] Digital twin identification: calibrate cooling coefficients from controlled synthetic data.
- [ ] Discrete PI vs PID with noise, actuator saturation and sample-rate studies.
- [ ] Network fault matrix: packet loss / jitter with `tc netem` on the Docker field network.
- [ ] OPC UA server adapter (open62541) beside Modbus for the supervisory layer.
- [ ] Persistent TSDB + Grafana dashboard fed by `/metrics`.

## Phase 3 — Optional embedded / hardware-in-the-loop

- [ ] ESP32/STM32 PlatformIO environment implementing the remote I/O register contract on real hardware.
- [ ] Physical temperature sensor and simulated output load with qualified isolation circuits.
- [ ] Independent safety chain and engineering review before operating any real refrigeration equipment.

## Prioritization

Highest remaining engineering value per unit of effort: P1.1b/P1.2b (external IEC runtime), P1.6 (security), then PREEMPT_RT measurements. Resist adding unrelated AI or cloud features before these.
