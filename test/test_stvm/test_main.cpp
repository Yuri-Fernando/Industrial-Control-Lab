// stvm interpreter tests + step-by-step differential test of st/FB_Interlocks.st against the C++ engine.
#include <unity.h>
#include <control.hpp>
#include <st_controller.hpp>
#include <stvm.hpp>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

using namespace icl;

void setUp() {}
void tearDown() {}

static std::string st_path() {
    for (const char* p : {"st/FB_Interlocks.st", "../st/FB_Interlocks.st", "../../st/FB_Interlocks.st"}) {
        std::ifstream f(p);
        if (f) return p;
    }
    return "st/FB_Interlocks.st";
}

static st::Instance run_src(const std::string& body, const std::string& vars) {
    st::Instance fb(st::compile("FUNCTION_BLOCK T\n" + vars + "\n" + body + "\nEND_FUNCTION_BLOCK"));
    fb.execute();
    return fb;
}

void test_precedence_and_arithmetic() {
    auto fb = run_src("A := 2 + 3 * 4; B := (2 + 3) * 4; C := 7 MOD 3; D := -2 * 3 + 10 / 4;"
                      "X := 1 < 2 AND 3 > 2 OR FALSE; Y := NOT TRUE = FALSE; R := 1.5 * 2.0 + 1;",
                      "VAR A, B, C, D : DINT; X, Y : BOOL; R : LREAL; END_VAR");
    TEST_ASSERT_EQUAL_INT64(14, fb.get_int("A"));
    TEST_ASSERT_EQUAL_INT64(20, fb.get_int("B"));
    TEST_ASSERT_EQUAL_INT64(1, fb.get_int("C"));
    TEST_ASSERT_EQUAL_INT64(-4, fb.get_int("D"));  // integer division truncates
    TEST_ASSERT_TRUE(fb.get_bool("X"));
    TEST_ASSERT_TRUE(fb.get_bool("Y"));             // (NOT TRUE) = FALSE  (NOT binds tighter than =)
    TEST_ASSERT_EQUAL_DOUBLE(4.0, fb.get_real("R"));
}

void test_if_case_and_builtins() {
    auto fb = run_src(
        "CASE S OF 0: O := 10; 1, 2: O := 20; 3..5: O := 30; ELSE O := 99; END_CASE;"
        "IF O = 30 THEN P := 1; ELSIF O = 20 THEN P := 2; ELSE P := 3; END_IF;"
        "L := LIMIT(0.0, 7.5, 5.0); M := MAX(1, 9, 4); N := MIN(3, -2); Q := ABS(-3.25); Z := SEL(TRUE, 1, 2);",
        "VAR S : INT := 4; O, P, M, N, Z : DINT; L, Q : LREAL; END_VAR");
    TEST_ASSERT_EQUAL_INT64(30, fb.get_int("O"));
    TEST_ASSERT_EQUAL_INT64(1, fb.get_int("P"));
    TEST_ASSERT_EQUAL_DOUBLE(5.0, fb.get_real("L"));
    TEST_ASSERT_EQUAL_INT64(9, fb.get_int("M"));
    TEST_ASSERT_EQUAL_INT64(-2, fb.get_int("N"));
    TEST_ASSERT_EQUAL_DOUBLE(3.25, fb.get_real("Q"));
    TEST_ASSERT_EQUAL_INT64(2, fb.get_int("Z"));
}

void test_type_fidelity() {
    auto fb = run_src("I := 32767; I := I + 1; F := 0.1; D := 0.1;",
                      "VAR I : INT; F : REAL; D : LREAL; END_VAR");
    TEST_ASSERT_EQUAL_INT64(-32768, fb.get_int("I"));  // INT is 16-bit two's complement
    TEST_ASSERT_TRUE(fb.get_real("F") != 0.1);          // REAL rounds to float32
    TEST_ASSERT_EQUAL_DOUBLE(0.1, fb.get_real("D"));
}

