(* IEC 61131-3 Structured Text — cooling-loop interlock state machine.
   Executed in the scan cycle by the stvm runtime when CONTROL_ENGINE=st, and differential-tested
   step-by-step against the C++ engine (lib/control) in test/test_stvm.
   Semantics must stay identical to icl::Controller; limits mirror icl::limits.
   Portable ST only (no vendor extensions) so it can also be imported into OpenPLC / matiec.
   Physical E-stop MUST be implemented independently of this software. *)
FUNCTION_BLOCK FB_Interlocks
VAR_INPUT
    RunRequest : BOOL;
    StopRequest : BOOL;
    ResetRequest : BOOL;
    EStopRequest : BOOL;
    TemperatureC : LREAL;
    FlowLpm : LREAL;
    LowFlowFaultActive : BOOL;
    SensorValid : BOOL := TRUE;
    HeartbeatHealthy : BOOL := TRUE;
    IoHealthy : BOOL := TRUE;      (* remote I/O link up and samples fresh *)
    CycleTimeS : LREAL := 0.05;    (* scan period, seconds *)
END_VAR
VAR_OUTPUT
    PumpEnable : BOOL;
    CoolingEnable : BOOL;
    State : INT := 0;              (* 0 STOPPED, 1 STARTING, 2 RUNNING, 3 FAULT, 4 ESTOP *)
    FaultCode : INT := 0;          (* 1 LOW_FLOW, 2 OVERTEMP, 3 SENSOR, 4 WATCHDOG, 5 ESTOP, 6 IO_COMM *)
    SafetyTrips : DINT := 0;
END_VAR
VAR
    StartupElapsedS : LREAL := 0.0;
    Dt : LREAL;
    TripCode : INT;
END_VAR
VAR CONSTANT
    ST_STOPPED : INT := 0;
    ST_STARTING : INT := 1;
    ST_RUNNING : INT := 2;
    ST_FAULT : INT := 3;
    ST_ESTOP : INT := 4;
    OVERTEMP_C : LREAL := 45.0;
    MIN_FLOW_LPM : LREAL := 5.0;
    STARTUP_TIMEOUT_S : LREAL := 2.0;
END_VAR

Dt := LIMIT(0.0, CycleTimeS, 0.2);

(* 1. Operator commands. ESTOP has absolute priority; STOP never clears a latched FAULT/ESTOP. *)
IF EStopRequest THEN
    IF State <> ST_ESTOP THEN
        SafetyTrips := SafetyTrips + 1;
    END_IF;
    State := ST_ESTOP;
    FaultCode := 5;
ELSIF StopRequest AND (State = ST_STOPPED OR State = ST_STARTING OR State = ST_RUNNING) THEN
    State := ST_STOPPED;
    FaultCode := 0;
    StartupElapsedS := 0.0;
ELSIF ResetRequest AND (State = ST_FAULT OR State = ST_ESTOP) THEN
    (* Reset permissive: every trip cause must be cleared first. *)
    IF SensorValid AND TemperatureC < OVERTEMP_C AND HeartbeatHealthy AND IoHealthy
       AND NOT LowFlowFaultActive THEN
        State := ST_STOPPED;
        FaultCode := 0;
        StartupElapsedS := 0.0;
    END_IF;
ELSIF RunRequest AND State = ST_STOPPED AND HeartbeatHealthy AND IoHealthy THEN
    State := ST_STARTING;
    StartupElapsedS := 0.0;
END_IF;

(* 2. Interlocks, evaluated while the pump is commanded. First cause wins. *)
CASE State OF
    ST_STARTING, ST_RUNNING:
        TripCode := 0;
        IF NOT HeartbeatHealthy THEN
            TripCode := 4;
        ELSIF NOT IoHealthy THEN
            TripCode := 6;
        ELSIF NOT SensorValid THEN
            TripCode := 3;
        ELSIF TemperatureC >= OVERTEMP_C THEN
            TripCode := 2;
        ELSIF State = ST_RUNNING AND FlowLpm < MIN_FLOW_LPM THEN
            TripCode := 1;
        ELSIF State = ST_STARTING AND FlowLpm >= MIN_FLOW_LPM THEN
            State := ST_RUNNING;
        ELSIF State = ST_STARTING THEN
            StartupElapsedS := StartupElapsedS + Dt;
            IF StartupElapsedS >= STARTUP_TIMEOUT_S THEN
                TripCode := 1;   (* no flow proven within the startup window *)
            END_IF;
        END_IF;
        IF TripCode <> 0 THEN    (* latch the fault and de-energize *)
            SafetyTrips := SafetyTrips + 1;
            State := ST_FAULT;
            FaultCode := TripCode;
            StartupElapsedS := 0.0;
        END_IF;
END_CASE;

(* 3. Outputs *)
PumpEnable := State = ST_STARTING OR State = ST_RUNNING;
CoolingEnable := State = ST_RUNNING;
END_FUNCTION_BLOCK
