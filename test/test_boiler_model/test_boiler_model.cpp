#include <display/util/BoilerIdentifier.h>
#include <display/util/BoilerModel.h>
#include <unity.h>
#include <vector>

void setUp() {}
void tearDown() {}

// The user's machine as measured: 900 W on a plug meter, 21 °C tank and room, and the ~3.3 kJ/K effective capacity its
// own shot logs imply. Sensor lag and standing loss are guesses, which is the point of the identifier.
static BoilerModelParams silvia() {
    BoilerModelParams p;
    p.capacityJPerK = 3300.0f;
    p.lossWPerK = 1.4f;
    p.sensorTauS = 5.0f;
    p.heaterWatts = 900.0f;
    p.ambientC = 21.0f;
    p.inletC = 21.0f;
    return p;
}

// ---------------------------------------------------------------------------
// Forward model
// ---------------------------------------------------------------------------

// At a 98 °C boiler and no flow the element only has the standing loss to cover: 1.4 * 77 = 108 W, 12 % of 900 W.
void test_idle_duty_is_standing_loss_only() {
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.12f, boilerModelRequiredDuty(silvia(), 98.0f, 0.0f));
}

// Every ml/s needs 4.18 * 77 = 322 W, so 2 ml/s wants 644 W of water heating on top of the 108 W standing loss: 84 %.
// This is the arithmetic the disturbance feedforward in Heater.cpp performs.
void test_flow_dominates_the_duty() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.836f, boilerModelRequiredDuty(silvia(), 98.0f, 2.0f));
}

// Where the element runs out: (900 - 108) / (4.18 * 77) = 2.46 ml/s. Below that a shot can be held indefinitely;
// above it the shortfall comes out of stored heat, which is what ThermalBank.h and ThermalRecovery.h are for.
void test_max_sustainable_flow() {
    const auto p = silvia();
    const float limit = boilerModelMaxSustainableFlow(p, 98.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 2.46f, limit);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, boilerModelRequiredDuty(p, 98.0f, limit));
}

// A cooler setpoint needs less per ml, so the element reaches further. Worth stating because it is the reason a 93 °C
// profile sags less than a 98 °C one at identical flow.
void test_cooler_setpoint_sustains_more_flow() {
    const auto p = silvia();
    TEST_ASSERT_TRUE(boilerModelMaxSustainableFlow(p, 88.0f) > boilerModelMaxSustainableFlow(p, 98.0f));
}

// Below the inlet temperature there is nothing for the water to take out, so no flow limit applies.
void test_no_limit_below_inlet() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, boilerModelMaxSustainableFlow(silvia(), 20.0f));
}

// The step returns net power, which makes the energy balance checkable from outside: at the sustainable flow the net
// is zero and the temperature holds; at twice that flow it falls at net/C.
void test_step_energy_balance() {
    const auto p = silvia();
    BoilerModelState held;
    held.boilerC = 98.0f;
    held.sensorC = 98.0f;
    const float net = boilerModelStep(held, p, 0.1f, 1.0f, boilerModelMaxSustainableFlow(p, 98.0f));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, net);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 98.0f, held.boilerC);

    BoilerModelState sagging;
    sagging.boilerC = 98.0f;
    sagging.sensorC = 98.0f;
    const float deficit = boilerModelStep(sagging, p, 1.0f, 1.0f, 9.85f);
    // 9.85 ml/s needs 3 171 W of water heating plus 108 W of loss against 900 W: a 2 379 W shortfall.
    TEST_ASSERT_FLOAT_WITHIN(30.0f, -2379.0f, deficit);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 98.0f + deficit / p.capacityJPerK, sagging.boilerC);
}

