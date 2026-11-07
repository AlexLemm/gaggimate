#include <display/util/BoilerObserver.h>
#include <display/util/PowerEstimate.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

// The user's Silvia, both figures measured at the plug.
static PowerEstimateParams silvia() {
    PowerEstimateParams p;
    p.heaterWatts = 900.0f;
    p.pumpWatts = 59.0f;
    p.standbyWatts = 4.0f;
    p.valveWatts = 8.0f;
    return p;
}

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

// Idle with the element off: only the standing electronics. Not zero, which is the point of having a standby term --
// a machine plugged in and switched on is drawing something, and reporting 0 W would be plainly false.
void test_idle_draws_standby_only() {
    PowerEstimateInputs in;
    const auto out = estimatePower(in, silvia());
    TEST_ASSERT_EQUAL_FLOAT(4.0f, out.totalW);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.heaterW);
}

// Everything flat out: 900 + 59 + 4 standby + 8 valve. The full-load figure a plug meter would show, and the check
// that the element really is 94 % of it.
void test_full_load() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 100.0f;
    in.pumpPowerPercent = 100.0f;
    in.valveOpen = true;
    const auto out = estimatePower(in, silvia());
    TEST_ASSERT_EQUAL_FLOAT(971.0f, out.totalW);
    TEST_ASSERT_TRUE(out.heaterW / out.totalW > 0.92f);
}

// The case that prompted the user's question about the 5 °C offset: 60 °C boiler, 86 °C setpoint, PID output 29 %.
// 29 % of 900 W is 261 W... which is not the 600 W they measured, because the plug meter was averaging over a window
// in which the duty was higher. Worth keeping as a test of the arithmetic, not of the meter.
void test_partial_duty_is_linear_in_the_element() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 29.0f;
    const auto out = estimatePower(in, silvia());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 261.0f, out.heaterW);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 265.0f, out.totalW);
}

// The pump at half drive, linear: the default, and an acknowledged approximation. 29.5 W of a 59 W pump.
void test_pump_is_linear_by_default() {
    PowerEstimateInputs in;
    in.pumpPowerPercent = 50.0f;
    const auto out = estimatePower(in, silvia());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 29.5f, out.pumpW);
}

// pumpCurve bends it the way phase-angle control actually behaves. At 2.0 the fraction is squared, so half drive is a
// quarter of the power: 14.75 W rather than 29.5. It exists so a measured curve can be applied without touching
// anything else.
void test_pump_curve_bends_the_estimate() {
    auto params = silvia();
    params.pumpCurve = 2.0f;
    PowerEstimateInputs in;
    in.pumpPowerPercent = 50.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 14.75f, estimatePower(in, params).pumpW);

    // And an exponent between the two blends them rather than calling pow() on an ESP32 every frame.
    params.pumpCurve = 1.5f;
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 22.125f, estimatePower(in, params).pumpW);
}

// Duties outside 0-100 are clamped rather than trusted, so a bad telemetry frame cannot report more than the machine
// can physically draw or a negative total.
void test_duties_are_clamped() {
    PowerEstimateInputs high;
    high.heaterDutyPercent = 500.0f;
    high.pumpPowerPercent = 400.0f;
    TEST_ASSERT_EQUAL_FLOAT(963.0f, estimatePower(high, silvia()).totalW); // 900 + 59 + 4, valve shut

    PowerEstimateInputs low;
    low.heaterDutyPercent = -50.0f;
    low.pumpPowerPercent = -10.0f;
    TEST_ASSERT_EQUAL_FLOAT(4.0f, estimatePower(low, silvia()).totalW);
}

// The alt relay is a grinder or a steam boiler depending on configuration, so its rating comes with the input rather
// than the params -- the caller knows which it is.
void test_alt_relay_is_counted() {
    PowerEstimateInputs in;
    in.altRelayOn = true;
    in.altRelayWatts = 250.0f;
    TEST_ASSERT_EQUAL_FLOAT(254.0f, estimatePower(in, silvia()).totalW);
}