void test_state_persists_between_scans() {
    st::Instance fb(st::compile("FUNCTION_BLOCK T VAR_INPUT Inc : BOOL; END_VAR VAR_OUTPUT N : DINT; END_VAR "
                                "IF Inc THEN N := N + 1; END_IF; END_FUNCTION_BLOCK"));
    fb.set("Inc", true);
    for (int k = 0; k < 5; ++k) fb.execute();
    fb.set("inc", false);  // identifiers are case-insensitive
    fb.execute();
    TEST_ASSERT_EQUAL_INT64(5, fb.get_int("N"));
}

void test_compile_errors_are_reported_with_line() {
    const char* bad[] = {
        "FUNCTION_BLOCK T VAR B : BOOL; END_VAR B := 1; END_FUNCTION_BLOCK",                    // type mismatch
        "FUNCTION_BLOCK T VAR I : INT; END_VAR I := 1.5; END_FUNCTION_BLOCK",                   // implicit narrowing
        "FUNCTION_BLOCK T VAR_INPUT X : BOOL; END_VAR X := TRUE; END_FUNCTION_BLOCK",           // write to input
        "FUNCTION_BLOCK T VAR CONSTANT K : INT := 1; END_VAR K := 2; END_FUNCTION_BLOCK",       // write to constant
        "FUNCTION_BLOCK T VAR A : INT; END_VAR A := Undeclared; END_FUNCTION_BLOCK",
        "FUNCTION_BLOCK T VAR A : INT; END_VAR IF A THEN A := 1; END_IF; END_FUNCTION_BLOCK",   // non-BOOL condition
        "FUNCTION_BLOCK T VAR A : INT; END_VAR\n\nA := 1 END_FUNCTION_BLOCK",                    // missing ';'
        "FUNCTION_BLOCK T VAR A : INT; END_VAR (* open comment END_FUNCTION_BLOCK",
    };
    for (const char* src : bad) {
        bool thrown = false;
        try { st::compile(src); } catch (const st::StError& e) { thrown = true; TEST_ASSERT_TRUE(e.line() >= 1); }
        TEST_ASSERT_TRUE_MESSAGE(thrown, src);
    }
}

void test_runtime_division_by_zero_throws() {
    st::Instance fb(st::compile("FUNCTION_BLOCK T VAR A : DINT; B : DINT; END_VAR A := 1 / B; END_FUNCTION_BLOCK"));
    bool thrown = false;
    try { fb.execute(); } catch (const st::StError&) { thrown = true; }
    TEST_ASSERT_TRUE(thrown);
}

void test_project_st_block_compiles() {
    const auto prog = st::compile_file(st_path());
    TEST_ASSERT_EQUAL_STRING("FB_INTERLOCKS", prog->pou_name.c_str());
}

// ---------------------------------------------------------------- differential testing
static void assert_same(const Outputs& a, const Outputs& b, std::uint32_t step) {
    char msg[64];
    std::snprintf(msg, sizeof msg, "diverged at step %u", static_cast<unsigned>(step));
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(a.state), static_cast<int>(b.state), msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(a.fault), static_cast<int>(b.fault), msg);
    TEST_ASSERT_EQUAL_MESSAGE(a.pump, b.pump, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.safety_trips, b.safety_trips, msg);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-9, a.cooling_pct, b.cooling_pct, msg);
}

struct Lcg {
    std::uint64_t s;
    std::uint32_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<std::uint32_t>(s >> 33); }
    double unit() { return next() / 2147483648.0; }
    bool chance(double p) { return unit() < p; }
};

