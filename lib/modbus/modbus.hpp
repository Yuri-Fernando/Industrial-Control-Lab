#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace icl {
// Register-level contract shared by every Modbus TCP server in the lab (controller and remote I/O node).
// Implementations must validate a whole write before committing any register (FC16 atomicity).
class ModbusDevice {
public:
    enum class Status { OK, ILLEGAL_ADDRESS, ILLEGAL_VALUE };
    virtual ~ModbusDevice() = default;
    virtual Status read_holding(std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out) const = 0;
    virtual Status read_input(std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out) const = 0;
    virtual Status write_multiple(std::uint16_t start, const std::vector<std::uint16_t>& values) = 0;
    // Test-bench hook: artificial response latency (network delay injection). Zero by default.
    virtual std::chrono::milliseconds response_delay() const { return std::chrono::milliseconds(0); }
};

struct RegisterSnapshot {
    std::array<std::uint16_t, 16> holding{};
    std::array<std::uint16_t, 32> inputs{};
};

// Controller register image (see docs/MODBUS_REGISTER_MAP.md).
class RegisterBank : public ModbusDevice {
public:
    RegisterBank();
    RegisterSnapshot snapshot() const;
    void update_inputs(const std::array<std::uint16_t, 32>& value);
    bool write_holding(std::uint16_t address, std::uint16_t value);
    bool write_multiple_checked(std::uint16_t start, const std::vector<std::uint16_t>& values);
    std::uint16_t take_command();

    Status read_holding(std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out) const override;
    Status read_input(std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out) const override;
    Status write_multiple(std::uint16_t start, const std::vector<std::uint16_t>& values) override;
private:
    mutable std::mutex mutex_;
    RegisterSnapshot regs_{};
    std::deque<std::uint16_t> queued_commands_;
};

// Pure frame handler (no sockets): turns one MBAP request into one MBAP response.
// Returns false when the frame is malformed at MBAP level and the connection must be dropped.
bool handle_modbus_frame(ModbusDevice& device, const std::uint8_t* header7, const std::vector<std::uint8_t>& pdu,
                         std::vector<std::uint8_t>& packet_out);

// Demonstration-only Modbus TCP subset: FC03, FC04, FC06, FC16.
// Bind to loopback in local mode; not a certified industrial protocol stack.
// Blocks forever serving clients; at most max_sessions concurrent connections are accepted.
void serve_modbus(ModbusDevice& device, const std::string& bind_ip, std::uint16_t port, int max_sessions = 16);

// Persistent Modbus TCP client used by the controller to scan the remote I/O node.
// Not thread-safe; owned by a single I/O thread. Any transport/protocol error closes the link.
class ModbusTcpClient {
public:
    ModbusTcpClient(std::string host, std::uint16_t port, int timeout_ms, std::uint8_t unit = 1);
    ~ModbusTcpClient();
    ModbusTcpClient(const ModbusTcpClient&) = delete;
    ModbusTcpClient& operator=(const ModbusTcpClient&) = delete;
    bool connected() const;
    void disconnect();
    bool read_input(std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out);
    bool read_holding(std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out);
    bool write_multiple(std::uint16_t start, const std::vector<std::uint16_t>& values);
    const std::string& last_error() const { return error_; }
private:
    bool ensure_connected();
    bool exchange(const std::vector<std::uint8_t>& pdu, std::vector<std::uint8_t>& response_pdu);
    bool read(std::uint8_t fc, std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out);
    std::string host_;
    std::uint16_t port_;
    int timeout_ms_;
    std::uint8_t unit_;
    std::uintptr_t fd_;
    std::uint16_t transaction_ = 0;
    std::string error_;
};
}
