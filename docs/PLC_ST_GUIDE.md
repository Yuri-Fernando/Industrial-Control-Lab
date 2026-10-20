# Structured Text (IEC 61131-3) in the lab

## Current status (v0.2)

`st/FB_Interlocks.st` is the cooling-loop interlock state machine written in portable IEC 61131-3 ST
(`FUNCTION_BLOCK`, `VAR_INPUT/OUTPUT/VAR/VAR CONSTANT`, `IF/ELSIF`, `CASE`, `LIMIT`).

It is **compiled and executed** by `stvm` (`lib/stvm`), a compact ST-subset interpreter written for this project:

| What | Evidence |
|---|---|
| The file parses and type-checks | `test_project_st_block_compiles` |
| It runs in the 50 ms scan as the control engine | `CONTROL_ENGINE=st` / `python scripts/lab.py run --engine st`; register 24 = 1 |
| Its semantics equal the C++ engine | `test_differential_random_inputs` (8 seeds × 25 000 steps, all 5 states and 7 fault codes reached), `test_differential_closed_loop_with_plant` |
| Equivalence holds live | the C++ engine runs in shadow; `ENGINE_MISMATCHES` (register 26) must stay 0 |
| Runtime errors fail safe | `test_st_runtime_error_fails_safe` |

Two defects were fixed in v0.2: code review found that the v0.1 ST let **STOP clear a latched FAULT**
(`StopRequest AND State <> 4`, now `State ∈ {STOPPED, STARTING, RUNNING}`) — the differential test now locks that
behaviour down — and the differential test itself caught an adapter bug where the PI received an unsaturated `dt`.

## stvm — supported subset and deviations

* POUs: `FUNCTION_BLOCK`, `PROGRAM`. Types: `BOOL`, `INT` (16-bit wrap), `DINT` (32-bit), `REAL` (rounded to float32), `LREAL`.
* Statements: `:=`, `IF/ELSIF/ELSE`, `CASE` (integer labels, lists, ranges, `VAR CONSTANT` labels), `RETURN`.
* Operators with IEC precedence: `OR`, `XOR`, `AND`/`&`, `= <>`, `< > <= >=`, `+ -`, `* / MOD`, unary `-`/`NOT`.
* Built-ins: `ABS`, `MIN`, `MAX`, `LIMIT`, `SEL`.
* Static checks: undeclared names, BOOL/number mismatch, implicit narrowing (REAL→INT), writes to inputs/constants.
* **Not supported:** loops, timers/counters (`TON`, `CTU`), user functions, arrays, structs, strings, `TIME`, bitwise integer logic. One scan therefore executes a bounded number of statements.
* stvm is **not** an IEC 61131-3 conformant compiler and is not a certified PLC runtime.

## Next step: an external IEC toolchain (FR-021b)

1. Compile the same file with [matiec / iec2c](https://github.com/beremiz/matiec) (`iec2c FB_Interlocks.st`) — syntax and code generation evidence.
2. Load it in the [OpenPLC Runtime](https://github.com/Autonomy-Logic/openplc-runtime) / Editor, map I/O to the Modbus contract and repeat the differential matrix against `icl::Controller`.
3. Collect cycle-time telemetry from that runtime and compare with `lab.py bench`.

matiec needs `bison`, `flex` and autotools (Linux/WSL with sudo); it was not available on the validation host.
