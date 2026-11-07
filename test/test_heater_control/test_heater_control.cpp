#include <display/util/HeaterControlModel.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

// The author's machine: 900 W, so Kff = 1000 / 900 = 1.111, with the gains its autotune produced.
static HeaterControlParams silvia() {
    HeaterControlParams c;
    c.kp = 29.0f;
    c.ki = 0.11f;
    c.kd = 379.5f;
    c.kff = 1.111f;
    c.inletC = 21.0f;
    return c;
}

// ---------------------------------------------------------------------------
// Safety scaling -- Heater::calculateSafetyScaling
// ---------------------------------------------------------------------------

// The cut-off that decides everything else: past +1 C the feedforward is gone, so no amount of flow pins the element.
void test_safety_cuts_off_above_one_degree() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, heaterSafetyScaling(1.01f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, heaterSafetyScaling(11.7f));
}

void test_safety_is_full_at_setpoint_and_below() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, heaterSafetyScaling(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, heaterSafetyScaling(-2.0f));
}

// 0.7 is the floor wherever the function is non-zero, which is what the duty test leans on. It is reached at +1 C,
// and also just below the setpoint -- the firmware's two branches do not meet at zero, so the factor jumps from 0.7
// to 1.0 as the error crosses it. Reproduced rather than smoothed: the tools have to match the machine, and the jump
// is only worth 30 % of a feedforward term that is already three times the span where this matters.
void test_safety_floor_is_seventy_percent() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.7f, heaterSafetyScaling(1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.7f, heaterSafetyScaling(-0.0001f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.85f, heaterSafetyScaling(0.5f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.85f, heaterSafetyScaling(-0.5f));
    // The discontinuity itself, asserted so a tidy-up cannot silently change behaviour the gate depends on.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, heaterSafetyScaling(0.0f));
}

// ---------------------------------------------------------------------------
// Feedforward
// ---------------------------------------------------------------------------

// The number the whole gate fix turns on. At 8 ml/s, 93 C and a 21 C tank:
//   (8 * 4.18 * 72 + 5) / 0.95 * 1.111 = 2822 output units, against a span of 1000.
void test_feedforward_at_a_fast_fill_is_nearly_three_spans() {
    const float ff = heaterFeedforwardUnits(silvia(), 93.0f, 8.0f);
    TEST_ASSERT_FLOAT_WITHIN(15.0f, 2822.0f, ff);
    TEST_ASSERT_TRUE(ff > 2.0f * HEATER_OUTPUT_SPAN);
}

void test_feedforward_is_zero_without_flow_or_gain() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, heaterFeedforwardUnits(silvia(), 93.0f, 0.0f));
    HeaterControlParams noGain = silvia();
    noGain.kff = 0.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, heaterFeedforwardUnits(noGain, 93.0f, 8.0f));
}

// A setpoint at or below the tank asks for no heating of the incoming water.
void test_feedforward_is_zero_at_or_below_inlet() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, heaterFeedforwardUnits(silvia(), 21.0f, 8.0f));
}

// ---------------------------------------------------------------------------
// The duty gate
// ---------------------------------------------------------------------------

// The case the old gate threw away, and the reason for this header. Shot 55: the sensor sits ON its 93 C setpoint
// while the fill runs at 8 ml/s. The element is pinned regardless, because the feedforward alone is worth 2822.
void test_duty_is_full_at_setpoint_under_a_fast_fill() {
    TEST_ASSERT_TRUE(heaterDutyCertainlyFull(silvia(), 93.0f, 93.0f, 8.0f, 1.5f));
    TEST_ASSERT_TRUE(heaterDutyCertainlyFull(silvia(), 93.5f, 93.0f, 8.0f, 1.5f));
}

// The case the old gate was added to reject, still rejected -- but for the right reason now. An over-temperature
// machine is 11.7 C above its setpoint, far past where the safety scaling has switched the feedforward off.
void test_duty_is_unknown_on_an_over_temperature_machine() {
    TEST_ASSERT_FALSE(heaterDutyCertainlyFull(silvia(), 104.7f, 93.0f, 8.0f, 1.5f));
}

// A gentle shot never gets there, whatever the temperature is doing.
void test_duty_is_unknown_at_a_slow_flow() {
    TEST_ASSERT_FALSE(heaterDutyCertainlyFull(silvia(), 90.0f, 93.0f, 1.3f, 1.5f));
    TEST_ASSERT_FALSE(heaterDutyCertainlyFull(silvia(), 93.0f, 93.0f, 0.0f, 1.5f));
}