// ---------------------------------------------------------------------------
// Energy
// ---------------------------------------------------------------------------

// A full-power minute is 900 W x 60 s = 54 kJ = 15 Wh.
void test_accumulates_energy() {
    PowerAccumulator acc;
    acc.accumulate(1.0f, 900.0f); // first interval ramps from 0 under the trapezoid
    for (int i = 0; i < 59; i++) {
        acc.accumulate(1.0f, 900.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(500.0f, 54000.0f - 450.0f, acc.energyJ());
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 14.875f, acc.energyWh());
}

// Trapezoidal rather than holding either endpoint, because the duty ramps between frames -- the pump coming up to
// pressure, the element easing off. Over one step from 0 to 900 W the honest figure is the average, 450 J, not 900.
void test_energy_uses_the_interval_average() {
    PowerAccumulator acc;
    acc.accumulate(1.0f, 900.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 450.0f, acc.energyJ());
}

void test_energy_ignores_bad_intervals() {
    PowerAccumulator acc;
    acc.accumulate(0.0f, 900.0f);
    acc.accumulate(-1.0f, 900.0f);
    acc.accumulate(1.0f, -50.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, acc.energyJ());
}

void test_energy_resets() {
    PowerAccumulator acc;
    acc.accumulate(10.0f, 900.0f);
    TEST_ASSERT_TRUE(acc.energyJ() > 0.0f);
    acc.reset();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, acc.energyJ());
}

// ---------------------------------------------------------------------------
// Live observer
// ---------------------------------------------------------------------------

static BoilerModelParams boiler() {
    BoilerModelParams p;
    p.capacityJPerK = 3300.0f;
    p.lossWPerK = 1.4f;
    p.sensorTauS = 5.0f;
    p.heaterWatts = 900.0f;
    p.ambientC = 21.0f;
    p.inletC = 21.0f;
    return p;
}

// The first sample has nothing to predict from, so it seeds and reports no error.
void test_observer_seeds_on_the_first_sample() {
    BoilerObserver obs;
    obs.sample(0.1f, 98.0f, 1.0f, 0.0f, boiler());
    TEST_ASSERT_TRUE(obs.isRunning());
    TEST_ASSERT_EQUAL_FLOAT(98.0f, obs.predictedC());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, obs.errorC());
}

// Fed its own model's behaviour, the observer tracks with no error: this is the check that it runs the same physics,
// not a check of the physics itself.
void test_observer_tracks_a_matching_machine() {
    const auto params = boiler();
    BoilerObserver obs;
    BoilerModelState truth;
    truth.boilerC = 98.0f;
    truth.sensorC = 98.0f;

    obs.sample(0.1f, truth.sensorC, 1.0f, 6.0f, params);
    for (int i = 0; i < 150; i++) {
        boilerModelStep(truth, params, 0.1f, 1.0f, 6.0f);
        obs.sample(0.1f, truth.sensorC, 1.0f, 6.0f, params);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, obs.errorC());
    TEST_ASSERT_EQUAL_UINT32(1, obs.reseedCount()); // only the initial seed
}

// Open loop, so a wrong heat capacity shows up as a growing gap rather than being absorbed. That gap is the whole
// point of putting the model on a graph beside the measurement.
void test_observer_diverges_when_the_model_is_wrong() {
    const auto truth = boiler();
    auto wrong = truth;
    wrong.capacityJPerK = 6600.0f; // twice the real inertia: the model will sag half as fast

    BoilerObserver obs;
    BoilerModelState real;
    real.boilerC = 98.0f;
    real.sensorC = 98.0f;
    obs.sample(0.1f, real.sensorC, 1.0f, 6.0f, wrong);
    for (int i = 0; i < 30; i++) {
        boilerModelStep(real, truth, 0.1f, 1.0f, 6.0f);
        obs.sample(0.1f, real.sensorC, 1.0f, 6.0f, wrong);
    }
    TEST_ASSERT_TRUE(obs.errorC() > 0.1f); // the model is running hot, as a too-heavy boiler would
}

