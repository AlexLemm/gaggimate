#include <display/util/ThermalBank.h>
#include <unity.h>
#include <vector>

void setUp() {}
void tearDown() {}

// A Silvia measured at 900 W on a plug meter, 21 °C tank, 93 °C brew, and the ~3.3 kJ/K effective capacity derived
// from its own shot logs (123.8 ml pulled 6.7 °C out of the boiler against what the element could supply).
static ThermalBankParams silvia() {
    ThermalBankParams p;
    p.setpointC = 93.0f;
    p.inletC = 21.0f;
    p.heaterWatts = 900.0f;
    p.boilerCapacityJPerK = 3300.0f;
    p.maxBankK = 4.0f;
    p.ceilingC = 98.0f;
    return p;
}

static ThermalBankPlan plan(const std::vector<ThermalBankPhase> &phases, const ThermalBankParams &params) {
    return planThermalBank(phases.data(), phases.size(), params);
}

// 1.3 ml/s needs ~390 W, well inside a 900 W element, so nothing has to be banked.
void test_low_flow_needs_no_bank() {
    const auto result = plan({{30.0f, 1.3f}}, silvia());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 39.0f, result.waterMl);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, result.peakDeficitJ);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, result.bankK);
}

// The opening phase of the user's Adaptive v2 profile: 8 ml/s for 6.5 s needs 2.4 kW from a 900 W element.
void test_high_flow_opening_banks() {
    const auto result = plan({{6.5f, 8.0f}}, silvia());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 52.0f, result.waterMl);
    // 52 ml * 4.18 * 72 = 15.65 kJ needed, 900 W * 6.5 s = 5.85 kJ available.
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 15649.0f, result.energyNeededJ);
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 5850.0f, result.energyAvailableJ);
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 9799.0f, result.peakDeficitJ);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 2.97f, result.bankK); // 9.80 kJ / 3.3 kJ/K
}

// A bloom between two pulls lets the element catch up, so the bank covers the worst moment, not the total shortfall.
void test_bloom_recharges_between_phases() {
    const ThermalBankParams params = silvia();
    const auto single = plan({{6.5f, 8.0f}}, params);
    const auto withBloom = plan({{6.5f, 8.0f}, {20.0f, 0.0f}, {6.5f, 8.0f}}, params);

    TEST_ASSERT_FLOAT_WITHIN(0.1f, 104.0f, withBloom.waterMl); // twice the water
    // 20 s of idle heater banks 18 kJ, more than the 9.8 kJ the first phase cost, so the peak never compounds.
    TEST_ASSERT_FLOAT_WITHIN(1.0f, single.peakDeficitJ, withBloom.peakDeficitJ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, single.bankK, withBloom.bankK);
}

// Back-to-back high-flow phases have no recovery time, so the deficit compounds.
void test_consecutive_high_flow_compounds() {
    const ThermalBankParams params = silvia();
    const auto single = plan({{6.5f, 8.0f}}, params);
    const auto doubled = plan({{6.5f, 8.0f}, {6.5f, 8.0f}}, params);
    TEST_ASSERT_TRUE(doubled.peakDeficitJ > single.peakDeficitJ * 1.9f);
}

void test_bank_is_capped_by_user_limit() {
    ThermalBankParams params = silvia();
    params.maxBankK = 1.5f;
    const auto result = plan({{6.5f, 8.0f}}, params); // would ask for 2.87 K
    TEST_ASSERT_EQUAL_FLOAT(1.5f, result.bankK);
}

// A long flush asks for more than can be banked below boiling; the ceiling wins.
void test_ceiling_caps_bank_below_boiling() {
    ThermalBankParams params = silvia();
    params.maxBankK = 20.0f;
    params.ceilingC = 96.0f;
    const auto result = plan({{14.5f, 8.5f}}, params);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, result.bankK); // 96 - 93
}

void test_ceiling_below_setpoint_banks_nothing() {
    ThermalBankParams params = silvia();
    params.ceilingC = 92.0f; // below the setpoint: no headroom at all
    const auto result = plan({{6.5f, 8.0f}}, params);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, result.bankK);
}

void test_colder_inlet_banks_more() {
    ThermalBankParams warm = silvia();
    ThermalBankParams cold = silvia();
    cold.inletC = 12.0f; // winter tap fill
    const auto warmPlan = plan({{6.5f, 8.0f}}, warm);
    const auto coldPlan = plan({{6.5f, 8.0f}}, cold);
    TEST_ASSERT_TRUE(coldPlan.bankK > warmPlan.bankK);
}

void test_invalid_inputs_disable_banking() {
    const std::vector<ThermalBankPhase> phases = {{6.5f, 8.0f}};
    ThermalBankParams params = silvia();

    TEST_ASSERT_EQUAL_FLOAT(0.0f, planThermalBank(nullptr, 1, params).bankK);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, plan({}, params).bankK);

    params = silvia();
    params.inletC = 93.0f; // nothing to heat
    TEST_ASSERT_EQUAL_FLOAT(0.0f, plan(phases, params).bankK);

    params = silvia();
    params.heaterWatts = 0.0f; // wattage never configured
    TEST_ASSERT_EQUAL_FLOAT(0.0f, plan(phases, params).bankK);

    params = silvia();
    params.boilerCapacityJPerK = 0.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, plan(phases, params).bankK);

    params = silvia();
    params.maxBankK = 0.0f; // feature left at its default cap of zero
    TEST_ASSERT_EQUAL_FLOAT(0.0f, plan(phases, params).bankK);
}

// A low-pressure fill phase runs at close to the pump's capability, not at a puck-limited flow: a real 0.2 bar phase
// measured 9.85 ml/s. Banking has to see that, since it is where the flow outruns the element. Mirrors
// Controller::plannedPhaseFlow, which cannot be linked here (it needs Profile, and so ArduinoJson).
void test_fill_phase_flow_dominates_the_bank() {
    const ThermalBankParams params = silvia();
    const auto fill = plan({{14.5f, 9.85f}}, params);
    const auto extraction = plan({{25.0f, 1.3f}}, params);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 142.8f, fill.waterMl);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, extraction.bankK);
    TEST_ASSERT_EQUAL_FLOAT(params.maxBankK, fill.bankK); // far past what can be banked; the cap takes over
}

void test_zero_and_negative_durations_are_ignored() {
    const auto result = plan({{0.0f, 8.0f}, {-5.0f, 8.0f}, {6.5f, 8.0f}}, silvia());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 52.0f, result.waterMl);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_low_flow_needs_no_bank);
    RUN_TEST(test_high_flow_opening_banks);
    RUN_TEST(test_bloom_recharges_between_phases);
    RUN_TEST(test_consecutive_high_flow_compounds);
    RUN_TEST(test_bank_is_capped_by_user_limit);
    RUN_TEST(test_ceiling_caps_bank_below_boiling);
    RUN_TEST(test_ceiling_below_setpoint_banks_nothing);
    RUN_TEST(test_colder_inlet_banks_more);
    RUN_TEST(test_invalid_inputs_disable_banking);
    RUN_TEST(test_fill_phase_flow_dominates_the_bank);
    RUN_TEST(test_zero_and_negative_durations_are_ignored);
    return UNITY_END();
}
