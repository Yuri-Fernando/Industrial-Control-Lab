# Modbus TCP register map (0-based PDU addressing)

TCP: `127.0.0.1:15020` local or Docker internal `controller:15020`. Unit ID is returned as requested. Register values are unsigned 16-bit big-endian; scaling shown below. **Most SCADA clients show 40001/30001 notation: check whether the client expects zero-based addresses.**

## Holding registers (`FC03` read; `FC06`, `FC16` write)

| Offset (dec) | Tag | Type/range | Meaning |
|---|---|---|---|
| 0 | `CMD` | 0..4 | 0 noop; 1 START; 2 STOP; 3 RESET; 4 ESTOP; nonzero commands queued FIFO |
| 1 | `SP_T_X10` | 240..380 | setpoint 24.0..38.0 °C |
| 2 | `HEAT_LOAD_PCT` | 0..100 | synthetic plant heat-load percentage |
| 3 | `FAULT_INJECT_LOW_FLOW` | 0/1 | synthetic coolant flow fault |
| 4 | `FAULT_INJECT_SENSOR` | 0/1 | synthetic sensor validity fault |
| 5 | `HEARTBEAT_COUNTER` | uint16 | gateway increments every 500 ms |
| 6 | `FAULT_INJECT_OVERTEMP` | 0/1 | synthetic 50°C temperature offset |
| 7..15 | reserved | read-only zero | writes rejected |

`FC16`: all targeted writes are validated before any write is committed (atomic multi-register commit under mutex); invalid writes get exception 03. Off-range reads get exception 02. Unsupported function codes get exception 01. Unsupported values in `FC06` also get exception 03.

## Input registers (`FC04` read)

| Offset | Tag | Scaling / enumeration |
|---|---|---|
| 0 | `STATE` | 0 STOPPED, 1 STARTING, 2 RUNNING, 3 FAULT, 4 ESTOP |
| 1 | `TEMPERATURE_X10` | °C × 10 |
| 2 | `FLOW_X10` | L/min × 10 |
| 3 | `COOLING_X10` | actuator % × 10 |
| 4 | `PUMP_ENABLE` | 0/1 |
| 5 | `FAULT_CODE` | 0 NONE, 1 LOW_FLOW, 2 OVERTEMP, 3 SENSOR_INVALID, 4 WATCHDOG, 5 EMERGENCY_STOP, 6 IO_COMM_LOSS (v0.2) |
| 6 | `MAX_SCHEDULER_LATENESS_US` | saturated uint16 microseconds (positive late wake only) |
| 7 | `MISSED_DEADLINES` | count, saturated to 65535 |
| 8 | `UPTIME_S` | seconds, saturated to 65535 |
| 9 | `WATCHDOG_AGE_MS` | age, saturated to 65535 |
| 10 | `HEARTBEAT_ECHO` | last heartbeat |
| 11 | `SAFETY_TRIP_COUNT` | count, saturated |
| 12 | `CONTROL_ERROR_X10` | signed int16 two's complement, °C ×10 |
| 13 | `SENSOR_VALID` | 0/1 |
| 14 | `SCAN_LATENESS_P50_US` | µs, 60 s sliding window, refreshed 1×/s (v0.2) |
| 15 | `SCAN_LATENESS_P95_US` | µs (v0.2) |
| 16 | `SCAN_LATENESS_P99_US` | µs (v0.2) |
| 17 | `SCAN_LATENESS_MAX_US` | µs, window maximum (register 6 is since boot) (v0.2) |
| 18 | `SCAN_EXEC_P99_US` | µs, input scan → logic → output scan (v0.2) |
| 19 | `SCAN_EXEC_MAX_US` | µs (v0.2) |
| 20 | `IO_MODE` | 0 local (in-process plant), 1 remote I/O node (v0.2) |
| 21 | `IO_HEALTHY` | 0/1: link up and sample age ≤ `IO_TIMEOUT_MS` (always 1 in local mode) (v0.2) |
| 22 | `IO_SAMPLE_AGE_MS` | age of the last *changed* remote sample (v0.2) |
| 23 | `IO_COMM_ERRORS` | remote I/O transaction failures, saturated (v0.2) |
| 24 | `CONTROL_ENGINE` | 0 C++ (`icl::Controller`), 1 IEC 61131-3 ST (`st/FB_Interlocks.st` on stvm) (v0.2) |
| 25 | `RT_ELEVATED` | 1 if `ICL_RT=1` obtained elevated scheduling (v0.2) |
| 26 | `ENGINE_MISMATCHES` | scans where the shadow engine disagreed with the primary (expected 0) (v0.2) |
| 27 | `SHADOW_ACTIVE` | 1 if the other engine runs in shadow for live differential checking (v0.2) |
| 28..31 | reserved | zero |

