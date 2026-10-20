#include "modbus.hpp"
#include <net.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace icl {
static std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
static void put16(std::vector<std::uint8_t>& data, std::uint16_t v) {
    data.push_back(static_cast<std::uint8_t>(v >> 8)); data.push_back(static_cast<std::uint8_t>(v & 255));
}

RegisterBank::RegisterBank() { regs_.holding[1] = 270; regs_.holding[2] = 60; }
RegisterSnapshot RegisterBank::snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return regs_; }
void RegisterBank::update_inputs(const std::array<std::uint16_t, 32>& v) {
    std::lock_guard<std::mutex> lock(mutex_); regs_.inputs = v;
}
bool RegisterBank::write_holding(std::uint16_t a, std::uint16_t v) {
    return write_multiple_checked(a, {v});
}
bool RegisterBank::write_multiple_checked(std::uint16_t start, const std::vector<std::uint16_t>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (values.empty() || start >= regs_.holding.size() ||
        values.size() > regs_.holding.size()-start) return false;
    for (std::size_t i=0; i<values.size(); ++i) {
        const auto a = start+i; const auto v = values[i];
        bool valid = false;
        switch (a) {
            case 0: valid = v <= 4; break;
            case 1: valid = v >= 240 && v <= 380; break;
            case 2: valid = v <= 100; break;
            case 3: case 4: case 6: valid = v <= 1; break;
            case 5: valid = true; break;
            default: valid = false;
        }
        if (!valid || (a==0 && v!=0 && queued_commands_.size()>=32)) return false;
    }
    for (std::size_t i=0;i<values.size();++i) {
        const auto a=start+i; const auto v=values[i];
        if (a==0 && v!=0) queued_commands_.push_back(v);
        regs_.holding[a] = v;
    }
    return true;
}
std::uint16_t RegisterBank::take_command() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cmd = queued_commands_.empty() ? 0 : queued_commands_.front();
    if (!queued_commands_.empty()) queued_commands_.pop_front();
    regs_.holding[0] = 0;
    return cmd;
}

template <std::size_t N>
static ModbusDevice::Status copy_range(const std::array<std::uint16_t, N>& regs, std::uint16_t start,
                                       std::uint16_t count, std::vector<std::uint16_t>& out) {
    if (start >= N || count > N - start) return ModbusDevice::Status::ILLEGAL_ADDRESS;
    out.assign(regs.begin() + start, regs.begin() + start + count);
    return ModbusDevice::Status::OK;
}
ModbusDevice::Status RegisterBank::read_holding(std::uint16_t s, std::uint16_t c, std::vector<std::uint16_t>& out) const {
    std::lock_guard<std::mutex> lock(mutex_); return copy_range(regs_.holding, s, c, out);
}
ModbusDevice::Status RegisterBank::read_input(std::uint16_t s, std::uint16_t c, std::vector<std::uint16_t>& out) const {
    std::lock_guard<std::mutex> lock(mutex_); return copy_range(regs_.inputs, s, c, out);
}
ModbusDevice::Status RegisterBank::write_multiple(std::uint16_t s, const std::vector<std::uint16_t>& v) {
    if (s >= regs_.holding.size() || v.size() > regs_.holding.size() - s) return Status::ILLEGAL_ADDRESS;
    return write_multiple_checked(s, v) ? Status::OK : Status::ILLEGAL_VALUE;
}

bool handle_modbus_frame(ModbusDevice& device, const std::uint8_t* head, const std::vector<std::uint8_t>& request,
                         std::vector<std::uint8_t>& packet) {
    if (request.empty()) return false;
    const std::uint8_t fc = request[0];
    std::vector<std::uint8_t> response;
    auto exception = [&](std::uint8_t code) { response = {static_cast<std::uint8_t>(fc|0x80), code}; };
    auto status_exception = [&](ModbusDevice::Status s) {
        exception(s == ModbusDevice::Status::ILLEGAL_ADDRESS ? 2 : 3);
    };
    if (fc == 3 || fc == 4) {
        if (request.size() != 5) exception(3);
        else {
            const auto address = u16(&request[1]); const auto count = u16(&request[3]);
            std::vector<std::uint16_t> values;
            if (count == 0 || count > 125) exception(3);
            else {
                const auto st = fc == 3 ? device.read_holding(address, count, values)
                                        : device.read_input(address, count, values);
                if (st != ModbusDevice::Status::OK) status_exception(st);
                else {
                    response = {fc, static_cast<std::uint8_t>(count*2)};
                    for (const auto v : values) put16(response, v);
                }
            }
        }
    } else if (fc == 6) {
        if (request.size() != 5) exception(3);
        else {
            const auto st = device.write_multiple(u16(&request[1]), {u16(&request[3])});
            if (st != ModbusDevice::Status::OK) status_exception(st);
            else response = request;
        }
    } else if (fc == 16) {
        if (request.size() < 6) exception(3);
        else {
            const auto address=u16(&request[1]), count=u16(&request[3]);
            if (!count || count>123 || request[5] != count*2 || request.size() != static_cast<std::size_t>(6+count*2)) exception(3);
            else {
                std::vector<std::uint16_t> values;
                for (std::size_t i=0;i<count;++i) values.push_back(u16(&request[6+i*2]));
                const auto st = device.write_multiple(address, values);
                if (st != ModbusDevice::Status::OK) status_exception(st);
                else { response={fc}; put16(response,address); put16(response,count); }
            }
        }
    } else exception(1);
    packet.assign(head, head+7);
    const std::uint16_t out_len = static_cast<std::uint16_t>(response.size()+1);
    packet[4]=static_cast<std::uint8_t>(out_len>>8);
    packet[5]=static_cast<std::uint8_t>(out_len&255);
    packet.insert(packet.end(), response.begin(), response.end());
    return true;
}

