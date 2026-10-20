// Industrial Control Lab — soft-PLC controller process.
// Scan cycle (50 ms): read field inputs -> execute control engine -> write field outputs -> publish registers.
#include <control.hpp>
#include <cycle_stats.hpp>
#include <io.hpp>
#include <modbus.hpp>
#include <net.hpp>
#include <st_controller.hpp>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {
std::atomic<bool> running{true};
void request_exit(int) { running.store(false); }
std::uint16_t reg(double value) {
    return static_cast<std::uint16_t>(std::clamp<long long>(std::llround(value), 0LL, 65535LL));
}
std::uint16_t signed_reg(double value) {
    auto v = static_cast<std::int16_t>(std::clamp<long long>(std::llround(value), -32768LL, 32767LL));
    return static_cast<std::uint16_t>(v);
}
std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}
int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    try { return (v && *v) ? std::stoi(v) : fallback; } catch (...) { return fallback; }
}
bool same(const icl::Outputs& a, const icl::Outputs& b) {
    return a.state == b.state && a.fault == b.fault && a.pump == b.pump && a.safety_trips == b.safety_trips &&
           std::fabs(a.cooling_pct - b.cooling_pct) < 1e-9;
}
}

int main() {
    using clock = std::chrono::steady_clock;
    constexpr auto period = std::chrono::milliseconds(50);
    constexpr double dt = 0.05;
    std::signal(SIGINT, request_exit);
    std::signal(SIGTERM, request_exit);
    if (!icl::net::startup()) { std::cerr << "socket subsystem startup failed\n"; return 1; }

    const auto bind_ip = env_or("CONTROL_BIND", "127.0.0.1");
    const auto port = static_cast<std::uint16_t>(env_int("CONTROL_PORT", 15020));
    const auto io_mode = env_or("IO_MODE", "local");
    const auto engine_name = env_or("CONTROL_ENGINE", "cpp");
    const auto st_path = env_or("ICL_ST_FILE", "st/FB_Interlocks.st");
    if ((io_mode != "local" && io_mode != "remote") || (engine_name != "cpp" && engine_name != "st")) {
        std::cerr << "IO_MODE must be local|remote and CONTROL_ENGINE cpp|st\n";
        return 2;
    }

    // Control engines: the selected one drives outputs, the other runs in shadow for live differential checking.
    icl::Controller cpp_engine;
    std::unique_ptr<icl::StController> st_engine;
    try {
        st_engine = std::make_unique<icl::StController>(icl::StController::from_file(st_path));
    } catch (const std::exception& e) {
        if (engine_name == "st") { std::cerr << "Cannot load ST program: " << e.what() << '\n'; return 2; }
        std::cerr << "ST shadow disabled: " << e.what() << '\n';
    }
    icl::ControlEngine* primary = engine_name == "st" ? static_cast<icl::ControlEngine*>(st_engine.get()) : &cpp_engine;
    icl::ControlEngine* shadow = engine_name == "st" ? static_cast<icl::ControlEngine*>(&cpp_engine) : st_engine.get();

    std::unique_ptr<icl::IoChannel> io;
    if (io_mode == "remote")
        io = std::make_unique<icl::RemoteIo>(env_or("IO_HOST", "127.0.0.1"),
                                             static_cast<std::uint16_t>(env_int("IO_PORT", 15021)),
                                             env_int("IO_TIMEOUT_MS", 300));
    else
        io = std::make_unique<icl::LocalIo>();

    icl::RtOutcome rt;
    if (env_or("ICL_RT", "0") == "1") rt = icl::apply_realtime_tuning();

    icl::RegisterBank bank;
    std::thread server([&]() {
        try { icl::serve_modbus(bank, bind_ip, port); }
        catch (const std::exception& e) { std::cerr << "Server error: " << e.what() << '\n'; running.store(false); }
    });
    server.detach(); // Process exit terminates demonstration transport workers.
    std::cout << "Controller: engine=" << primary->name() << " shadow=" << (shadow ? shadow->name() : "none")
              << " io=" << io->mode() << " rt=" << rt.detail << std::endl;

    icl::SlidingPercentiles lateness_window(1200), exec_window(1200);  // 60 s of scans
    icl::Percentiles lat{}, exe{};
    auto due = clock::now() + period;
    const auto boot = clock::now();
    auto last_heartbeat = boot;
    std::uint16_t previous_heartbeat = 0;
    std::uint32_t overruns = 0, max_jitter_us = 0, mismatches = 0;
    std::uint64_t scans = 0;
    while (running.load()) {
        icl::sleep_until_precise(due);
        const auto now = clock::now();
        const long long lateness_us = std::chrono::duration_cast<std::chrono::microseconds>(now - due).count();
        if (lateness_us > 50000) ++overruns;
        max_jitter_us = std::max(max_jitter_us, static_cast<std::uint32_t>(std::clamp<long long>(lateness_us, 0, 65535)));
        due += period;
        if (due <= now) due = now + period;

        // 1. input scan
        const auto h = bank.snapshot().holding;
        const auto command = bank.take_command();
        if (h[5] != previous_heartbeat) { previous_heartbeat = h[5]; last_heartbeat = now; }
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat).count();
        const icl::FieldInputs field = io->read();
        icl::Inputs inputs;
        inputs.temperature_c = field.temperature_c;
        inputs.flow_lpm = field.flow_lpm;
        inputs.sensor_valid = field.sensor_valid;
        inputs.io_healthy = field.healthy;
        inputs.low_flow_fault_active = h[3] != 0;
        inputs.heartbeat_fresh = age_ms < 3000;
        inputs.start = command == 1; inputs.stop = command == 2;
        inputs.reset = command == 3; inputs.estop = command == 4;
        inputs.setpoint_c = h[1] / 10.0;

        // 2. logic
        const auto out = primary->step(inputs, dt);
        if (shadow && !same(out, shadow->step(inputs, dt))) ++mismatches;

        // 3. output scan
        icl::FieldOutputs field_out;
        field_out.pump = out.pump;
        field_out.cooling_pct = out.cooling_pct;
        field_out.heat_load_pct = h[2];
        field_out.inject_low_flow = h[3] != 0;
        field_out.inject_sensor = h[4] != 0;
        field_out.inject_overtemp = h[6] != 0;
        io->write(field_out, dt);

        // 4. diagnostics
        const long long exec_us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - now).count();
        lateness_window.add(static_cast<std::uint32_t>(std::clamp<long long>(lateness_us, 0, 0xffffffffLL)));
        exec_window.add(static_cast<std::uint32_t>(std::clamp<long long>(exec_us, 0, 0xffffffffLL)));
        if (++scans % 20 == 0) { lat = lateness_window.compute(); exe = exec_window.compute(); }

        std::array<std::uint16_t, 32> vals{};
        vals[0] = static_cast<std::uint16_t>(out.state);
        vals[1] = reg(inputs.temperature_c * 10);
        vals[2] = reg(inputs.flow_lpm * 10);
        vals[3] = reg(out.cooling_pct * 10);
        vals[4] = out.pump ? 1 : 0;
        vals[5] = static_cast<std::uint16_t>(out.fault);
        vals[6] = reg(max_jitter_us);
        vals[7] = reg(overruns);
        vals[8] = reg(static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(now - boot).count()));
        vals[9] = reg(static_cast<double>(age_ms));
        vals[10] = h[5];
        vals[11] = reg(out.safety_trips);
        vals[12] = signed_reg(out.error_c * 10);
        vals[13] = inputs.sensor_valid ? 1 : 0;
        vals[14] = reg(lat.p50);
        vals[15] = reg(lat.p95);
        vals[16] = reg(lat.p99);
        vals[17] = reg(lat.max);
        vals[18] = reg(exe.p99);
        vals[19] = reg(exe.max);
        vals[20] = io_mode == "remote" ? 1 : 0;
        vals[21] = field.healthy ? 1 : 0;
        vals[22] = reg(field.sample_age_ms);
        vals[23] = reg(field.comm_errors);
        vals[24] = engine_name == "st" ? 1 : 0;
        vals[25] = rt.elevated ? 1 : 0;
        vals[26] = reg(mismatches);
        vals[27] = shadow ? 1 : 0;
        bank.update_inputs(vals);
    }
    icl::release_realtime_tuning();
    std::cout << "Controller exiting after " << scans << " scans, engine mismatches=" << mismatches << std::endl;
}