// Drift has to be bounded or the trace leaves the axis and stops being informative, so the observer restarts itself
// once it is 5 °C out. Frequent re-seeding is itself the signal that the parameters do not fit.
void test_observer_reseeds_when_it_loses_the_plot() {
    const auto params = boiler();
    BoilerObserver obs;
    obs.sample(0.1f, 98.0f, 1.0f, 0.0f, params);
    const unsigned long initial = obs.reseedCount();
    // Claim the sensor jumped 20 °C, which no real boiler does: the model cannot follow and must restart.
    obs.sample(0.1f, 78.0f, 1.0f, 0.0f, params);
    TEST_ASSERT_TRUE(obs.reseedCount() > initial);
    TEST_ASSERT_EQUAL_FLOAT(78.0f, obs.predictedC());
}

// A gap longer than the cap means the loop was blocked and the held telemetry describes nothing, so the model is
// restarted rather than propagated across a stretch it has no inputs for.
void test_observer_reseeds_after_a_long_gap() {
    const auto params = boiler();
    BoilerObserver obs;
    obs.sample(0.1f, 98.0f, 1.0f, 0.0f, params);
    const unsigned long initial = obs.reseedCount();
    obs.sample(30.0f, 96.0f, 1.0f, 0.0f, params);
    TEST_ASSERT_TRUE(obs.reseedCount() > initial);
    TEST_ASSERT_EQUAL_FLOAT(96.0f, obs.predictedC());
}

// Without a heat capacity or an element rating there is nothing to model with, and the observer must stop rather than
// publish a trace that means nothing. getModelTemp() reports 0 for a stopped observer, which the chart renders as a
// gap.
void test_observer_stops_without_parameters() {
    auto params = boiler();
    params.capacityJPerK = 0.0f;
    BoilerObserver obs;
    obs.sample(0.1f, 98.0f, 1.0f, 0.0f, params);
    TEST_ASSERT_FALSE(obs.isRunning());
}

// The water runs ahead of the sensor during a shot, which is the quantity no machine can measure and the reason the
// PID brakes too late.
void test_observer_water_leads_the_sensor() {
    const auto params = boiler();
    BoilerObserver obs;
    obs.sample(0.1f, 98.0f, 1.0f, 6.0f, params);
    for (int i = 0; i < 50; i++) {
        obs.sample(0.1f, obs.predictedC(), 1.0f, 6.0f, params);
    }
    TEST_ASSERT_TRUE(obs.predictedBoilerC() < obs.predictedC());
    TEST_ASSERT_TRUE(obs.netPowerW() < 0.0f); // being drained faster than the element can refill
}

// ---------------------------------------------------------------------------
// A duty is not evidence that the element is on
// ---------------------------------------------------------------------------

// The bug this rule exists for. The controller board runs the PID and reports its duty over BLE; when the display
// stops commanding a setpoint the controller cuts the relay, but the duty it last computed stays in the field it
// reports from, because the PID returns early in manual mode without writing a new one. A dashboard that believed it
// showed a 900 W element on a machine standing in standby, and the energy total climbed the whole time.
void test_a_duty_reported_for_a_commanded_off_element_is_not_charged() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 100.0f; // what the controller is still reporting
    in.heaterEnabled = false;      // what the firmware is actually commanding
    const auto out = estimatePower(in, silvia());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.heaterW);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, out.totalW); // standby alone
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out.heaterDutyPercent);
}

// And it says so rather than quietly presenting the corrected figure: the disagreement is a fault in the link, worth
// surfacing, not a reading to smooth over.
void test_an_ignored_duty_is_reported_as_ignored() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 37.0f;
    in.heaterEnabled = false;
    TEST_ASSERT_TRUE(estimatePower(in, silvia()).heaterReportIgnored);
}

