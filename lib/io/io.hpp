#pragma once
// Field I/O abstraction for the controller scan: read input image -> logic -> write output image.
//   LocalIo  : thermal plant simulated inside the controller process (baseline, IO_MODE=local).
//   RemoteIo : plant lives in a separate remote I/O node process; scanned over Modbus TCP by a
//              dedicated I/O thread so a slow/dead network never blocks the control scan (IO_MODE=remote).
#include <control.hpp>
#include <modbus.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace icl {
// Remote I/O node register contract (docs/MODBUS_REGISTER_MAP.md, "Remote I/O node").
namespace iomap {
// holding registers, written by the controller every I/O scan (FC16 0..6)
constexpr std::uint16_t H_PUMP_CMD = 0, H_COOLING_X10 = 1, H_HEAT_LOAD = 2, H_INJECT_LOW_FLOW = 3,
                        H_INJECT_SENSOR = 4, H_INJECT_OVERTEMP = 5, H_OUTPUT_WATCHDOG = 6,
                        H_CONTROLLER_BLOCK = 7;  // number of registers in the controller write block
// holding registers reserved for the test bench (never written by the controller)
constexpr std::uint16_t H_BENCH_FREEZE = 7, H_BENCH_LATENCY_MS = 8;
// input registers
constexpr std::uint16_t I_TEMPERATURE_X10 = 0, I_FLOW_X10 = 1, I_SENSOR_VALID = 2, I_SAMPLE_SEQ = 3,
                        I_PUMP_APPLIED = 4, I_COOLING_APPLIED_X10 = 5, I_FAILSAFE_ACTIVE = 6,
                        I_FAILSAFE_TRIPS = 7, I_UPTIME_S = 8, I_PLANT_TEMPERATURE_X10 = 9, I_BLOCK = 10;
constexpr int OUTPUT_WATCHDOG_MS = 500;
}

struct FieldInputs {
    double temperature_c = 28.0;
    double flow_lpm = 0.0;
    bool sensor_valid = true;
    bool healthy = true;          // link up and sample fresher than the stale timeout
    bool link_up = true;
    std::uint32_t sample_age_ms = 0;
    std::uint32_t comm_errors = 0;
};
struct FieldOutputs {
    bool pump = false;
    double cooling_pct = 0.0;
    double heat_load_pct = 60.0;
    bool inject_low_flow = false;
    bool inject_sensor = false;
    bool inject_overtemp = false;
};

class IoChannel {
public:
    virtual ~IoChannel() = default;
    virtual FieldInputs read() = 0;
    virtual void write(const FieldOutputs& out, double dt_seconds) = 0;
    virtual const char* mode() const = 0;
};

class LocalIo : public IoChannel {
public:
    FieldInputs read() override;
    void write(const FieldOutputs& out, double dt_seconds) override;
    const char* mode() const override { return "local"; }
private:
    Plant plant_;
    FieldOutputs last_{};
};

class RemoteIo : public IoChannel {
public:
    RemoteIo(const std::string& host, std::uint16_t port, int stale_timeout_ms, int scan_period_ms = 20);
    ~RemoteIo() override;
    FieldInputs read() override;
    void write(const FieldOutputs& out, double dt_seconds) override;
    const char* mode() const override { return "remote"; }
private:
    void scan_loop();
    std::string host_;
    std::uint16_t port_;
    int stale_timeout_ms_, scan_period_ms_;
    std::mutex mutex_;
    FieldOutputs pending_{};
    FieldInputs sample_{};
    bool have_sample_ = false;
    std::uint16_t last_seq_ = 0;
    std::chrono::steady_clock::time_point last_fresh_{};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

// Register image + plant of the remote I/O node process (src/io_node/main.cpp).
class IoNodeDevice : public ModbusDevice {
public:
    IoNodeDevice();
    // One physics/IO tick: applies output watchdog fail-safe, steps the plant, refreshes input registers.
    void tick(double dt_seconds, std::chrono::steady_clock::time_point now, std::uint32_t uptime_s);
    Status read_holding(std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out) const override;
    Status read_input(std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out) const override;
    Status write_multiple(std::uint16_t start, const std::vector<std::uint16_t>& values) override;
    std::chrono::milliseconds response_delay() const override;
private:
    mutable std::mutex mutex_;
    std::array<std::uint16_t, 16> holding_{};
    std::array<std::uint16_t, 16> inputs_{};
    Plant plant_;
    std::uint16_t last_watchdog_ = 0, seq_ = 0, failsafe_trips_ = 0;
    std::chrono::steady_clock::time_point watchdog_seen_{};
    bool watchdog_ever_ = false, failsafe_ = true;
};
}
