#include <unity.h>
#include <control.hpp>
using namespace icl;

void setUp() {}
void tearDown() {}

static void run_to(Controller& ctl, Inputs& in) {
    in.start=true; ctl.step(in,0.05);
    in.start=false; in.flow_lpm=12; ctl.step(in,0.05);
}

void test_start_and_cooling() {
    Controller ctl;
    Inputs in;
    in.start=true; in.heartbeat_fresh=true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::STARTING),static_cast<int>(ctl.step(in,0.05).state));
    in.start=false; in.flow_lpm=12;
    auto result=ctl.step(in,0.05);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::RUNNING),static_cast<int>(result.state));
    TEST_ASSERT_TRUE(result.pump);
    TEST_ASSERT_TRUE(result.cooling_pct>0);
}
void test_low_flow_latches_fault() {
    Controller ctl;
    Inputs in;
    in.start=true; ctl.step(in,0.05);
    in.start=false; in.flow_lpm=12; ctl.step(in,0.05);
    in.flow_lpm=0;
    auto result=ctl.step(in,0.05);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::FAULT),static_cast<int>(result.state));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Fault::LOW_FLOW),static_cast<int>(result.fault));
    TEST_ASSERT_FALSE(result.pump);
    in.flow_lpm=12;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::FAULT),static_cast<int>(ctl.step(in,0.05).state));
    in.reset=true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::STOPPED),static_cast<int>(ctl.step(in,0.05).state));
}
void test_emergency_stop_priority() {
    Controller ctl;
    Inputs in;
    in.start=true; in.estop=true;
    auto out=ctl.step(in,0.05);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::ESTOP),static_cast<int>(out.state));
    TEST_ASSERT_FALSE(out.pump);
    in.estop=false; in.start=false; in.stop=true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::ESTOP),static_cast<int>(ctl.step(in,0.05).state));
}
void test_sensor_watchdog_overtemp() {
    for (int mode=0;mode<3;++mode) {
        Controller ctl;
        Inputs in;
        in.start=true; ctl.step(in,0.05);
        in.start=false; in.flow_lpm=12;
        ctl.step(in,0.05);
        if(mode==0) in.sensor_valid=false;
        if(mode==1) in.heartbeat_fresh=false;
        if(mode==2) in.temperature_c=50;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(State::FAULT),static_cast<int>(ctl.step(in,0.05).state));
    }
}
void test_stop_never_clears_latched_fault() {
    Controller ctl;
    Inputs in;
    run_to(ctl, in);
    in.sensor_valid=false; ctl.step(in,0.05);
    in.sensor_valid=true; in.stop=true;
    const auto out=ctl.step(in,0.05);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::FAULT),static_cast<int>(out.state));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Fault::SENSOR_INVALID),static_cast<int>(out.fault));
}
void test_io_comm_loss_trips_and_blocks_start_and_reset() {
    Controller ctl;
    Inputs in;
    run_to(ctl, in);
    in.io_healthy=false;
    auto out=ctl.step(in,0.05);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Fault::IO_COMM_LOSS),static_cast<int>(out.fault));
    TEST_ASSERT_FALSE(out.pump);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, out.cooling_pct);
    in.reset=true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::FAULT),static_cast<int>(ctl.step(in,0.05).state));
    in.io_healthy=true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::STOPPED),static_cast<int>(ctl.step(in,0.05).state));
    in.reset=false; in.io_healthy=false; in.start=true;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::STOPPED),static_cast<int>(ctl.step(in,0.05).state));
}
void test_startup_timeout_is_two_seconds() {
    Controller ctl;
    Inputs in;
    in.start=true; ctl.step(in,0.05);
    in.start=false;
    int scans=1;
    while (ctl.output().state==State::STARTING && scans<100) { ctl.step(in,0.05); ++scans; }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Fault::LOW_FLOW),static_cast<int>(ctl.output().fault));
    TEST_ASSERT_INT_WITHIN(1, 40, scans);  // 2.0 s / 50 ms
}
void test_safety_trip_counter_counts_each_trip_once() {
    Controller ctl;
    Inputs in;
    run_to(ctl, in);
    in.temperature_c=50; ctl.step(in,0.05); ctl.step(in,0.05);
    TEST_ASSERT_EQUAL_UINT32(1, ctl.output().safety_trips);
    in.estop=true; ctl.step(in,0.05); ctl.step(in,0.05);
    TEST_ASSERT_EQUAL_UINT32(2, ctl.output().safety_trips);
}
void test_pi_anti_windup_recovers_quickly() {
    PiCooling pi;
    for (int k=0;k<2000;++k) TEST_ASSERT_EQUAL_DOUBLE(100.0, pi.update(true, 20.0, 0.05));  // saturated
    // Integral must not have wound up: a negative error drops output to zero immediately.
    TEST_ASSERT_EQUAL_DOUBLE(0.0, pi.update(true, -2.0, 0.05));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, pi.update(false, 5.0, 0.05));
}
void test_closed_loop_reaches_setpoint() {
    Controller ctl;
    Plant plant;
    Inputs in;
    in.setpoint_c=27.0;
    in.start=true;
    for (int k=0;k<20*600;++k) {  // 10 simulated minutes
        in.temperature_c=plant.temperature(); in.flow_lpm=plant.flow();
        const auto out=ctl.step(in,0.05);
        in.start=false;
        plant.step(0.05,out.pump,out.cooling_pct,false,60);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::RUNNING),static_cast<int>(ctl.output().state));
    TEST_ASSERT_DOUBLE_WITHIN(0.3, 27.0, plant.temperature());
}

int main(int,char**) {
    UNITY_BEGIN();
    RUN_TEST(test_start_and_cooling);
    RUN_TEST(test_low_flow_latches_fault);
    RUN_TEST(test_emergency_stop_priority);
    RUN_TEST(test_sensor_watchdog_overtemp);
    RUN_TEST(test_stop_never_clears_latched_fault);
    RUN_TEST(test_io_comm_loss_trips_and_blocks_start_and_reset);
    RUN_TEST(test_startup_timeout_is_two_seconds);
    RUN_TEST(test_safety_trip_counter_counts_each_trip_once);
    RUN_TEST(test_pi_anti_windup_recovers_quickly);
    RUN_TEST(test_closed_loop_reaches_setpoint);
    return UNITY_END();
}
