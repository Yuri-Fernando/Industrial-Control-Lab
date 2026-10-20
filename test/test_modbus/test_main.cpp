// Modbus frame handling (no sockets), negative/fuzz tests, remote I/O node fail-safe and timing statistics.
#include <unity.h>
#include <cycle_stats.hpp>
#include <io.hpp>
#include <modbus.hpp>
#include <chrono>
#include <cstdint>
#include <vector>

using namespace icl;

void setUp() {}
void tearDown() {}

static std::vector<std::uint8_t> call(ModbusDevice& dev, const std::vector<std::uint8_t>& pdu) {
    const std::uint8_t head[7] = {0x12, 0x34, 0, 0, 0, static_cast<std::uint8_t>(pdu.size() + 1), 1};
    std::vector<std::uint8_t> packet;
    TEST_ASSERT_TRUE(handle_modbus_frame(dev, head, pdu, packet));
    TEST_ASSERT_EQUAL_UINT8(0x12, packet[0]);  // transaction id echoed
    TEST_ASSERT_EQUAL_UINT8(0x34, packet[1]);
    TEST_ASSERT_EQUAL_UINT16(packet.size() - 6, (packet[4] << 8) | packet[5]);  // MBAP length consistent
    return std::vector<std::uint8_t>(packet.begin() + 7, packet.end());
}

void test_fc03_reads_defaults() {
    RegisterBank bank;
    const auto r = call(bank, {3, 0, 1, 0, 2});
    TEST_ASSERT_EQUAL_UINT8(3, r[0]);
    TEST_ASSERT_EQUAL_UINT8(4, r[1]);
    TEST_ASSERT_EQUAL_UINT16(270, (r[2] << 8) | r[3]);  // setpoint 27.0 C
    TEST_ASSERT_EQUAL_UINT16(60, (r[4] << 8) | r[5]);   // heat load 60 %
}

void test_exceptions() {
    RegisterBank bank;
    TEST_ASSERT_EQUAL_HEX8(0x84, call(bank, {4, 0, 31, 0, 2})[0]);
    TEST_ASSERT_EQUAL_UINT8(2, call(bank, {4, 0, 31, 0, 2})[1]);   // off-range read
    TEST_ASSERT_EQUAL_UINT8(3, call(bank, {6, 0, 1, 1, 244})[1]);  // setpoint 50.0 C rejected
    TEST_ASSERT_EQUAL_UINT8(3, call(bank, {6, 0, 9, 0, 1})[1]);    // reserved register
    TEST_ASSERT_EQUAL_UINT8(1, call(bank, {5, 0, 0, 0xff, 0})[1]); // unsupported FC05
    TEST_ASSERT_EQUAL_UINT8(3, call(bank, {3, 0, 0, 0, 0})[1]);    // zero quantity
}

void test_fc16_atomic_commit() {
    RegisterBank bank;
    TEST_ASSERT_EQUAL_UINT8(16, call(bank, {16, 0, 1, 0, 2, 4, 1, 39, 0, 80})[0]);  // 295, 80
    TEST_ASSERT_EQUAL_UINT8(0x90, call(bank, {16, 0, 1, 0, 2, 4, 2, 188, 0, 50})[0]); // 700 invalid
    const auto s = bank.snapshot();
    TEST_ASSERT_EQUAL_UINT16(295, s.holding[1]);
    TEST_ASSERT_EQUAL_UINT16(80, s.holding[2]);  // second register untouched by the rejected frame
}

void test_command_fifo_bounded() {
    RegisterBank bank;
    for (int k = 0; k < 32; ++k) TEST_ASSERT_TRUE(bank.write_holding(0, 1));
    TEST_ASSERT_FALSE(bank.write_holding(0, 1));  // 33rd queued command rejected
    TEST_ASSERT_EQUAL_UINT16(1, bank.take_command());
}

void test_fuzz_random_frames_never_crash() {
    RegisterBank bank;
    IoNodeDevice node;
    std::uint64_t s = 42;
    auto rnd = [&] { s = s * 6364136223846793005ULL + 1; return static_cast<std::uint8_t>(s >> 56); };
    for (int k = 0; k < 20000; ++k) {
        std::vector<std::uint8_t> pdu(1 + rnd() % 40);
        for (auto& b : pdu) b = rnd();
        if (k % 3 == 0) pdu[0] = static_cast<std::uint8_t>(std::vector<int>{3, 4, 6, 16}[rnd() % 4]);
        const std::uint8_t head[7] = {0, 1, 0, 0, 0, static_cast<std::uint8_t>(pdu.size() + 1), 1};
        std::vector<std::uint8_t> out;
        TEST_ASSERT_TRUE(handle_modbus_frame(k % 2 ? static_cast<ModbusDevice&>(bank) : node, head, pdu, out));
        TEST_ASSERT_TRUE(out.size() >= 9);
    }
    // Registers stay within their validated ranges whatever the fuzzer wrote.
    const auto h = bank.snapshot().holding;
    TEST_ASSERT_TRUE(h[1] >= 240 && h[1] <= 380);
    TEST_ASSERT_TRUE(h[2] <= 100);
    for (int a = 7; a < 16; ++a) TEST_ASSERT_EQUAL_UINT16(0, h[a]);
}

