#include <display/util/BoilerModel.h>
#include <display/util/BoilerTwoMass.h>
#include <unity.h>
#include <vector>

void setUp() {}
void tearDown() {}

// A 300 ml boiler split in half, each cell holding its water plus a share of the brass, with convective mixing
// between them. Physical starting values rather than the fitted ones -- the tests are about the structure's
// behaviour, which has to hold for any sensible parameters, not about one machine's numbers.
static BoilerTwoMassParams silvia() {
    BoilerTwoMassParams p;
    p.inletCapacityJPerK = 1000.0f;
    p.outletCapacityJPerK = 1000.0f;
    p.mixingWPerK = 60.0f;
    p.lossWPerK = 1.2f;
    p.sensorTauS = 4.0f;
    p.heaterWatts = 900.0f;
    p.ambientC = 21.0f;
    p.inletC = 21.0f;
    return p;
}

// The fill in shot 55: 5.2 s at 8 ml/s with the element pinned.
static void runFill(BoilerTwoMassState &s, const BoilerTwoMassParams &p) {
    for (int i = 0; i < 52; i++) {
        boilerTwoMassStep(s, p, 0.1f, 1.0f, 8.0f);
    }
}

// ---------------------------------------------------------------------------
// The step
// ---------------------------------------------------------------------------

// Every term checked separately, because an energy balance that only balances in total can hide two compensating
// sign errors.
void test_step_energy_balance() {
    BoilerTwoMassParams p = silvia();
    BoilerTwoMassState s;
    s.inletCellC = 95.0f;
    s.outletCellC = 93.0f;
    s.sensorC = 93.0f;
    const auto power = boilerTwoMassStep(s, p, 0.1f, 1.0f, 2.0f);

    TEST_ASSERT_FLOAT_WITHIN(0.1f, 2.0f * 4.18f * (21.0f - 95.0f), power.coldInW); // flow * c * (inlet - Tb)
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f * 2.0f, power.mixedW);                   // Kx * (Tb - Tt)
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 2.0f * 4.18f * 2.0f, power.advectedW);          // flow * c * (Tb - Tt)
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.2f * 72.0f, power.lostW);                    // UA * (Tt - ambient)
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 900.0f - 618.6f - 120.0f, power.intoInletCellW);
}

void test_step_rejects_nonsense() {
    BoilerTwoMassParams p = silvia();
    BoilerTwoMassState s = boilerTwoMassSeedIdle(p, 93.0f);
    const float before = s.inletCellC;
    boilerTwoMassStep(s, p, 0.0f, 1.0f, 8.0f);
    TEST_ASSERT_EQUAL_FLOAT(before, s.inletCellC);
    p.inletCapacityJPerK = 0.0f;
    boilerTwoMassStep(s, p, 0.1f, 1.0f, 8.0f);
    TEST_ASSERT_EQUAL_FLOAT(before, s.inletCellC);
}

