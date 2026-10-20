#include "io.hpp"
#include <algorithm>
#include <cmath>

namespace icl {
static std::uint16_t x10(double v) {
    return static_cast<std::uint16_t>(std::clamp<long>(std::lround(v * 10.0), 0L, 65535L));
}

// ------------------------------------------------------------------ LocalIo
FieldInputs LocalIo::read() {
    FieldInputs in;
    in.temperature_c = plant_.temperature() + (last_.inject_overtemp ? 50.0 : 0.0);
    in.flow_lpm = plant_.flow();
    in.sensor_valid = !last_.inject_sensor;
    return in;
}
void LocalIo::write(const FieldOutputs& out, double dt) {
    last_ = out;
    plant_.step(dt, out.pump, out.cooling_pct, out.inject_low_flow, out.heat_load_pct);
}

// ------------------------------------------------------------------ RemoteIo
RemoteIo::RemoteIo(const std::string& host, std::uint16_t port, int stale_timeout_ms, int scan_period_ms)
    : host_(host), port_(port), stale_timeout_ms_(stale_timeout_ms), scan_period_ms_(scan_period_ms) {
    sample_.healthy = sample_.link_up = sample_.sensor_valid = false;
    worker_ = std::thread(&RemoteIo::scan_loop, this);
}
RemoteIo::~RemoteIo() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
}
FieldInputs RemoteIo::read() {
    std::lock_guard<std::mutex> lock(mutex_);
    FieldInputs in = sample_;
    const auto age = have_sample_
        ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_fresh_).count()
        : 65535;
    in.sample_age_ms = static_cast<std::uint32_t>(std::clamp<long long>(age, 0, 65535));
    in.healthy = have_sample_ && in.link_up && age <= stale_timeout_ms_;
    if (!in.healthy) in.sensor_valid = false;  // stale data is never presented as a valid measurement
    return in;
}
void RemoteIo::write(const FieldOutputs& out, double) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = out;
}
void RemoteIo::scan_loop() {
    ModbusTcpClient client(host_, port_, stale_timeout_ms_);
    std::uint16_t watchdog = 0;
    auto next = std::chrono::steady_clock::now();
    while (!stop_.load()) {
        FieldOutputs out;
        { std::lock_guard<std::mutex> lock(mutex_); out = pending_; }
        const std::vector<std::uint16_t> block{
            static_cast<std::uint16_t>(out.pump ? 1 : 0), x10(out.cooling_pct),
            static_cast<std::uint16_t>(std::clamp(std::lround(out.heat_load_pct), 0L, 100L)),
            static_cast<std::uint16_t>(out.inject_low_flow ? 1 : 0), static_cast<std::uint16_t>(out.inject_sensor ? 1 : 0),
            static_cast<std::uint16_t>(out.inject_overtemp ? 1 : 0), ++watchdog};
        std::vector<std::uint16_t> regs;
        const bool ok = client.write_multiple(iomap::H_PUMP_CMD, block) &&
                        client.read_input(0, iomap::I_BLOCK, regs);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sample_.link_up = ok;
            if (ok) {
                const auto now = std::chrono::steady_clock::now();
                sample_.temperature_c = regs[iomap::I_TEMPERATURE_X10] / 10.0;
                sample_.flow_lpm = regs[iomap::I_FLOW_X10] / 10.0;
                sample_.sensor_valid = regs[iomap::I_SENSOR_VALID] != 0;
                if (!have_sample_ || regs[iomap::I_SAMPLE_SEQ] != last_seq_) {
                    last_seq_ = regs[iomap::I_SAMPLE_SEQ];
                    last_fresh_ = now;
                    have_sample_ = true;
                }
            } else {
                ++sample_.comm_errors;
            }
        }
        next += std::chrono::milliseconds(ok ? scan_period_ms_ : 100);  // back off while the node is unreachable
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;
        while (!stop_.load() && std::chrono::steady_clock::now() < next)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ------------------------------------------------------------------ IoNodeDevice
IoNodeDevice::IoNodeDevice() { holding_[iomap::H_HEAT_LOAD] = 60; }

void IoNodeDevice::tick(double dt, std::chrono::steady_clock::time_point now, std::uint32_t uptime_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (holding_[iomap::H_OUTPUT_WATCHDOG] != last_watchdog_) {  // controller proves it is alive by changing it
        last_watchdog_ = holding_[iomap::H_OUTPUT_WATCHDOG];
        watchdog_seen_ = now;
        watchdog_ever_ = true;
    }
    const bool expired = !watchdog_ever_ ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - watchdog_seen_).count() > iomap::OUTPUT_WATCHDOG_MS;
    if (expired && !failsafe_) ++failsafe_trips_;
    failsafe_ = expired;
    // Fail-safe state of the I/O module: outputs de-energized while the controller is silent.
    const bool pump = !failsafe_ && holding_[iomap::H_PUMP_CMD] != 0;
    const double cooling = failsafe_ ? 0.0 : holding_[iomap::H_COOLING_X10] / 10.0;
    plant_.step(dt, pump, cooling, holding_[iomap::H_INJECT_LOW_FLOW] != 0, holding_[iomap::H_HEAT_LOAD]);
    if (!holding_[iomap::H_BENCH_FREEZE]) ++seq_;  // frozen samples emulate a stuck transmitter
    const double measured = plant_.temperature() + (holding_[iomap::H_INJECT_OVERTEMP] ? 50.0 : 0.0);
    if (!holding_[iomap::H_BENCH_FREEZE]) {
        inputs_[iomap::I_TEMPERATURE_X10] = x10(measured);
        inputs_[iomap::I_FLOW_X10] = x10(plant_.flow());
        inputs_[iomap::I_SENSOR_VALID] = holding_[iomap::H_INJECT_SENSOR] ? 0 : 1;
    }
    inputs_[iomap::I_SAMPLE_SEQ] = seq_;
    inputs_[iomap::I_PUMP_APPLIED] = pump ? 1 : 0;
    inputs_[iomap::I_COOLING_APPLIED_X10] = x10(cooling);
    inputs_[iomap::I_FAILSAFE_ACTIVE] = failsafe_ ? 1 : 0;
    inputs_[iomap::I_FAILSAFE_TRIPS] = failsafe_trips_;
    inputs_[iomap::I_UPTIME_S] = static_cast<std::uint16_t>(std::min<std::uint32_t>(uptime_s, 65535));
    inputs_[iomap::I_PLANT_TEMPERATURE_X10] = x10(plant_.temperature());
}