void test_io_node_output_watchdog_failsafe() {
    IoNodeDevice node;
    auto t = std::chrono::steady_clock::now();
    std::vector<std::uint16_t> in;
    // controller block: pump on, cooling 50 %, heat 60 %, no faults, watchdog=1
    TEST_ASSERT_TRUE(node.write_multiple(0, {1, 500, 60, 0, 0, 0, 1}) == ModbusDevice::Status::OK);
    node.tick(0.01, t, 0);
    node.read_input(0, iomap::I_BLOCK, in);
    TEST_ASSERT_EQUAL_UINT16(1, in[iomap::I_PUMP_APPLIED]);
    TEST_ASSERT_EQUAL_UINT16(120, in[iomap::I_FLOW_X10]);
    TEST_ASSERT_EQUAL_UINT16(0, in[iomap::I_FAILSAFE_ACTIVE]);
    // controller goes silent for > 500 ms: outputs drop to the safe state
    node.tick(0.01, t + std::chrono::milliseconds(600), 0);
    node.read_input(0, iomap::I_BLOCK, in);
    TEST_ASSERT_EQUAL_UINT16(0, in[iomap::I_PUMP_APPLIED]);
    TEST_ASSERT_EQUAL_UINT16(0, in[iomap::I_FLOW_X10]);
    TEST_ASSERT_EQUAL_UINT16(1, in[iomap::I_FAILSAFE_ACTIVE]);
    TEST_ASSERT_EQUAL_UINT16(1, in[iomap::I_FAILSAFE_TRIPS]);
    // watchdog changes again: outputs follow commands
    node.write_multiple(iomap::H_OUTPUT_WATCHDOG, {2});
    node.tick(0.01, t + std::chrono::milliseconds(610), 0);
    node.read_input(0, iomap::I_BLOCK, in);
    TEST_ASSERT_EQUAL_UINT16(1, in[iomap::I_PUMP_APPLIED]);
}

void test_io_node_freeze_and_injection() {
    IoNodeDevice node;
    auto t = std::chrono::steady_clock::now();
    std::vector<std::uint16_t> a, b;
    node.write_multiple(0, {0, 0, 60, 0, 1, 1, 1});  // sensor fault + overtemp offset
    node.tick(0.01, t, 0);
    node.read_input(0, iomap::I_BLOCK, a);
    TEST_ASSERT_EQUAL_UINT16(0, a[iomap::I_SENSOR_VALID]);
    TEST_ASSERT_TRUE(a[iomap::I_TEMPERATURE_X10] >= a[iomap::I_PLANT_TEMPERATURE_X10] + 499);
    TEST_ASSERT_TRUE(node.write_multiple(iomap::H_BENCH_FREEZE, {1}) == ModbusDevice::Status::OK);
    node.tick(0.01, t + std::chrono::milliseconds(10), 0);
    node.tick(0.01, t + std::chrono::milliseconds(20), 0);
    node.read_input(0, iomap::I_BLOCK, b);
    TEST_ASSERT_EQUAL_UINT16(a[iomap::I_SAMPLE_SEQ], b[iomap::I_SAMPLE_SEQ]);  // stuck transmitter
    TEST_ASSERT_TRUE(node.write_multiple(iomap::H_BENCH_LATENCY_MS, {2001}) == ModbusDevice::Status::ILLEGAL_VALUE);
    TEST_ASSERT_TRUE(node.write_multiple(iomap::H_BENCH_LATENCY_MS, {150}) == ModbusDevice::Status::OK);
    TEST_ASSERT_EQUAL_INT(150, static_cast<int>(node.response_delay().count()));
}

void test_sliding_percentiles() {
    SlidingPercentiles w(100);
    for (std::uint32_t v = 1; v <= 100; ++v) w.add(v);
    auto p = w.compute();
    TEST_ASSERT_EQUAL_UINT32(51, p.p50);
    TEST_ASSERT_EQUAL_UINT32(95, p.p95);
    TEST_ASSERT_EQUAL_UINT32(99, p.p99);
    TEST_ASSERT_EQUAL_UINT32(100, p.max);
    for (int k = 0; k < 100; ++k) w.add(7);  // window slides: old samples evicted
    p = w.compute();
    TEST_ASSERT_EQUAL_UINT32(7, p.max);
    TEST_ASSERT_EQUAL_UINT32(100, static_cast<std::uint32_t>(p.samples));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_fc03_reads_defaults);
    RUN_TEST(test_exceptions);
    RUN_TEST(test_fc16_atomic_commit);
    RUN_TEST(test_command_fifo_bounded);
    RUN_TEST(test_fuzz_random_frames_never_crash);
    RUN_TEST(test_io_node_output_watchdog_failsafe);
    RUN_TEST(test_io_node_freeze_and_injection);
    RUN_TEST(test_sliding_percentiles);
    return UNITY_END();
}