// Commanded off and reporting zero is the ordinary case -- an element that has been off long enough for something to
// overwrite the stale figure, or firmware that zeroes it. Nothing to flag.
void test_a_commanded_off_element_reporting_zero_is_not_a_fault() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 0.0f;
    in.heaterEnabled = false;
    TEST_ASSERT_FALSE(estimatePower(in, silvia()).heaterReportIgnored);
}

// With a setpoint commanded the reported duty is believed at face value, which is the whole point: the controller is
// the only thing that knows what the PID did inside the last window.
void test_an_enabled_element_is_charged_what_it_reports() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 40.0f;
    in.heaterEnabled = true;
    const auto out = estimatePower(in, silvia());
    TEST_ASSERT_EQUAL_FLOAT(360.0f, out.heaterW);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, out.heaterDutyPercent);
    TEST_ASSERT_FALSE(out.heaterReportIgnored);
}

// The charged duty is the clamped one, so it can be printed beside the watts without the two disagreeing.
void test_the_charged_duty_is_the_clamped_duty() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 140.0f;
    const auto out = estimatePower(in, silvia());
    TEST_ASSERT_EQUAL_FLOAT(100.0f, out.heaterDutyPercent);
    TEST_ASSERT_EQUAL_FLOAT(900.0f, out.heaterW);
}

// Defaults stay as they were: a caller that knows nothing about the command believes the duty. Keeping the old
// behaviour the default means this rule cannot silently zero an estimate somewhere that was never updated.
void test_the_default_believes_the_reported_duty() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 50.0f;
    TEST_ASSERT_EQUAL_FLOAT(450.0f, estimatePower(in, silvia()).heaterW);
}

// The rule is about the element only. The pump's drive is commanded by this same firmware, so there is no second
// party to disagree with and no equivalent gate.
void test_the_rule_leaves_the_pump_alone() {
    PowerEstimateInputs in;
    in.heaterDutyPercent = 100.0f;
    in.heaterEnabled = false;
    in.pumpPowerPercent = 100.0f;
    TEST_ASSERT_EQUAL_FLOAT(59.0f, estimatePower(in, silvia()).pumpW);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_draws_standby_only);
    RUN_TEST(test_full_load);
    RUN_TEST(test_partial_duty_is_linear_in_the_element);
    RUN_TEST(test_pump_is_linear_by_default);
    RUN_TEST(test_pump_curve_bends_the_estimate);
    RUN_TEST(test_duties_are_clamped);
    RUN_TEST(test_alt_relay_is_counted);
    RUN_TEST(test_a_duty_reported_for_a_commanded_off_element_is_not_charged);
    RUN_TEST(test_an_ignored_duty_is_reported_as_ignored);
    RUN_TEST(test_a_commanded_off_element_reporting_zero_is_not_a_fault);
    RUN_TEST(test_an_enabled_element_is_charged_what_it_reports);
    RUN_TEST(test_the_charged_duty_is_the_clamped_duty);
    RUN_TEST(test_the_default_believes_the_reported_duty);
    RUN_TEST(test_the_rule_leaves_the_pump_alone);
    RUN_TEST(test_accumulates_energy);
    RUN_TEST(test_energy_uses_the_interval_average);
    RUN_TEST(test_energy_ignores_bad_intervals);
    RUN_TEST(test_energy_resets);
    RUN_TEST(test_observer_seeds_on_the_first_sample);
    RUN_TEST(test_observer_tracks_a_matching_machine);
    RUN_TEST(test_observer_diverges_when_the_model_is_wrong);
    RUN_TEST(test_observer_reseeds_when_it_loses_the_plot);
    RUN_TEST(test_observer_reseeds_after_a_long_gap);
    RUN_TEST(test_observer_stops_without_parameters);
    RUN_TEST(test_observer_water_leads_the_sensor);
    return UNITY_END();
}
