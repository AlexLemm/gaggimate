#include <display/util/StandingLoss.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

// The author's machine: a 900 W element, a room at 21 C. The idle duty used throughout is 4 %, which is what the
// websocket actually reported while the boiler held 85 C -- 36 W, so 0.56 W/K.
static StandingLossConfig silvia() {
    StandingLossConfig c;
    c.heaterWatts = 900.0f;
    c.ambientC = 21.0f;
    return c;
}

// A boiler holding steady at sensorC on a constant duty, for seconds.
static void idle(StandingLossMeter &meter, float seconds, float sensorC, float duty, float dtS = 0.5f) {
    for (float t = 0.0f; t < seconds; t += dtS) {
        meter.sample(dtS, sensorC, 0.0f, duty);
    }
}

// The whole point of the file: no model, no optimiser, and the answer is exactly the arithmetic. 4 % of 900 W is
// 36 W, held against 64 K above the room, so 0.5625 W/K and nothing else.
static void test_steady_idle_gives_the_exact_ratio() {
    StandingLossMeter meter(silvia());
    idle(meter, 300.0f, 85.0f, 0.04f);
    meter.finish();

    TEST_ASSERT_TRUE(meter.identified());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5625f, meter.lossWPerK());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 36.0f, meter.holdingWatts());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 85.0f, meter.meanTempC());
    TEST_ASSERT_EQUAL_INT(1, meter.windows());
}

// A flat trace leaves nothing for the capacity term, so the bound on it is zero however large the capacity is. This
// is what licenses dropping that term at all.
static void test_a_flat_trace_has_no_drift_to_explain() {
    StandingLossMeter meter(silvia());
    idle(meter, 300.0f, 85.0f, 0.04f);
    meter.finish();

    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.0f, meter.driftBoundWPerK(3500.0f));
}

// And a machine still settling says so. 2 C of drift over 300 s at 85 C: 3500 * 2 / (64 * 300) = 0.36 W/K, which is
// most of the answer -- the case the tool is meant to warn about rather than quietly average away.
static void test_drift_is_bounded_not_ignored() {
    StandingLossMeter meter(silvia());
    for (int i = 0; i < 600; i++) {
        meter.sample(0.5f, 84.0f + 2.0f * static_cast<float>(i) / 599.0f, 0.0f, 0.04f);
    }
    meter.finish();

    const float bound = meter.driftBoundWPerK(3500.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.36f, bound);
    TEST_ASSERT_TRUE(bound > 0.25f * meter.lossWPerK());
}

// Water leaving the boiler is a second way out for the heat, and this estimator knows about only one. So a draw ends
// the window rather than being averaged into it -- otherwise a flush would be read as an enormous standing loss.
static void test_flow_ends_the_window() {
    StandingLossMeter meter(silvia());
    idle(meter, 100.0f, 85.0f, 0.04f);
    for (int i = 0; i < 20; i++) {
        meter.sample(0.5f, 85.0f, 8.0f, 1.0f); // a fill at full duty
    }
    idle(meter, 100.0f, 85.0f, 0.04f);
    meter.finish();

    // Two clean windows, and the fill contributed to neither: 100 % duty folded in would have wrecked the ratio.
    TEST_ASSERT_EQUAL_INT(2, meter.windows());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5625f, meter.lossWPerK());
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 200.0f, meter.idleS());
}

// A stretch too short for the duty's own cycling to average out is thrown away rather than used. The element on this
// machine cycles over tens of seconds, so ten seconds of it says only where in the cycle the capture started.
static void test_a_short_window_is_discarded() {
    StandingLossMeter meter(silvia());
    idle(meter, 20.0f, 85.0f, 1.0f); // 20 s with the element flat out
    meter.finish();

    TEST_ASSERT_FALSE(meter.identified());
    TEST_ASSERT_EQUAL_INT(0, meter.windows());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, meter.lossWPerK());
}

// The window in progress has not been counted. Forgetting finish() on the last stretch of a capture would silently
// throw away the only window a short capture has.
static void test_finish_is_what_counts_the_last_window() {
    StandingLossMeter meter(silvia());
    idle(meter, 300.0f, 85.0f, 0.04f);

    TEST_ASSERT_FALSE(meter.identified());
    meter.finish();
    TEST_ASSERT_TRUE(meter.identified());
}

// Duty cycling around a mean is the real case: the element is bang-bang over seconds, not a steady 4 %. The mean is
// what the energy integral sees, which is why windows are used instead of instants.
static void test_a_cycling_element_averages_to_the_same_answer() {
    StandingLossMeter meter(silvia());
    // 5 % duty as 3 s on in every 60, rather than a constant 5 %.
    for (int cycle = 0; cycle < 10; cycle++) {
        for (int i = 0; i < 120; i++) {
            meter.sample(0.5f, 85.0f, 0.0f, i < 6 ? 1.0f : 0.0f);
        }
    }
    meter.finish();

    TEST_ASSERT_FLOAT_WITHIN(0.005f, 900.0f * 0.05f / 64.0f, meter.lossWPerK());
}

// The answer is an integral, so it cannot depend on how often the capture happened to sample. thermalcap records at
// 2 Hz and drops intervals across a reconnect, so the rate is not even constant within one file.
static void test_the_answer_does_not_depend_on_the_sample_rate() {
    StandingLossMeter fast(silvia());
    StandingLossMeter slow(silvia());
    idle(fast, 300.0f, 85.0f, 0.04f, 0.25f);
    idle(slow, 300.0f, 85.0f, 0.04f, 2.0f);
    fast.finish();
    slow.finish();

    TEST_ASSERT_FLOAT_WITHIN(0.002f, fast.lossWPerK(), slow.lossWPerK());
}

