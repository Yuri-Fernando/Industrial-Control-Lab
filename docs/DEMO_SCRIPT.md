# Live technical demo — English script (5–7 minutes)

## 0:00–0:45 — Problem statement

"I built this proof of concept to demonstrate how I approach industrial automation software when physical hardware is unavailable. The application is a virtual thermal cooling loop for scientific infrastructure. Its main objective is not graphical visualization: it is control software architecture, process interfaces, fault handling and testability."

## 0:45–1:45 — Architecture

"The controller runs in a separate Linux C++ process, with a periodic 50-millisecond control task. A Modbus TCP register interface exposes process data. A Python edge gateway bridges these registers into MQTT messages and a browser-based supervisory interface. This separation keeps the control loop independent from the telemetry and UI layers."

Show: architecture diagram (`docs/ARCHITECTURE.md`), `lib/control`, `lib/modbus`, `gateway`.

## 1:45–3:00 — Control behavior

Show: START -> STARTING -> RUNNING; setpoint changes; watch temperature and PI output.

"The state machine starts the pump, verifies minimum coolant flow and then allows the PI temperature control to run. The PI loop includes output saturation and anti-windup; the plant follows a first-order thermal model with a synthetic heat load."

## 3:00–4:15 — Failure management

Show: trigger LOW_FLOW -> latched FAULT -> STOP does not clear it -> release fault -> RESET.

"I deliberately validate that an ordinary stop command cannot clear a latched interlock. The simulation also handles sensor invalidity, high temperature, startup flow timeout and communication watchdog expiration. The emergency stop is a **software simulation only**, not a physical safety function."

## 4:15–5:30 — Engineering quality

Show: tests, register map, traceability table, `GET /metrics`.

"I added black-box integration tests that start the real compiled C++ executable, communicate over real TCP sockets and verify the Modbus control and fault semantics. Requirements are traceable to code and tests. Monitoring records scheduler wake lateness and deadline misses; I don't claim hard real-time performance from an ordinary Linux scheduler."

## 5:30–6:30 — Structured Text and distributed I/O

Run `python scripts/lab.py run --io remote --engine st --rt`.

"Now the interlock logic executing every scan is the IEC 61131-3 Structured Text block, interpreted by a small ST runtime I wrote in C++. The C++ engine runs in shadow on the same inputs, and this counter shows zero disagreements. In CI the two engines are compared step by step over two hundred thousand randomized scans."

"The plant now lives in a separate remote I/O process that speaks Modbus TCP. If I kill it, the controller sees stale data within 300 milliseconds and latches IO_COMM_LOSS. If I kill the controller instead, the I/O node's output watchdog drops the pump on its own after 500 milliseconds — the same fail-safe pattern as a fieldbus I/O module."

## 6:30–7:00 — Timing evidence

"On Windows, the standard sleep had 8 milliseconds median lateness because of the default timer granularity. Switching to an absolute-deadline high-resolution timer brought p99 to about 0.6 milliseconds even with all cores loaded. On Linux I use clock_nanosleep with TIMER_ABSTIME; without CAP_SYS_NICE SCHED_FIFO is refused, and the tool reports that instead of hiding it. None of this is a hard real-time claim."

## FAQ — design questions

**Q: Is this a SoftPLC?**
A: It is a *software PLC-style runtime*: process image, cyclic scan, selectable C++ or ST logic, I/O scanning of a remote node. It is not a certified runtime, and the ST interpreter is a documented subset, not an IEC-conformant compiler. The next step is running the same `.st` file in OpenPLC.

**Q: Why write your own ST interpreter instead of using OpenPLC?**
A: To execute the real ST file inside my scan and run differential tests in CI on Windows and Linux without extra infrastructure. It also forced me to implement IEC operator precedence and type rules precisely. The file stays portable for OpenPLC.

**Q: Why not implement the control in Python?**
A: To give the cyclic control and protocol transport an explicit native boundary, with tighter overhead control and a direct migration path to embedded C++.

**Q: Is Modbus secure?**
A: Plain Modbus TCP doesn't provide inherent authentication, authorization or transport confidentiality. The PoC binds ports only to localhost and must be hardened/segmented before any deployment.

**Q: How do you prove deterministic deadlines?**
A: I don't claim a hard bound with general-purpose Linux. I log scheduling lateness, plan p99/max latency campaigns, measure control execution time and assess PREEMPT_RT / system priorities if needed.

**Q: Is the distributed I/O real?**
A: The I/O node is a separate process (a separate container in Docker, on a field network the gateway cannot reach), so the closed loop really crosses a network boundary with real failure modes. The instruments themselves are simulated; replacing the node with an ESP32 implementing the same register contract is the hardware-in-the-loop step.