Register-map version **0.2** only *adds* offsets 14–27 and fault code 6; offsets, scaling and enumerations of v0.1 are unchanged.
FC06/FC16 to an address beyond the table returns exception **02**; an in-range but reserved/invalid value returns **03**.

## Remote I/O node (`io_node`, TCP `127.0.0.1:15021`, Docker `io-node:15021`)

Emulates a Modbus TCP field I/O module wired to the cooling skid. The controller scans it from a dedicated I/O thread
every 20 ms (`IO_MODE=remote`): one FC16 write of holding 0..6, then one FC04 read of inputs 0..9.

| Holding | Tag | Range | Meaning |
|---|---|---|---|
| 0 | `PUMP_CMD` | 0/1 | pump contactor command |
| 1 | `COOLING_CMD_X10` | 0..1000 | cooling valve % × 10 |
| 2 | `HEAT_LOAD_PCT` | 0..100 | synthetic process disturbance |
| 3 | `INJECT_LOW_FLOW` | 0/1 | flow-path blockage at the plant |
| 4 | `INJECT_SENSOR` | 0/1 | transmitter reports invalid |
| 5 | `INJECT_OVERTEMP` | 0/1 | +50 °C offset at the transmitter |
| 6 | `OUTPUT_WATCHDOG` | uint16 | controller changes it every scan; unchanged > 500 ms ⇒ outputs fail-safe OFF |
| 7 | `BENCH_FREEZE` | 0/1 | test bench: sample sequence and values freeze (stuck transmitter) |
| 8 | `BENCH_LATENCY_MS` | 0..2000 | test bench: delay every response (network latency injection) |

| Input | Tag | Meaning |
|---|---|---|
| 0 | `TEMPERATURE_X10` | measured temperature (with injected offset) |
| 1 | `FLOW_X10` | measured flow |
| 2 | `SENSOR_VALID` | 0/1 |
| 3 | `SAMPLE_SEQ` | increments every 10 ms plant tick; a frozen value means stale data |
| 4 | `PUMP_APPLIED` | output actually applied after the fail-safe |
| 5 | `COOLING_APPLIED_X10` | idem |
| 6 | `FAILSAFE_ACTIVE` | 1 while the output watchdog has expired |
| 7 | `FAILSAFE_TRIPS` | number of fail-safe activations |
| 8 | `UPTIME_S` | node uptime |
| 9 | `PLANT_TEMPERATURE_X10` | true process value (diagnostics only) |

Controller-side rules: a sample is **stale** when `SAMPLE_SEQ` has not changed for `IO_TIMEOUT_MS` (default 300 ms) or the
link is down; stale data is reported as `SENSOR_VALID=0`, trips `IO_COMM_LOSS` while STARTING/RUNNING, blocks START and RESET.

## Commands and operating rules

1. Send heartbeat changes at intervals **less than 3 seconds** before START.
2. Send START once, then observe `STATE==RUNNING` (flow interlock).
3. STOP only affects STARTING/RUNNING/STOPPED. FAULT and ESTOP are latched until RESET.
4. On FAULT, clear the synthetic fault cause, restore heartbeat, then RESET.
5. No Modbus commands are safety-rated. Production needs independently qualified hardware E-stop and authorization.

## Design limits

A custom Modbus subset is provided for reproducibility, not full MODBUS conformance/certification. For real fieldbus devices use maintained industry libraries and interoperability tests. Traffic is unauthenticated and unencrypted; Docker/host port mapping remains local only.