static void client_session(net::socket_t fd, ModbusDevice& device, std::atomic<int>& sessions) {
    net::set_timeouts(fd, 5000, 5000);  // idle clients are dropped after 5 s
    net::set_nodelay(fd);
    while (true) {
        std::uint8_t head[7];
        if (!net::receive_all(fd, head, 7)) break;
        const std::uint16_t length = u16(head + 4);
        if (u16(head+2) != 0 || length < 2 || length > 254) break;
        std::vector<std::uint8_t> request(length-1);
        if (!net::receive_all(fd, request.data(), request.size())) break;
        std::vector<std::uint8_t> packet;
        if (!handle_modbus_frame(device, head, request, packet)) break;
        const auto delay = device.response_delay();
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        if (!net::send_all(fd, packet.data(), packet.size())) break;
    }
    net::close_socket(fd);
    --sessions;
}

void serve_modbus(ModbusDevice& device, const std::string& bind_ip, std::uint16_t port, int max_sessions) {
    const auto server = net::listen_tcp(bind_ip, port);
    std::cout << "Modbus TCP listening on " << bind_ip << ":" << port << std::endl;
    static std::atomic<int> sessions{0};
    while (true) {
        const auto client = accept(server, nullptr, nullptr);
        if (client == net::invalid_socket) { if (net::interrupted_by_signal()) continue; break; }
        if (sessions.load() >= max_sessions) { net::close_socket(client); continue; }  // connection quota
        ++sessions;
        std::thread(client_session, client, std::ref(device), std::ref(sessions)).detach();
    }
    net::close_socket(server);
}

ModbusTcpClient::ModbusTcpClient(std::string host, std::uint16_t port, int timeout_ms, std::uint8_t unit)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms), unit_(unit),
      fd_(static_cast<std::uintptr_t>(net::invalid_socket)) {}
ModbusTcpClient::~ModbusTcpClient() { disconnect(); }
bool ModbusTcpClient::connected() const { return fd_ != static_cast<std::uintptr_t>(net::invalid_socket); }
void ModbusTcpClient::disconnect() {
    net::close_socket(static_cast<net::socket_t>(fd_));
    fd_ = static_cast<std::uintptr_t>(net::invalid_socket);
}
bool ModbusTcpClient::ensure_connected() {
    if (connected()) return true;
    const auto fd = net::connect_tcp(host_, port_, timeout_ms_);
    if (fd == net::invalid_socket) { error_ = "connect " + host_ + ":" + std::to_string(port_) + " failed"; return false; }
    fd_ = static_cast<std::uintptr_t>(fd);
    return true;
}
bool ModbusTcpClient::exchange(const std::vector<std::uint8_t>& pdu, std::vector<std::uint8_t>& rsp) {
    if (!ensure_connected()) return false;
    const auto fd = static_cast<net::socket_t>(fd_);
    const std::uint16_t txn = ++transaction_;
    std::vector<std::uint8_t> frame;
    put16(frame, txn); put16(frame, 0); put16(frame, static_cast<std::uint16_t>(pdu.size() + 1));
    frame.push_back(unit_);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    std::uint8_t head[7];
    if (!net::send_all(fd, frame.data(), frame.size()) || !net::receive_all(fd, head, 7)) {
        error_ = "transport timeout/disconnect"; disconnect(); return false;
    }
    const auto length = u16(head + 4);
    if (u16(head) != txn || u16(head + 2) != 0 || head[6] != unit_ || length < 2 || length > 254) {
        error_ = "invalid MBAP response"; disconnect(); return false;
    }
    rsp.resize(length - 1);
    if (!net::receive_all(fd, rsp.data(), rsp.size())) { error_ = "truncated response"; disconnect(); return false; }
    if (rsp[0] == (pdu[0] | 0x80)) { error_ = "modbus exception " + std::to_string(rsp.size() > 1 ? rsp[1] : 0); return false; }
    if (rsp[0] != pdu[0]) { error_ = "unexpected function code"; disconnect(); return false; }
    return true;
}
bool ModbusTcpClient::read(std::uint8_t fc, std::uint16_t start, std::uint16_t count, std::vector<std::uint16_t>& out) {
    std::vector<std::uint8_t> pdu{fc}, rsp;
    put16(pdu, start); put16(pdu, count);
    if (!exchange(pdu, rsp)) return false;
    if (rsp.size() != static_cast<std::size_t>(2 + count * 2) || rsp[1] != count * 2) {
        error_ = "incorrect response length"; disconnect(); return false;
    }
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) out[i] = u16(&rsp[2 + i * 2]);
    return true;
}
bool ModbusTcpClient::read_input(std::uint16_t s, std::uint16_t c, std::vector<std::uint16_t>& out) { return read(4, s, c, out); }
bool ModbusTcpClient::read_holding(std::uint16_t s, std::uint16_t c, std::vector<std::uint16_t>& out) { return read(3, s, c, out); }
bool ModbusTcpClient::write_multiple(std::uint16_t start, const std::vector<std::uint16_t>& values) {
    std::vector<std::uint8_t> pdu{16}, rsp;
    put16(pdu, start); put16(pdu, static_cast<std::uint16_t>(values.size()));
    pdu.push_back(static_cast<std::uint8_t>(values.size() * 2));
    for (const auto v : values) put16(pdu, v);
    if (!exchange(pdu, rsp)) return false;
    if (rsp.size() != 5 || u16(&rsp[1]) != start || u16(&rsp[3]) != values.size()) {
        error_ = "write echo mismatch"; disconnect(); return false;
    }
    return true;
}
}