// The sensor lags the boiler, which is the whole reason it gets its own state: one time constant in, it has covered
// 63 % of the step and no more.
void test_sensor_lags_the_boiler() {
    auto p = silvia();
    p.heaterWatts = 0.0f; // isolate the lag from the heating
    BoilerModelState state;
    state.boilerC = 98.0f;
    state.sensorC = 88.0f;
    p.capacityJPerK = 1e9f; // hold the boiler still
    for (int i = 0; i < 50; i++) {
        boilerModelStep(state, p, 0.1f, 0.0f, 0.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 88.0f + 10.0f * 0.632f, state.sensorC);
}

// ---------------------------------------------------------------------------
// Identification
// ---------------------------------------------------------------------------

// Generates what a shot log would contain for a known machine: the sensor trace, the flow, and whether the element is
// saturated. Deliberately returns only what a real .slog carries, so the test cannot cheat by handing the filter
// something the tool will not have.
static std::vector<BoilerLogSample> syntheticShot(const BoilerModelParams &truth, float startC,
                                                  const std::vector<std::pair<float, float>> &phases, float dtS) {
    std::vector<BoilerLogSample> log;
    BoilerModelState state;
    state.boilerC = startC;
    state.sensorC = startC;

    for (const auto &phase : phases) {
        const int steps = static_cast<int>(phase.first / dtS + 0.5f);
        for (int i = 0; i < steps; i++) {
            const float flow = phase.second;
            // The real machine's duty: saturated whenever the demand exceeds the element, otherwise whatever holds it.
            const float required = boilerModelRequiredDuty(truth, state.boilerC, flow);
            const float duty = required > 1.0f ? 1.0f : required;
            boilerModelStep(state, truth, dtS, duty, flow);

            BoilerLogSample s;
            s.dtS = dtS;
            s.measuredC = state.sensorC;
            s.flowMlPerS = flow;
            // What the tool can know: saturated means the duty is 1.0, and nothing else is trustworthy.
            s.dutyKnown = BoilerIdentifier::elementSaturated(truth, state.sensorC, flow, 1.2f);
            s.dutyFraction = s.dutyKnown ? 1.0f : 0.0f;
            log.push_back(s);
        }
    }
    return log;
}

// Sweeps the log `passes` times, as the offline tool does. One pass already lands close; the rest polish, and are
// what makes the answer independent of the starting guess.
static BoilerIdentifierResult identify(const std::vector<BoilerLogSample> &log, const BoilerModelParams &prior,
                                       int passes = 6) {
    BoilerIdentifierConfig cfg;
    cfg.prior = prior;
    BoilerIdentifier filter(cfg);
    for (int pass = 0; pass < passes; pass++) {
        filter.beginPass();
        for (const auto &s : log) {
            filter.observe(s.dtS, s.measuredC, s.flowMlPerS, s.dutyFraction, s.dutyKnown);
        }
    }
    return filter.result();
}

// The test that matters: started from a heat capacity 50 % too high, does the filter find the real one from nothing
// but the sensor trace and the flow? 20 s at 6 ml/s is well past the 2.46 ml/s the element can hold.
void test_recovers_heat_capacity_from_a_wrong_prior() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{20.0f, 6.0f}}, 0.1f);

    auto prior = truth;
    prior.capacityJPerK = 5000.0f;
    prior.sensorTauS = 4.0f;
    prior.lossWPerK = 1.0f;

    const auto fit = identify(log, prior);
    TEST_ASSERT_TRUE(fit.samplesLearned > 150);
    // Within 10 % of the truth, from 52 % out.
    TEST_ASSERT_FLOAT_WITHIN(330.0f, truth.capacityJPerK, fit.params.capacityJPerK);
    // And it has to be closer than where it started, not merely inside the tolerance by luck.
    const float startError = 5000.0f - truth.capacityJPerK;
    const float endError = fit.params.capacityJPerK - truth.capacityJPerK;
    TEST_ASSERT_TRUE((endError < 0.0f ? -endError : endError) < startError * 0.4f);
}

// Approaching from below, so the result is not an artefact of one direction.
void test_recovers_heat_capacity_from_a_low_prior() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{20.0f, 6.0f}}, 0.1f);

    auto prior = truth;
    prior.capacityJPerK = 2000.0f;
    const auto fit = identify(log, prior);
    TEST_ASSERT_FLOAT_WITHIN(400.0f, truth.capacityJPerK, fit.params.capacityJPerK);
}

