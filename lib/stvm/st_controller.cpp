#include "st_controller.hpp"
#include <algorithm>

namespace icl {
StController::StController(std::shared_ptr<const st::Program> program) : fb_(std::move(program)) {}

StController StController::from_file(const std::string& path) { return StController(st::compile_file(path)); }

Outputs StController::step(const Inputs& in, double dt) {
    out_.error_c = in.temperature_c - in.setpoint_c;
    if (!runtime_error_.empty()) {  // latched: a faulted ST runtime never re-energizes outputs
        out_.pump = false;
        out_.cooling_pct = 0.0;
        return out_;
    }
    try {
        fb_.set("RunRequest", in.start);
        fb_.set("StopRequest", in.stop);
        fb_.set("ResetRequest", in.reset);
        fb_.set("EStopRequest", in.estop);
        fb_.set("TemperatureC", in.temperature_c);
        fb_.set("FlowLpm", in.flow_lpm);
        fb_.set("LowFlowFaultActive", in.low_flow_fault_active);
        fb_.set("SensorValid", in.sensor_valid);
        fb_.set("HeartbeatHealthy", in.heartbeat_fresh);
        fb_.set("IoHealthy", in.io_healthy);
        fb_.set("CycleTimeS", dt);
        fb_.execute();
        out_.state = static_cast<State>(fb_.get_int("State"));
        out_.fault = static_cast<Fault>(fb_.get_int("FaultCode"));
        out_.pump = fb_.get_bool("PumpEnable");
        out_.safety_trips = static_cast<std::uint32_t>(fb_.get_int("SafetyTrips"));
        // Same dt saturation as the ST block (LIMIT) and icl::Controller; found by the differential test.
        out_.cooling_pct = pi_.update(fb_.get_bool("CoolingEnable"), out_.error_c, std::clamp(dt, 0.0, limits::MAX_DT_S));
    } catch (const std::exception& e) {
        runtime_error_ = e.what();
        ++out_.safety_trips;
        out_.state = State::FAULT;
        out_.fault = Fault::WATCHDOG;
        out_.pump = false;
        out_.cooling_pct = 0.0;
    }
    return out_;
}
}
