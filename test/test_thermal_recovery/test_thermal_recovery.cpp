#include <display/util/ThermalRecovery.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

// The same machine the banking tests use: a Silvia measured at 900 W on a plug meter, 21 °C tank, and the ~3.3 kJ/K
// effective capacity derived from its own shot logs. 98 °C setpoint is the real boiler figure for a 93 °C profile with
// the user's 5 °C offset, which is what recovery is handed.
static ThermalRecoveryParams silvia() {
    ThermalRecoveryParams p;
    p.setpointC = 98.0f;
    p.inletC = 21.0f;
    p.heaterWatts = 900.0f;
    p.boilerCapacityJPerK = 3300.0f;
    p.maxBoostK = 2.0f;
    p.ceilingC = 100.0f;
    p.deadbandK = 0.1f;
    p.maxDebtS = 60.0f;
    return p;
}

// Runs the balance forward at 100 ms, the cadence of the controller's sensor frame.
static void run(ThermalRecovery &r, float seconds, float flow, float duty, const ThermalRecoveryParams &p) {
    const int steps = static_cast<int>(seconds / 0.1f + 0.5f);
    for (int i = 0; i < steps; i++) {
        r.sample(0.1f, flow, duty, p);
    }
}

// Idle at the setpoint: the element ticks over against standing losses and no water moves, so there is nothing to
// recover and the offset has to be exactly zero -- anything else would park the boiler high between shots.
void test_idle_builds_no_debt() {
    ThermalRecovery r;
    run(r, 60.0f, 0.0f, 5.0f, silvia());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.debtJ());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.offsetK(silvia()));
}

// 1.3 ml/s at a 77 K rise needs 418 W. A saturated element more than covers it, so the debt stays at zero: the floor
// is doing its job, since the surplus is not credit to spend later.
void test_low_flow_stays_square() {
    ThermalRecovery r;
    run(r, 25.0f, 1.3f, 100.0f, silvia());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.debtJ());
}

// The fill phase that actually causes the sag: 9.85 ml/s for 5 s against a saturated element.
// removed  = 9.85 * 5 * 4.18 * 77 = 15 849 J
// restored = 900 * 5             =  4 500 J
// debt     =                       11 349 J, which is 3.44 K at 3.3 kJ/K
void test_fill_phase_builds_the_expected_debt() {
    ThermalRecovery r;
    const auto p = silvia();
    run(r, 5.0f, 9.85f, 100.0f, p);
    TEST_ASSERT_FLOAT_WITHIN(60.0f, 11349.0f, r.debtJ());
    // 3.44 K of debt, but the user allowed 2 K, so that is what is asked for.
    TEST_ASSERT_EQUAL_FLOAT(2.0f, r.offsetK(p));
}

// The point of subtracting the measured duty. Naively "E/P seconds of full power" would ask for 15 849 / 900 = 17.6 s
// here; the element already delivered 4 500 J during the phase, so the honest remaining figure is 12.6 s.
void test_debt_excludes_energy_already_delivered() {
    ThermalRecovery r;
    const auto p = silvia();
    run(r, 5.0f, 9.85f, 100.0f, p);
    const float naiveSeconds = 9.85f * 5.0f * 4.18f * 77.0f / 900.0f;
    const float honestSeconds = r.debtJ() / 900.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 17.6f, naiveSeconds);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 12.6f, honestSeconds);
}

// After the shot the pump stops and the element keeps running, so the debt drains at the element's rating and the
// offset follows it down. 11 349 J at 900 W is 12.6 s; half way through, 5 674 J remains.
void test_debt_drains_at_the_element_rating() {
    ThermalRecovery r;
    const auto p = silvia();
    run(r, 5.0f, 9.85f, 100.0f, p);
    run(r, 6.3f, 0.0f, 100.0f, p);
    TEST_ASSERT_FLOAT_WITHIN(120.0f, 5679.0f, r.debtJ());
    // Still past the 2 K cap (5 679 / 3 300 = 1.72 K), so the request is tapering rather than saturated.
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.72f, r.offsetK(p));
}

// The handover back to the PID is continuous: the offset shrinks to nothing rather than stepping off, so there is no
// edge for the boiler to overshoot through.
void test_recovery_settles_to_zero() {
    ThermalRecovery r;
    const auto p = silvia();
    run(r, 5.0f, 9.85f, 100.0f, p);
    run(r, 13.0f, 0.0f, 100.0f, p);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.debtJ());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.offsetK(p));
}

// A debt small enough to sit inside the deadband is treated as settled. Without this the measured balance never lands
// exactly on zero and the boiler would hold a fraction of a degree high indefinitely.
void test_deadband_treats_a_small_debt_as_settled() {
    ThermalRecovery r;
    const auto p = silvia();
    // 0.05 K at 3.3 kJ/K is 165 J; one 0.1 s step at 2 ml/s removes 64 J against 90 J of element output, so build the
    // debt with the element off and then let it run just short of square.
    run(r, 1.0f, 1.0f, 0.0f, p);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 322.0f, r.debtJ());
    run(r, 0.2f, 0.0f, 100.0f, p);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 142.0f, r.debtJ()); // 0.043 K, inside the 0.1 K deadband
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.offsetK(p));
}