void test_differential_random_inputs() {
    const auto prog = st::compile_file(st_path());
    std::uint32_t states_seen = 0, faults_seen = 0;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        Controller cpp;
        StController st(prog);
        Lcg rng{seed};
        Inputs in;
        for (std::uint32_t k = 0; k < 25000; ++k) {
            in.start = rng.chance(0.08);
            in.stop = rng.chance(0.02);
            in.reset = rng.chance(0.05);
            in.estop = rng.chance(0.005);
            in.temperature_c = rng.chance(0.01) ? 45.0 : 20.0 + rng.unit() * (rng.chance(0.02) ? 40.0 : 22.0);
            in.flow_lpm = rng.chance(0.8) ? 12.0 : (rng.chance(0.5) ? 0.0 : rng.unit() * 10.0);
            in.sensor_valid = !rng.chance(0.01);
            in.heartbeat_fresh = !rng.chance(0.005);
            in.io_healthy = !rng.chance(0.005);
            in.low_flow_fault_active = rng.chance(0.05);
            in.setpoint_c = 24.0 + rng.unit() * 14.0;
            const double dt = rng.chance(0.9) ? 0.05 : rng.unit() * 0.3;
            const auto a = cpp.step(in, dt);
            const auto b = st.step(in, dt);
            assert_same(a, b, k);
            states_seen |= 1u << static_cast<unsigned>(a.state);
            faults_seen |= 1u << static_cast<unsigned>(a.fault);
        }
        TEST_ASSERT_EQUAL_STRING("", st.runtime_error().c_str());
    }
    TEST_ASSERT_EQUAL_HEX32(0x1F, states_seen);   // every state visited
    TEST_ASSERT_EQUAL_HEX32(0x7F, faults_seen);   // every fault code produced
}

void test_differential_closed_loop_with_plant() {
    const auto prog = st::compile_file(st_path());
    Controller cpp;
    StController st(prog);
    Plant pa, pb;
    Inputs ia, ib;
    for (std::uint32_t k = 0; k < 20 * 300; ++k) {
        const bool start = k == 1 || k == 3000, low_flow = k > 2000 && k < 2100, reset = k == 2500;
        for (Inputs* in : {&ia, &ib}) {
            in->start = start; in->reset = reset; in->low_flow_fault_active = low_flow;
        }
        ia.temperature_c = pa.temperature(); ia.flow_lpm = pa.flow();
        ib.temperature_c = pb.temperature(); ib.flow_lpm = pb.flow();
        const auto a = cpp.step(ia, 0.05);
        const auto b = st.step(ib, 0.05);
        assert_same(a, b, k);
        pa.step(0.05, a.pump, a.cooling_pct, low_flow, 60);
        pb.step(0.05, b.pump, b.cooling_pct, low_flow, 60);
        TEST_ASSERT_EQUAL_DOUBLE(pa.temperature(), pb.temperature());
    }
    TEST_ASSERT_EQUAL_UINT32(1, cpp.output().safety_trips);
}

void test_st_runtime_error_fails_safe() {
    StController st(st::compile(
        "FUNCTION_BLOCK B VAR_INPUT RunRequest, StopRequest, ResetRequest, EStopRequest, LowFlowFaultActive,"
        " SensorValid, HeartbeatHealthy, IoHealthy : BOOL; TemperatureC, FlowLpm, CycleTimeS : LREAL; END_VAR"
        " VAR_OUTPUT PumpEnable, CoolingEnable : BOOL; State, FaultCode : INT; SafetyTrips : DINT; END_VAR"
        " VAR Z : DINT; END_VAR PumpEnable := TRUE; State := 2; State := State / Z; END_FUNCTION_BLOCK"));
    Inputs in;
    const auto out = st.step(in, 0.05);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::FAULT), static_cast<int>(out.state));
    TEST_ASSERT_FALSE(out.pump);
    TEST_ASSERT_FALSE(st.runtime_error().empty());
    TEST_ASSERT_FALSE(st.step(in, 0.05).pump);  // stays de-energized
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_precedence_and_arithmetic);
    RUN_TEST(test_if_case_and_builtins);
    RUN_TEST(test_type_fidelity);
    RUN_TEST(test_state_persists_between_scans);
    RUN_TEST(test_compile_errors_are_reported_with_line);
    RUN_TEST(test_runtime_division_by_zero_throws);
    RUN_TEST(test_project_st_block_compiles);
    RUN_TEST(test_differential_random_inputs);
    RUN_TEST(test_differential_closed_loop_with_plant);
    RUN_TEST(test_st_runtime_error_fails_safe);
    return UNITY_END();
}