void test_duty_is_clamped() {
    BoilerTwoMassParams p = silvia();
    BoilerTwoMassState a = boilerTwoMassSeedIdle(p, 93.0f);
    BoilerTwoMassState b = a;
    const auto wild = boilerTwoMassStep(a, p, 0.1f, 4.0f, 0.0f);
    const auto full = boilerTwoMassStep(b, p, 0.1f, 1.0f, 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(full.intoInletCellW, wild.intoInletCellW);
}

// ---------------------------------------------------------------------------
// The behaviour that made this header necessary
// ---------------------------------------------------------------------------

// The cold goes to the bottom, so the bottom is what empties. The outlet -- the water the puck gets -- is barely
// touched by a draw this small, which is exactly what the machine's logs show and what one node cannot produce.
void test_the_cold_lands_at_the_bottom_not_the_outlet() {
    const BoilerTwoMassParams p = silvia();
    BoilerTwoMassState s = boilerTwoMassSeedIdle(p, 93.0f);
    const float bottomBefore = s.inletCellC;
    const float outletBefore = s.outletCellC;
    runFill(s, p);
    const float bottomFall = bottomBefore - s.inletCellC;
    const float outletFall = outletBefore - s.outletCellC;
    TEST_ASSERT_TRUE(bottomFall > 5.0f);
    TEST_ASSERT_TRUE(outletFall < bottomFall / 3.0f);
}

// Side by side with one node, at the SAME total heat capacity, so the comparison is about structure and nothing else.
// Give the one node 3500 J/K against two 1000 J/K cells and it is the larger mass that wins, which proves nothing.
// Matched at 2000 J/K, one node has nowhere to put the arriving cold but the volume it is measuring, while the
// stratified pair puts it at the bottom and the outlet is spared most of it.
void test_one_node_falls_faster_than_the_outlet_cell_at_equal_capacity() {
    const BoilerTwoMassParams two = silvia();
    BoilerModelParams one;
    one.capacityJPerK = two.inletCapacityJPerK + two.outletCapacityJPerK;
    one.lossWPerK = two.lossWPerK;
    one.sensorTauS = two.sensorTauS;
    one.heaterWatts = two.heaterWatts;
    one.ambientC = two.ambientC;
    one.inletC = two.inletC;

    BoilerModelState a;
    a.boilerC = 93.0f;
    a.sensorC = 93.0f;
    for (int i = 0; i < 52; i++) {
        boilerModelStep(a, one, 0.1f, 1.0f, 8.0f);
    }
    const float oneNodeFall = 93.0f - a.boilerC;

    BoilerTwoMassState b = boilerTwoMassSeedIdle(two, 93.0f);
    runFill(b, two);
    const float outletFall = 93.0f - b.outletCellC;

    TEST_ASSERT_TRUE(oneNodeFall > 2.0f);
    TEST_ASSERT_TRUE(outletFall < oneNodeFall);
}

// And how much the outlet is spared depends on the mixing, which is the parameter the whole structure turns on: the
// less the halves mix, the longer the cold stays where it arrived and the later the sensor hears about it. Monotone
// in mixing, so there is a direction for a fit to move in.
void test_less_mixing_spares_the_outlet_more() {
    BoilerTwoMassParams slow = silvia();
    slow.mixingWPerK = 20.0f;
    BoilerTwoMassParams fast = silvia();
    fast.mixingWPerK = 200.0f;

    BoilerTwoMassState a = boilerTwoMassSeedIdle(slow, 93.0f);
    BoilerTwoMassState b = boilerTwoMassSeedIdle(fast, 93.0f);
    const float startA = a.outletCellC;
    const float startB = b.outletCellC;
    runFill(a, slow);
    runFill(b, fast);
    TEST_ASSERT_TRUE(startA - a.outletCellC < startB - b.outletCellC);
}

// And the deficit arrives afterwards. With the flow stopped and the element off, the outlet keeps falling as the cold
// that is sitting at the bottom mixes upward. One node has no way to delay a loss; this is where the delay lives.
void test_the_outlet_keeps_falling_after_the_flow_stops() {
    const BoilerTwoMassParams p = silvia();
    BoilerTwoMassState s = boilerTwoMassSeedIdle(p, 93.0f);
    runFill(s, p);
    const float atFillEnd = s.outletCellC;
    for (int i = 0; i < 200; i++) { // 20 s, no flow, element off
        boilerTwoMassStep(s, p, 0.1f, 0.0f, 0.0f);
    }
    TEST_ASSERT_TRUE(s.outletCellC < atFillEnd - 1.0f);
}

// The sensor lags the water it is nearest, so the brew temperature and the reading are not the same number even at
// the outlet. During a fill the reading is the more optimistic of the two.
void test_the_sensor_lags_the_outlet() {
    const BoilerTwoMassParams p = silvia();
    BoilerTwoMassState s = boilerTwoMassSeedIdle(p, 93.0f);
    runFill(s, p);
    TEST_ASSERT_TRUE(s.sensorC > s.outletCellC);
}

// ---------------------------------------------------------------------------
// Steady state, seeding and plausibility
// ---------------------------------------------------------------------------

void test_idle_duty_is_small() {
    const float duty = boilerTwoMassRequiredDuty(silvia(), 93.0f, 0.0f);
    TEST_ASSERT_TRUE(duty > 0.0f && duty < 0.15f);
}

// Every ml/s needs 4.18 * 72 = 301 W, so 2 ml/s is two thirds of a 900 W element before the standing loss.
void test_flow_dominates_the_duty() {
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 0.76f, boilerTwoMassRequiredDuty(silvia(), 93.0f, 2.0f));
}