// Learning tightens the heat capacity's confidence interval well below the prior, which is the filter reporting that
// the data did pin this parameter down.
void test_capacity_sigma_shrinks() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{20.0f, 6.0f}}, 0.1f);
    auto prior = truth;
    prior.capacityJPerK = 5000.0f;

    const auto fit = identify(log, prior);
    TEST_ASSERT_TRUE(fit.capacitySigma < 1500.0f * 0.5f);
}

// The honest half: a shot says almost nothing about standing loss, because 108 W of it hides inside 900 W of element
// over 20 s. Both standing loss and heat capacity slow the sag, so left free the loss absorbs the capacity error, and
// the default prior is therefore tight enough to hold it. This test states that consequence rather than pretending
// otherwise: handed a loss 29 % below the truth, the fit leaves it there and does not claim to have learned it.
// Identifying UA needs a log that can see it -- an idle stretch or a warm-up, where it is the only term left.
void test_loss_is_held_not_learned_from_a_shot() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{20.0f, 6.0f}}, 0.1f);
    auto prior = truth;
    prior.lossWPerK = 1.0f; // truth is 1.4

    const auto fit = identify(log, prior);
    TEST_ASSERT_FLOAT_WITHIN(0.15f, 1.0f, fit.params.lossWPerK); // still essentially the prior
    TEST_ASSERT_TRUE(fit.lossSigma > 0.08f);                     // and its interval has barely closed
}

// The cost of holding a wrong loss: it has to be paid somewhere, and it is paid in the heat capacity. 0.4 W/K of error
// is ~31 W against the ~1 139 W the sag is driven by, so the capacity comes back a few per cent off rather than wildly
// wrong. Worth pinning down, because it bounds how much a bad ambient or loss guess can cost the answer.
void test_a_wrong_loss_biases_capacity_only_slightly() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{20.0f, 6.0f}}, 0.1f);

    auto good = truth;
    good.capacityJPerK = 5000.0f;
    auto bad = good;
    bad.lossWPerK = 1.0f;

    const float withTruth = identify(log, good).params.capacityJPerK;
    const float withWrongLoss = identify(log, bad).params.capacityJPerK;
    const float bias = withWrongLoss - withTruth;
    TEST_ASSERT_TRUE((bias < 0.0f ? -bias : bias) < truth.capacityJPerK * 0.06f);
}

// A profile-shaped shot: a fast fill, a bloom with the pump off, then extraction under the element's limit. Only the
// fill is saturated, so only the fill teaches the filter anything -- and that is enough.
void test_learns_from_the_fill_phase_alone() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{6.0f, 9.85f}, {8.0f, 0.0f}, {25.0f, 1.3f}}, 0.1f);

    size_t saturated = 0;
    for (const auto &s : log) {
        if (s.dutyKnown) {
            saturated++;
        }
    }
    TEST_ASSERT_TRUE(saturated > 50);
    TEST_ASSERT_TRUE(saturated < log.size() / 2); // most of the shot teaches nothing, as expected

    auto prior = truth;
    prior.capacityJPerK = 5000.0f;
    const auto fit = identify(log, prior);
    // One short of the saturated count: the first sample of a window supplies the starting temperature rather than
    // teaching anything, since there is no prior state to have predicted it from.
    TEST_ASSERT_EQUAL_UINT32(saturated - 1, fit.samplesLearned);
    TEST_ASSERT_TRUE(fit.params.capacityJPerK < 4500.0f); // moved towards the truth on 6 s of fill
}

// A log with no saturated stretch at all teaches nothing, and the filter must return the prior unchanged rather than
// drifting. A 1.3 ml/s shot never saturates a 900 W element.
void test_unsaturated_log_learns_nothing() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{30.0f, 1.3f}}, 0.1f);

    auto prior = truth;
    prior.capacityJPerK = 5000.0f;
    const auto fit = identify(log, prior);
    TEST_ASSERT_EQUAL_UINT32(0, fit.samplesLearned);
    TEST_ASSERT_TRUE(fit.samplesSeen > 250);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 5000.0f, fit.params.capacityJPerK);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 1500.0f, fit.capacitySigma);
}

