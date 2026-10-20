// Industrial Control Lab — remote I/O node process.
// Emulates a Modbus TCP field I/O module wired to the cooling skid: temperature transmitter, flow meter,
// pump contactor and cooling valve. The thermal plant runs here at 100 Hz, outside the controller process,
// so the closed loop really crosses a process/network boundary (IO_MODE=remote on the controller).
#include <io.hpp>
#include <modbus.hpp>
#include <net.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic<bool> running{true};
void request_exit(int) { running.store(false); }
}

int main() {
    using clock = std::chrono::steady_clock;
    std::signal(SIGINT, request_exit);
    std::signal(SIGTERM, request_exit);
    if (!icl::net::startup()) { std::cerr << "socket subsystem startup failed\n"; return 1; }
    const char* bind_env = std::getenv("IO_BIND");
    const char* port_env = std::getenv("IO_PORT");
    const std::string bind_ip = (bind_env && *bind_env) ? bind_env : "127.0.0.1";
    const auto port = static_cast<std::uint16_t>(port_env ? std::atoi(port_env) : 15021);

    icl::IoNodeDevice device;
    std::thread server([&]() {
        try { icl::serve_modbus(device, bind_ip, port); }
        catch (const std::exception& e) { std::cerr << "IO node server error: " << e.what() << '\n'; running.store(false); }
    });
    server.detach();
    std::cout << "Remote I/O node: plant 100 Hz, output watchdog " << icl::iomap::OUTPUT_WATCHDOG_MS << " ms" << std::endl;

    constexpr auto tick = std::chrono::milliseconds(10);
    const auto boot = clock::now();
    auto due = boot + tick;
    while (running.load()) {
        std::this_thread::sleep_until(due);
        const auto now = clock::now();
        due += tick;
        if (due <= now) due = now + tick;
        const auto up = std::chrono::duration_cast<std::chrono::seconds>(now - boot).count();
        device.tick(0.01, now, static_cast<std::uint32_t>(up));
    }
    std::cout << "Remote I/O node exiting" << std::endl;
}