// The duty that holds a temperature should actually hold it.
void test_required_duty_really_holds() {
    const BoilerTwoMassParams p = silvia();
    const float duty = boilerTwoMassRequiredDuty(p, 93.0f, 1.0f);
    BoilerTwoMassState s;
    s.outletCellC = 93.0f;
    s.sensorC = 93.0f;
    s.inletCellC = 93.0f + p.lossWPerK * (93.0f - p.ambientC) / p.mixingWPerK;
    for (int i = 0; i < 600; i++) {
        boilerTwoMassStep(s, p, 0.1f, duty, 1.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.6f, 93.0f, s.outletCellC);
}

// Seeding has to put the bottom ABOVE the outlet, by the gradient mixing needs to carry the standing loss. Starting
// them equal gives the model a boiler with no stored gradient, and the replay then sags at the wrong moment.
void test_seeding_puts_the_bottom_above_the_outlet() {
    const BoilerTwoMassParams p = silvia();
    const BoilerTwoMassState s = boilerTwoMassSeedIdle(p, 93.0f);
    TEST_ASSERT_EQUAL_FLOAT(93.0f, s.sensorC);
    TEST_ASSERT_EQUAL_FLOAT(93.0f, s.outletCellC);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 93.0f + 1.2f * 72.0f / 60.0f, s.inletCellC);
}

void test_a_seeded_idle_machine_holds() {
    const BoilerTwoMassParams p = silvia();
    BoilerTwoMassState s = boilerTwoMassSeedIdle(p, 93.0f);
    const float duty = boilerTwoMassRequiredDuty(p, 93.0f, 0.0f);
    for (int i = 0; i < 600; i++) {
        boilerTwoMassStep(s, p, 0.1f, duty, 0.0f);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 93.0f, s.sensorC);
}

// The guard that stops a fit buying a flat sensor trace with an impossible boiler. At 1.2 W/K the standing loss is
// 86 W, so mixing below about 12 W/K would need the bottom above boiling to carry it while idling at 93 C.
void test_implausible_mixing_is_rejected() {
    BoilerTwoMassParams p = silvia();
    TEST_ASSERT_TRUE(boilerTwoMassPlausible(p, 93.0f));
    p.mixingWPerK = 5.0f;
    TEST_ASSERT_FALSE(boilerTwoMassPlausible(p, 93.0f));
    p.mixingWPerK = 0.0f;
    TEST_ASSERT_FALSE(boilerTwoMassPlausible(p, 93.0f));
}

