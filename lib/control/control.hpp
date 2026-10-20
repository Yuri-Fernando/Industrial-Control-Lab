#pragma once
#include <cstdint>

namespace icl {
enum class State : std::uint16_t { STOPPED=0, STARTING=1, RUNNING=2, FAULT=3, ESTOP=4 };
enum class Fault : std::uint16_t { NONE=0, LOW_FLOW=1, OVERTEMP=2, SENSOR_INVALID=3, WATCHDOG=4, EMERGENCY_STOP=5, IO_COMM_LOSS=6 };

// Interlock limits shared by the C++ engine and the IEC 61131-3 ST block (st/FB_Interlocks.st).
namespace limits {
constexpr double OVERTEMP_C = 45.0;
constexpr double MIN_FLOW_LPM = 5.0;
constexpr double STARTUP_TIMEOUT_S = 2.0;
constexpr double MAX_DT_S = 0.2;
}

struct Inputs {
    double temperature_c = 28.0;
    double flow_lpm = 0.0;
    bool sensor_valid = true;
    bool heartbeat_fresh = true;
    bool io_healthy = true;          // remote I/O link up and samples fresh (always true for local I/O)
    bool low_flow_fault_active = false;
    bool start = false;
    bool stop = false;
    bool reset = false;
    bool estop = false;
    double setpoint_c = 27.0;
};
struct Outputs {
    State state = State::STOPPED;
    Fault fault = Fault::NONE;
    bool pump = false;
    double cooling_pct = 0.0;
    double error_c = 0.0;
    std::uint32_t safety_trips = 0;
};

// One scan of control logic: inputs image -> outputs image. Implemented in C++ (Controller)
// and by the Structured Text runtime (StController in lib/stvm).
class ControlEngine {
public:
    virtual ~ControlEngine() = default;
    virtual Outputs step(const Inputs& in, double dt_seconds) = 0;
    virtual const char* name() const = 0;
};

// Discrete PI with conditional-integration anti-windup; integral is cleared whenever not running.
class PiCooling {
public:
    double update(bool running, double error_c, double dt_seconds);
    void reset() { integral_ = 0.0; }
private:
    double integral_ = 0.0;
};

class Controller : public ControlEngine {
public:
    Outputs step(const Inputs& in, double dt_seconds) override;
    const char* name() const override { return "cpp"; }
    Outputs output() const { return out_; }
private:
    void trip(Fault code);
    Outputs out_{};
    PiCooling pi_;
    double startup_seconds_ = 0.0;
};

class Plant {
public:
    void step(double seconds, bool pump, double cooling_pct, bool low_flow, double heat_load_pct);
    double temperature() const { return temperature_c_; }
    double flow() const { return flow_lpm_; }
private:
    double temperature_c_ = 28.0;
    double flow_lpm_ = 0.0;
};
}
