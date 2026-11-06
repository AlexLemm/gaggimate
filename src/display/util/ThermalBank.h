// Pre-shot thermal banking. The boiler element cannot heat water fast enough for the high-flow phases of a profile:
// at a 93 °C setpoint and 21 °C inlet water, every ml/s of flow needs ~300 W, so anything past ~3 ml/s pins a 900 W
// heater at 100 % duty and the shortfall is paid out of the boiler's thermal mass as a temperature drop. Feedforward
// (Heater::calculateDisturbanceFeedforwardGain) already asks for that power, but it cannot create power the element
// does not have.
//
// The shortfall is known before the shot starts, because the profile states the flow and duration of every phase, so
// the boiler can wait at a higher temperature and spend the stored energy on the shot instead of sagging through it.
// What has to be banked is the worst *cumulative* shortfall at any point in the shot, not the total: a bloom phase
// pumps nothing, so the heater catches up during it. Surpluses only refill the bank up to the setpoint, since a boiler
// sitting at its setpoint stops storing energy.
#pragma once

#include <cstddef>

// Matches Heater::WATER_SPECIFIC_HEAT so both ends of the compensation use one number.
constexpr float THERMAL_BANK_WATER_SPECIFIC_HEAT = 4.18f; // J/(g·°C), density taken as 1 g/ml

struct ThermalBankPhase {
    float durationS = 0.0f;  // planned duration of the phase
    float flowMlPerS = 0.0f; // planned pump flow; 0 when the pump is idle
};

struct ThermalBankParams {
    float setpointC = 0.0f;           // brew temperature the profile asks for
    float inletC = 0.0f;              // water temperature entering the boiler
    float heaterWatts = 0.0f;         // element rating
    float boilerCapacityJPerK = 0.0f; // effective heat capacity of boiler, group and water
    float maxBankK = 0.0f;            // user's cap on how far above setpoint to park
    float ceilingC = 0.0f;            // absolute boiler temperature limit
};

struct ThermalBankPlan {
    float waterMl = 0.0f;          // water the profile plans to pump
    float energyNeededJ = 0.0f;    // energy to lift all of it from inlet to setpoint
    float energyAvailableJ = 0.0f; // energy the element can deliver over the same time
    float peakDeficitJ = 0.0f;     // worst cumulative shortfall during the shot
    float bankK = 0.0f;            // offset to add to the setpoint while the machine waits
};

// Returns an all-zero plan (no banking) for inputs that cannot produce a meaningful answer: a setpoint at or below the
// inlet temperature, a missing heater or heat-capacity figure, or a profile that pumps nothing.
inline ThermalBankPlan planThermalBank(const ThermalBankPhase *phases, size_t count, const ThermalBankParams &params) {
    ThermalBankPlan plan;

    const float deltaT = params.setpointC - params.inletC;
    if (phases == nullptr || count == 0 || deltaT <= 0.0f || params.heaterWatts <= 0.0f ||
        params.boilerCapacityJPerK <= 0.0f || params.maxBankK <= 0.0f) {
        return plan;
    }

    const float joulesPerMl = THERMAL_BANK_WATER_SPECIFIC_HEAT * deltaT;
    float cumulativeDeficit = 0.0f;

    for (size_t i = 0; i < count; i++) {
        const float duration = phases[i].durationS;
        const float flow = phases[i].flowMlPerS;
        if (duration <= 0.0f) {
            continue;
        }

        const float water = flow > 0.0f ? flow * duration : 0.0f;
        const float needed = water * joulesPerMl;
        const float available = params.heaterWatts * duration;

        plan.waterMl += water;
        plan.energyNeededJ += needed;
        plan.energyAvailableJ += available;

        cumulativeDeficit += needed - available;
        if (cumulativeDeficit < 0.0f) {
            cumulativeDeficit = 0.0f; // the boiler is back at setpoint; it banks nothing further
        }
        if (cumulativeDeficit > plan.peakDeficitJ) {
            plan.peakDeficitJ = cumulativeDeficit;
        }
    }

    if (plan.peakDeficitJ <= 0.0f) {
        return plan; // the element keeps up on its own
    }

    float bank = plan.peakDeficitJ / params.boilerCapacityJPerK;
    if (bank > params.maxBankK) {
        bank = params.maxBankK;
    }
    // Never park the boiler above the ceiling, whatever the deficit asks for.
    if (params.ceilingC > 0.0f && params.setpointC + bank > params.ceilingC) {
        bank = params.ceilingC - params.setpointC;
    }
    plan.bankK = bank > 0.0f ? bank : 0.0f;

    return plan;
}
