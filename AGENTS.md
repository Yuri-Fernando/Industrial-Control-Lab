# Agent collaboration instructions

You are modifying **Industrial Control Lab**, a hardware-free automation/control PoC (soft-PLC, IEC 61131-3 ST, Modbus TCP distributed I/O, IIoT telemetry). Keep engineering evidence factual. Preserve the C++ control loop and deterministic state-machine semantics; do not casually move loop logic into Python.

## Invariants

1. Physical safety is NEVER claimed. Simulated ESTOP is not a safety-rated emergency stop.
2. A FAULT/ESTOP is latched and cannot be cleared by STOP. Explicit RESET with cleared cause required.
3. Controller output must be de-energized on latched fault.
4. Control and supervision run in separate processes.
5. Do not break fixed register addresses, endianness or 0.1 scaling without versioning register-map protocol.
6. Each new requirement needs a traceable ID and tests.
7. Each new failure mode needs an integration test across actual Modbus TCP communication.
8. PlatformIO Native builds on Linux/WSL and Windows (MinGW-w64) through `lib/net`; keep platform `#ifdef`s inside `lib/net` and `lib/timing`. Embedded targets must live in separate adapters/envs.
9. `st/FB_Interlocks.st` is executed by stvm (a documented ST subset, not a conformant IEC compiler). Never claim OpenPLC/matiec execution until it is tested.
10. ST and C++ engines must stay semantically identical: any change to one requires the same change to the other and a green `test_stvm` differential run; live `ENGINE_MISMATCHES` must stay 0.
11. The control scan must never block on the network; field I/O goes through `IoChannel` (remote I/O on its own thread).
12. Avoid overwriting existing user files; prefer incremental changes with `git diff` and changelog entries.

## Development gates

```bash
pio run                      # controller (env native) + remote I/O node (env io_node)
pio test -e test           # Unity: control, ST runtime + differential, Modbus/fuzz/I-O node
python scripts/lab.py test   # all of the above + real-socket integration (local, ST engine, remote I/O)
```

Use `docs/REQUIREMENTS.md` and `docs/ROADMAP.md` as the source of truth. Prefer closing Phase 1 gaps (IEC ST execution, remote distributed I/O, scheduling metrics, protocol hardening) over adding unrelated frameworks or AI features.