// Parameters are held inside physical bounds whatever the data does, so a corrupt log degrades to a clamped answer
// with a wide sigma rather than to nonsense a caller might act on.
void test_parameters_stay_physical() {
    BoilerIdentifierConfig cfg;
    cfg.prior = silvia();
    BoilerIdentifier filter(cfg);
    // Sensor readings that no machine could produce: 40 °C swings at every sample.
    for (int i = 0; i < 400; i++) {
        filter.observe(0.1f, i % 2 == 0 ? 60.0f : 100.0f, 9.85f, 1.0f, true);
    }
    const auto fit = filter.result();
    TEST_ASSERT_TRUE(fit.params.capacityJPerK >= 500.0f && fit.params.capacityJPerK <= 20000.0f);
    TEST_ASSERT_TRUE(fit.params.lossWPerK >= 0.05f && fit.params.lossWPerK <= 20.0f);
    TEST_ASSERT_TRUE(fit.params.sensorTauS >= 0.2f && fit.params.sensorTauS <= 60.0f);
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

// Replaying the true parameters open-loop over their own log reproduces it, which is the sanity check on the replay
// itself: any error here is the replay's, not the fit's.
void test_replay_of_truth_is_exact() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{20.0f, 6.0f}}, 0.1f);
    const auto replay = boilerReplay(log.data(), log.size(), truth);
    TEST_ASSERT_TRUE(replay.samples > 150);
    TEST_ASSERT_TRUE(replay.rmsErrorC < 0.05f);
}

// The measure that proves a fit: predicting the sag without ever being corrected from it. Wrong parameters must score
// clearly worse than the fitted ones, or the number is not telling the user anything.
void test_replay_separates_a_good_fit_from_a_bad_one() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{20.0f, 6.0f}}, 0.1f);

    auto prior = truth;
    prior.capacityJPerK = 5000.0f;
    const auto fit = identify(log, prior);

    const auto fitted = boilerReplay(log.data(), log.size(), fit.params);
    const auto wrong = boilerReplay(log.data(), log.size(), prior);
    TEST_ASSERT_TRUE(fitted.rmsErrorC < wrong.rmsErrorC);
}

// Replay skips the stretches where the duty is unknown rather than predicting through them, so a shot that is mostly
// unsaturated scores only on the part the model was given the element's state for.
void test_replay_only_scores_known_duty() {
    const auto truth = silvia();
    const auto log = syntheticShot(truth, 98.0f, {{6.0f, 9.85f}, {8.0f, 0.0f}, {25.0f, 1.3f}}, 0.1f);
    const auto replay = boilerReplay(log.data(), log.size(), truth);
    TEST_ASSERT_TRUE(replay.samples > 0);
    TEST_ASSERT_TRUE(replay.samples < log.size() / 2);
}

void test_replay_handles_empty_input() {
    const auto replay = boilerReplay(nullptr, 0, silvia());
    TEST_ASSERT_EQUAL_UINT32(0, replay.samples);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, replay.rmsErrorC);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_duty_is_standing_loss_only);
    RUN_TEST(test_flow_dominates_the_duty);
    RUN_TEST(test_max_sustainable_flow);
    RUN_TEST(test_cooler_setpoint_sustains_more_flow);
    RUN_TEST(test_no_limit_below_inlet);
    RUN_TEST(test_step_energy_balance);
    RUN_TEST(test_sensor_lags_the_boiler);
    RUN_TEST(test_recovers_heat_capacity_from_a_wrong_prior);
    RUN_TEST(test_recovers_heat_capacity_from_a_low_prior);
    RUN_TEST(test_capacity_sigma_shrinks);
    RUN_TEST(test_loss_is_held_not_learned_from_a_shot);
    RUN_TEST(test_a_wrong_loss_biases_capacity_only_slightly);
    RUN_TEST(test_learns_from_the_fill_phase_alone);
    RUN_TEST(test_unsaturated_log_learns_nothing);
    RUN_TEST(test_parameters_stay_physical);
    RUN_TEST(test_replay_of_truth_is_exact);
    RUN_TEST(test_replay_separates_a_good_fit_from_a_bad_one);
    RUN_TEST(test_replay_only_scores_known_duty);
    RUN_TEST(test_replay_handles_empty_input);
    return UNITY_END();
}