// A 93 °C profile with the user's 5 °C offset puts the real setpoint at 98, so the 100 °C ceiling leaves room for 2 K
// and the 2 K cap is reachable. Push the setpoint to 99 and the ceiling, not the cap, becomes the bound.
void test_ceiling_bounds_the_boost() {
    ThermalRecovery r;
    auto p = silvia();
    p.setpointC = 99.0f;
    run(r, 5.0f, 9.85f, 100.0f, p);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.offsetK(p));

    p.setpointC = 100.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.offsetK(p)); // already at the ceiling
}

// maxBoostK is the switch. At zero the feature is off, and the debt is not tracked either, so turning it on mid-shot
// cannot act on an accounting period it did not observe.
void test_zero_cap_disables_recovery() {
    ThermalRecovery r;
    auto p = silvia();
    p.maxBoostK = 0.0f;
    run(r, 5.0f, 9.85f, 100.0f, p);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.debtJ());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.offsetK(p));
}

// Without a heater rating there is no repayment term and without a heat capacity the debt cannot become a temperature,
// so either missing has to disable the feature rather than divide by zero.
void test_missing_parameters_disable_recovery() {
    auto noHeater = silvia();
    noHeater.heaterWatts = 0.0f;
    ThermalRecovery a;
    run(a, 5.0f, 9.85f, 100.0f, noHeater);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.offsetK(noHeater));

    auto noCapacity = silvia();
    noCapacity.boilerCapacityJPerK = 0.0f;
    ThermalRecovery b;
    run(b, 5.0f, 9.85f, 100.0f, noCapacity);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b.offsetK(noCapacity));
}

// A setpoint at or below the inlet temperature means the water carries nothing out, so no debt accrues. The element
// term still runs, which is harmless: the floor holds the debt at zero.
void test_setpoint_below_inlet_removes_nothing() {
    ThermalRecovery r;
    auto p = silvia();
    p.setpointC = 20.0f;
    run(r, 5.0f, 9.85f, 0.0f, p);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.debtJ());
}

// A stuck flow reading cannot park the boiler hot indefinitely: the cap bounds the debt at 60 s of element output, so
// recovery unwinds within a minute of the reading clearing instead of hours.
void test_debt_is_capped() {
    ThermalRecovery r;
    const auto p = silvia();
    run(r, 600.0f, 9.85f, 100.0f, p);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 54000.0f, r.debtJ()); // 900 W * 60 s
    run(r, 60.0f, 0.0f, 100.0f, p);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.debtJ());
}

// Duty outside 0-100 % is clamped rather than trusted: a bad frame must not repay more than the element can deliver,
// and a negative one must not add to the debt.
void test_duty_is_clamped() {
    const auto p = silvia();
    ThermalRecovery high;
    high.sample(1.0f, 0.0f, 500.0f, p);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, high.debtJ());

    ThermalRecovery low;
    low.sample(1.0f, 2.0f, -50.0f, p);
    // 2 * 4.18 * 77 = 644 J removed, nothing repaid; a negative duty must not have added to it.
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 644.0f, low.debtJ());
}

// reset() is what the controller calls on leaving brew mode or losing the link. A debt measured before that no longer
// describes anything, and acting on it later would park the boiler high for a shot that never happened.
void test_reset_clears_the_debt() {
    ThermalRecovery r;
    const auto p = silvia();
    run(r, 5.0f, 9.85f, 100.0f, p);
    TEST_ASSERT_TRUE(r.debtJ() > 0.0f);
    r.reset();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.debtJ());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.offsetK(p));
}

// A whole Adaptive-v2-shaped shot: fill at 9.85 ml/s, bloom with the pump off, then extraction at 1.3 ml/s. The bloom
// is where recovery earns its keep -- the PID would be easing off as the temperature came back, while the measured
// debt keeps the element saturated until the energy is actually replaced.
void test_bloom_pays_down_the_fill_debt() {
    ThermalRecovery r;
    const auto p = silvia();
    run(r, 5.0f, 9.85f, 100.0f, p);
    const float afterFill = r.debtJ();
    run(r, 8.0f, 0.0f, 100.0f, p); // bloom: 7 200 J back
    const float afterBloom = r.debtJ();
    TEST_ASSERT_FLOAT_WITHIN(120.0f, afterFill - 7200.0f, afterBloom);
    TEST_ASSERT_TRUE(afterBloom > 0.0f); // not yet square, so the offset is still holding the element up
    run(r, 25.0f, 1.3f, 100.0f, p);     // extraction: 482 W of net repayment per second
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.debtJ());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_builds_no_debt);
    RUN_TEST(test_low_flow_stays_square);
    RUN_TEST(test_fill_phase_builds_the_expected_debt);
    RUN_TEST(test_debt_excludes_energy_already_delivered);
    RUN_TEST(test_debt_drains_at_the_element_rating);
    RUN_TEST(test_recovery_settles_to_zero);
    RUN_TEST(test_deadband_treats_a_small_debt_as_settled);
    RUN_TEST(test_ceiling_bounds_the_boost);
    RUN_TEST(test_zero_cap_disables_recovery);
    RUN_TEST(test_missing_parameters_disable_recovery);
    RUN_TEST(test_setpoint_below_inlet_removes_nothing);
    RUN_TEST(test_debt_is_capped);
    RUN_TEST(test_duty_is_clamped);
    RUN_TEST(test_reset_clears_the_debt);
    RUN_TEST(test_bloom_pays_down_the_fill_debt);
    return UNITY_END();
}