// A cooler setpoint leaves room for less mixing, because there is less loss to carry and more headroom to carry it in.
void test_a_cooler_setpoint_tolerates_less_mixing() {
    BoilerTwoMassParams p = silvia();
    p.mixingWPerK = 8.0f;
    TEST_ASSERT_FALSE(boilerTwoMassPlausible(p, 93.0f));
    TEST_ASSERT_TRUE(boilerTwoMassPlausible(p, 60.0f));
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

static std::vector<BoilerTwoMassSample> syntheticShot(const BoilerTwoMassParams &truth) {
    std::vector<BoilerTwoMassSample> out;
    BoilerTwoMassState s = boilerTwoMassSeedIdle(truth, 93.0f);
    for (int i = 0; i < 300; i++) {
        BoilerTwoMassSample sample;
        sample.dtS = 0.25f;
        sample.flowMlPerS = i < 24 ? 8.0f : (i < 60 ? 0.0f : 1.3f);
        sample.dutyFraction = sample.flowMlPerS > 3.0f ? 1.0f : 0.3f;
        boilerTwoMassStep(s, truth, sample.dtS, sample.dutyFraction, sample.flowMlPerS);
        sample.measuredC = s.sensorC;
        out.push_back(sample);
    }
    return out;
}

// Replaying the truth against itself is exact, which is the only way to know the replay scores the model rather than
// an accident of the seeding.
void test_replay_of_truth_is_exact() {
    const BoilerTwoMassParams truth = silvia();
    const auto shot = syntheticShot(truth);
    const auto replay = boilerTwoMassReplay(shot.data(), shot.size(), truth);
    TEST_ASSERT_EQUAL_UINT32(shot.size(), replay.samples);
    TEST_ASSERT_TRUE(replay.rmsErrorC < 0.01f);
}

// Wrong parameters have to score worse, or a fit has nothing to descend.
void test_replay_separates_a_good_fit_from_a_bad_one() {
    const BoilerTwoMassParams truth = silvia();
    const auto shot = syntheticShot(truth);
    BoilerTwoMassParams wrong = truth;
    wrong.mixingWPerK = 600.0f;
    wrong.inletCapacityJPerK = 300.0f;
    const auto good = boilerTwoMassReplay(shot.data(), shot.size(), truth);
    const auto bad = boilerTwoMassReplay(shot.data(), shot.size(), wrong);
    TEST_ASSERT_TRUE(bad.rmsErrorC > good.rmsErrorC + 0.5f);
}

// The replay reports where the water went, since that is the output the sensor cannot give -- and the bottom has to
// come out colder than the outlet, or the stratification has the wrong sign.
void test_replay_reports_both_minima() {
    const BoilerTwoMassParams truth = silvia();
    const auto shot = syntheticShot(truth);
    const auto replay = boilerTwoMassReplay(shot.data(), shot.size(), truth);
    TEST_ASSERT_TRUE(replay.minOutletC < 92.0f);
    TEST_ASSERT_TRUE(replay.minInletCellC < replay.minOutletC);
}

void test_replay_handles_empty_input() {
    const auto replay = boilerTwoMassReplay(nullptr, 0, silvia());
    TEST_ASSERT_EQUAL_UINT32(0, replay.samples);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, replay.rmsErrorC);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_step_energy_balance);
    RUN_TEST(test_step_rejects_nonsense);
    RUN_TEST(test_duty_is_clamped);
    RUN_TEST(test_the_cold_lands_at_the_bottom_not_the_outlet);
    RUN_TEST(test_one_node_falls_faster_than_the_outlet_cell_at_equal_capacity);
    RUN_TEST(test_less_mixing_spares_the_outlet_more);
    RUN_TEST(test_the_outlet_keeps_falling_after_the_flow_stops);
    RUN_TEST(test_the_sensor_lags_the_outlet);
    RUN_TEST(test_idle_duty_is_small);
    RUN_TEST(test_flow_dominates_the_duty);
    RUN_TEST(test_required_duty_really_holds);
    RUN_TEST(test_seeding_puts_the_bottom_above_the_outlet);
    RUN_TEST(test_a_seeded_idle_machine_holds);
    RUN_TEST(test_implausible_mixing_is_rejected);
    RUN_TEST(test_a_cooler_setpoint_tolerates_less_mixing);
    RUN_TEST(test_replay_of_truth_is_exact);
    RUN_TEST(test_replay_separates_a_good_fit_from_a_bad_one);
    RUN_TEST(test_replay_reports_both_minima);
    RUN_TEST(test_replay_handles_empty_input);
    return UNITY_END();
}