// Below the setpoint the safety scaling is full, so it takes less flow than it does at the setpoint.
void test_a_cold_boiler_needs_less_flow_than_a_hot_one() {
    const float cold = heaterSaturatingFlow(silvia(), 93.0f, 1.5f);
    TEST_ASSERT_TRUE(cold > 0.0f);
    TEST_ASSERT_TRUE(heaterDutyCertainlyFull(silvia(), 85.0f, 93.0f, cold + 0.1f, 1.5f));
    TEST_ASSERT_FALSE(heaterDutyCertainlyFull(silvia(), 85.0f, 93.0f, cold - 0.5f, 1.5f));
}

// The threshold and the test have to agree, or a tool reports one number and applies another.
void test_saturating_flow_matches_the_gate() {
    const HeaterControlParams c = silvia();
    const float needed = heaterSaturatingFlow(c, 93.0f, 1.5f);
    TEST_ASSERT_TRUE(heaterDutyCertainlyFull(c, 93.0f, 93.0f, needed * 1.01f, 1.5f));
    TEST_ASSERT_FALSE(heaterDutyCertainlyFull(c, 93.0f, 93.0f, needed * 0.99f, 1.5f));
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------

// A fast fill drives the output to the stop, so the reconstruction reports exactly 100 %.
void test_control_saturates_through_a_fill() {
    HeaterControlState state;
    const HeaterControlParams c = silvia();
    heaterSeedIntegral(state, c, 0.1f);
    float duty = 0.0f;
    for (int i = 0; i < 6; i++) {
        duty = heaterControlStep(state, c, 1.0f, 93.0f, 93.0f, 8.0f, true);
    }
    TEST_ASSERT_EQUAL_FLOAT(1.0f, duty);
}

// Well above the setpoint with no flow, the element is off: the proportional term is negative and nothing offsets it.
void test_control_shuts_off_when_too_hot() {
    HeaterControlState state;
    const HeaterControlParams c = silvia();
    heaterSeedIntegral(state, c, 0.1f);
    float duty = 1.0f;
    for (int i = 0; i < 5; i++) {
        duty = heaterControlStep(state, c, 1.0f, 99.0f, 93.0f, 0.0f, false);
    }
    TEST_ASSERT_EQUAL_FLOAT(0.0f, duty);
}

// The seeded integral is what stops a shot opening with the element off. A machine idling exactly at its setpoint has
// no error at all, so every term but the integral is zero and the duty is whatever the integral holds.
void test_seeded_integral_holds_the_idle_duty() {
    HeaterControlState state;
    const HeaterControlParams c = silvia();
    heaterSeedIntegral(state, c, 0.12f);
    const float duty = heaterControlStep(state, c, 1.0f, 93.0f, 93.0f, 0.0f, false);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.12f, duty);
}

// The PID runs at 1 Hz, so a log sampled four times a second must see the previous value held rather than four fresh
// computations -- that is what the real software PWM does.
void test_duty_is_held_between_ticks() {
    HeaterControlState state;
    const HeaterControlParams c = silvia();
    heaterSeedIntegral(state, c, 0.12f);
    const float first = heaterControlStep(state, c, 0.25f, 80.0f, 93.0f, 0.0f, false);
    const float held = heaterControlStep(state, c, 0.25f, 60.0f, 93.0f, 0.0f, false);
    TEST_ASSERT_EQUAL_FLOAT(first, held);
}

// Duty is a fraction; nothing downstream should ever have to clamp it again.
void test_duty_stays_in_range() {
    HeaterControlState state;
    const HeaterControlParams c = silvia();
    for (int i = 0; i < 60; i++) {
        const float sensor = 20.0f + static_cast<float>(i);
        const float duty = heaterControlStep(state, c, 1.0f, sensor, 93.0f, i % 3 == 0 ? 8.0f : 0.0f, true);
        TEST_ASSERT_TRUE(duty >= 0.0f && duty <= 1.0f);
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_safety_cuts_off_above_one_degree);
    RUN_TEST(test_safety_is_full_at_setpoint_and_below);
    RUN_TEST(test_safety_floor_is_seventy_percent);
    RUN_TEST(test_feedforward_at_a_fast_fill_is_nearly_three_spans);
    RUN_TEST(test_feedforward_is_zero_without_flow_or_gain);
    RUN_TEST(test_feedforward_is_zero_at_or_below_inlet);
    RUN_TEST(test_duty_is_full_at_setpoint_under_a_fast_fill);
    RUN_TEST(test_duty_is_unknown_on_an_over_temperature_machine);
    RUN_TEST(test_duty_is_unknown_at_a_slow_flow);
    RUN_TEST(test_a_cold_boiler_needs_less_flow_than_a_hot_one);
    RUN_TEST(test_saturating_flow_matches_the_gate);
    RUN_TEST(test_control_saturates_through_a_fill);
    RUN_TEST(test_control_shuts_off_when_too_hot);
    RUN_TEST(test_seeded_integral_holds_the_idle_duty);
    RUN_TEST(test_duty_is_held_between_ticks);
    RUN_TEST(test_duty_stays_in_range);
    return UNITY_END();
}
