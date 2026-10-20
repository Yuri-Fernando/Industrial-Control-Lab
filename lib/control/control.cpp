#include "control.hpp"
#include <algorithm>
#include <cmath>
namespace icl {
double PiCooling::update(bool running, double error_c, double dt) {
    if (!running) { integral_ = 0.0; return 0.0; }
    constexpr double KP = 16.0;
    constexpr double KI = 0.7;
    const double proposed_integral = std::clamp(integral_ + error_c * dt, -100.0, 100.0);
    const double raw = KP * error_c + KI * proposed_integral;
    // Conditional anti-windup: integrate only if not driving further into saturation.
    if ((raw >= 0.0 && raw <= 100.0) ||
        (raw > 100.0 && error_c < 0.0) ||
        (raw < 0.0 && error_c > 0.0)) integral_ = proposed_integral;
    return std::clamp(KP * error_c + KI * integral_, 0.0, 100.0);
}
void Controller::trip(Fault code) {
    if (out_.state == State::ESTOP) return;
    if (out_.state != State::FAULT) ++out_.safety_trips;
    out_.state = State::FAULT;
    out_.fault = code;
    out_.pump = false;
    out_.cooling_pct = 0;
    startup_seconds_ = 0;
}
Outputs Controller::step(const Inputs& in, double dt) {
    using namespace limits;
    dt = std::clamp(dt, 0.0, MAX_DT_S);
    if (in.estop) {
        if (out_.state != State::ESTOP) ++out_.safety_trips;
        out_.state = State::ESTOP;
        out_.fault = Fault::EMERGENCY_STOP;
    } else if (in.stop && (out_.state == State::STOPPED || out_.state == State::STARTING || out_.state == State::RUNNING)) {
        out_.state = State::STOPPED;
        out_.fault = Fault::NONE;
        startup_seconds_ = 0;
    } else if (in.reset && (out_.state == State::FAULT || out_.state == State::ESTOP)) {
        // Reset only when physical fault inputs are cleared and heartbeat/I-O link have returned.
        if (in.sensor_valid && in.temperature_c < OVERTEMP_C && in.heartbeat_fresh && in.io_healthy &&
            !in.low_flow_fault_active) {
            out_.state = State::STOPPED;
            out_.fault = Fault::NONE;
            startup_seconds_ = 0;
        }
    } else if (in.start && out_.state == State::STOPPED && in.heartbeat_fresh && in.io_healthy) {
        out_.state = State::STARTING;
        startup_seconds_ = 0;
    }
    if (out_.state == State::STARTING || out_.state == State::RUNNING) {
        if (!in.heartbeat_fresh) trip(Fault::WATCHDOG);
        else if (!in.io_healthy) trip(Fault::IO_COMM_LOSS);
        else if (!in.sensor_valid) trip(Fault::SENSOR_INVALID);
        else if (in.temperature_c >= OVERTEMP_C) trip(Fault::OVERTEMP);
        else if (out_.state == State::RUNNING && in.flow_lpm < MIN_FLOW_LPM) trip(Fault::LOW_FLOW);
        else if (out_.state == State::STARTING && in.flow_lpm >= MIN_FLOW_LPM) out_.state = State::RUNNING;
        else if (out_.state == State::STARTING) {
            startup_seconds_ += dt;
            if (startup_seconds_ >= STARTUP_TIMEOUT_S) trip(Fault::LOW_FLOW);
        }
    }
    out_.pump = (out_.state == State::STARTING || out_.state == State::RUNNING);
    out_.error_c = in.temperature_c - in.setpoint_c;
    out_.cooling_pct = pi_.update(out_.state == State::RUNNING, out_.error_c, dt);
    return out_;
}
void Plant::step(double dt, bool pump, double cooling_pct, bool low_flow, double heat_load_pct) {
    dt = std::clamp(dt, 0.0, 0.2);
    flow_lpm_ = (pump && !low_flow) ? 12.0 : 0.0;
    constexpr double ambient_c = 26.0;
    const double heat = 0.075 * std::clamp(heat_load_pct, 0.0, 100.0) / 100.0;
    const double cool = 0.16 * std::clamp(cooling_pct, 0.0, 100.0) / 100.0 * (flow_lpm_ / 12.0);
    const double exchange = 0.012 * (ambient_c - temperature_c_);
    temperature_c_ += dt * (heat - cool + exchange);
}
}