// Hotter boiler, same loss coefficient: the duty has to rise in proportion to the gap above the room. Getting this
// backwards would make the measurement depend on the setpoint, which is the thing it must not do.
static void test_the_coefficient_is_independent_of_the_temperature() {
    StandingLossMeter cool(silvia());
    StandingLossMeter hot(silvia());
    idle(cool, 300.0f, 61.0f, 0.5625f * 40.0f / 900.0f);
    idle(hot, 300.0f, 101.0f, 0.5625f * 80.0f / 900.0f);
    cool.finish();
    hot.finish();

    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.5625f, cool.lossWPerK());
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.5625f, hot.lossWPerK());
}

// Time-weighted, not per-sample: a capture whose intervals differ must not let the closely-spaced samples drag the
// reported temperature towards themselves.
static void test_the_mean_temperature_is_time_weighted() {
    StandingLossMeter meter(silvia());
    for (int i = 0; i < 200; i++) {
        meter.sample(0.1f, 80.0f, 0.0f, 0.04f); // 20 s at 80 C, closely sampled
    }
    for (int i = 0; i < 20; i++) {
        meter.sample(4.0f, 90.0f, 0.0f, 0.04f); // 80 s at 90 C, sparsely sampled
    }
    meter.finish();

    TEST_ASSERT_FLOAT_WITHIN(0.1f, 88.0f, meter.meanTempC()); // 0.2 * 80 + 0.8 * 90
}

// Two separate idle stretches pool into one answer rather than the last one winning, so a long capture with a flush
// in the middle of it still uses all its idle time.
static void test_separate_windows_pool() {
    StandingLossMeter meter(silvia());
    idle(meter, 60.0f, 85.0f, 0.04f);
    meter.sample(0.5f, 85.0f, 8.0f, 1.0f);
    idle(meter, 240.0f, 85.0f, 0.04f);
    meter.finish();

    TEST_ASSERT_EQUAL_INT(2, meter.windows());
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 300.0f, meter.idleS());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5625f, meter.lossWPerK());
}

// Nothing in a capture is guaranteed: a row can be missing a field, an interval can be zero or negative across a
// clock change. Garbage must be dropped rather than folded in, since one NaN would take the whole integral with it.
static void test_nonsense_samples_are_dropped() {
    StandingLossMeter meter(silvia());
    idle(meter, 300.0f, 85.0f, 0.04f);
    meter.sample(0.0f, 85.0f, 0.0f, 0.04f);
    meter.sample(-1.0f, 85.0f, 0.0f, 0.04f);
    meter.sample(0.5f, NAN, 0.0f, 0.04f);
    meter.sample(0.5f, 85.0f, 0.0f, NAN);
    meter.finish();

    TEST_ASSERT_TRUE(meter.identified());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5625f, meter.lossWPerK());
}

// A duty outside 0..100 % is a reporting glitch, not a boiler that returned energy to the wall.
static void test_duty_is_clamped() {
    StandingLossMeter low(silvia());
    StandingLossMeter high(silvia());
    idle(low, 300.0f, 85.0f, -0.5f);
    idle(high, 300.0f, 85.0f, 1.5f);
    low.finish();
    high.finish();

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, low.lossWPerK());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 900.0f / 64.0f, high.lossWPerK());
}

// Nothing fed in at all, which is what a capture of a machine that was brewing throughout amounts to.
static void test_no_idle_time_identifies_nothing() {
    StandingLossMeter meter(silvia());
    for (int i = 0; i < 200; i++) {
        meter.sample(0.5f, 85.0f, 8.0f, 1.0f);
    }
    meter.finish();

    TEST_ASSERT_FALSE(meter.identified());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, meter.lossWPerK());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, meter.driftBoundWPerK(3500.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, meter.meanTempC());
}

// The threshold is about the pump being off, not about it being slow: a dribble still carries heat out of the boiler
// at 4.18 J per ml per K, which at 85 C is 268 W per ml/s -- seven times the standing loss.
static void test_a_dribble_still_ends_the_window() {
    StandingLossMeter meter(silvia());
    idle(meter, 300.0f, 85.0f, 0.04f);
    meter.sample(0.5f, 85.0f, 0.2f, 0.04f);
    idle(meter, 20.0f, 85.0f, 0.04f);
    meter.finish();

    TEST_ASSERT_EQUAL_INT(1, meter.windows()); // the 20 s tail after the dribble is its own window, and too short
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 300.0f, meter.idleS());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_steady_idle_gives_the_exact_ratio);
    RUN_TEST(test_a_flat_trace_has_no_drift_to_explain);
    RUN_TEST(test_drift_is_bounded_not_ignored);
    RUN_TEST(test_flow_ends_the_window);
    RUN_TEST(test_a_short_window_is_discarded);
    RUN_TEST(test_finish_is_what_counts_the_last_window);
    RUN_TEST(test_a_cycling_element_averages_to_the_same_answer);
    RUN_TEST(test_the_answer_does_not_depend_on_the_sample_rate);
    RUN_TEST(test_the_coefficient_is_independent_of_the_temperature);
    RUN_TEST(test_the_mean_temperature_is_time_weighted);
    RUN_TEST(test_separate_windows_pool);
    RUN_TEST(test_nonsense_samples_are_dropped);
    RUN_TEST(test_duty_is_clamped);
    RUN_TEST(test_no_idle_time_identifies_nothing);
    RUN_TEST(test_a_dribble_still_ends_the_window);
    return UNITY_END();
}