template <std::size_t N>
static ModbusDevice::Status copy_regs(const std::array<std::uint16_t, N>& regs, std::uint16_t s, std::uint16_t c,
                                      std::vector<std::uint16_t>& out) {
    if (s >= N || c > N - s) return ModbusDevice::Status::ILLEGAL_ADDRESS;
    out.assign(regs.begin() + s, regs.begin() + s + c);
    return ModbusDevice::Status::OK;
}
ModbusDevice::Status IoNodeDevice::read_holding(std::uint16_t s, std::uint16_t c, std::vector<std::uint16_t>& out) const {
    std::lock_guard<std::mutex> lock(mutex_); return copy_regs(holding_, s, c, out);
}
ModbusDevice::Status IoNodeDevice::read_input(std::uint16_t s, std::uint16_t c, std::vector<std::uint16_t>& out) const {
    std::lock_guard<std::mutex> lock(mutex_); return copy_regs(inputs_, s, c, out);
}
ModbusDevice::Status IoNodeDevice::write_multiple(std::uint16_t s, const std::vector<std::uint16_t>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (values.empty() || s >= holding_.size() || values.size() > holding_.size() - s) return Status::ILLEGAL_ADDRESS;
    for (std::size_t k = 0; k < values.size(); ++k) {
        const auto a = s + k; const auto v = values[k];
        bool valid;
        switch (a) {
            case iomap::H_PUMP_CMD: case iomap::H_INJECT_LOW_FLOW: case iomap::H_INJECT_SENSOR:
            case iomap::H_INJECT_OVERTEMP: case iomap::H_BENCH_FREEZE: valid = v <= 1; break;
            case iomap::H_COOLING_X10: valid = v <= 1000; break;
            case iomap::H_HEAT_LOAD: valid = v <= 100; break;
            case iomap::H_OUTPUT_WATCHDOG: valid = true; break;
            case iomap::H_BENCH_LATENCY_MS: valid = v <= 2000; break;
            default: valid = false;
        }
        if (!valid) return Status::ILLEGAL_VALUE;
    }
    std::copy(values.begin(), values.end(), holding_.begin() + s);
    return Status::OK;
}
std::chrono::milliseconds IoNodeDevice::response_delay() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::chrono::milliseconds(holding_[iomap::H_BENCH_LATENCY_MS]);
}
}
