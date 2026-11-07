// Two cells in series instead of one well-mixed node, because one node cannot explain what this machine does during
// a shot and a pair of conducting masses cannot either.
//
// WHAT ONE NODE PREDICTS, AND WHAT HAPPENED. BoilerModel.h assumes a single well-mixed volume: the element heats it,
// outgoing water cools it, the thermocouple watches it through a lag. That says a 37.8 ml fill at 8 ml/s removes
// 11.4 kJ while the element returns 4.7 kJ, so the sensor must fall about 1.9 C. On shot 55 of the author's machine,
// at a temperature offset of 0, the sensor ROSE 0.5 C through that fill, began falling three seconds after it ended,
// and reached 7.6 C down some forty seconds later at a flow of barely 1.3 ml/s. Asked to explain a rise while 6.6 kJ
// leaves, a one-node fit returns a negative heat capacity; allowed to keep the sign, it runs to a 20 kJ/K boiler --
// six times anything a Silvia contains -- and still fits badly. The structure is wrong, not the numbers.
//
// WHY STRATIFICATION AND NOT TWO CONDUCTING MASSES. The obvious repair is a fast water mass and a slow metal one with
// a conductance between them, the sensor on the metal. Fitted to these logs that lands on a 5 W/K coupling, which is
// another way of saying the two never talk -- and it implies the water sits 17 C above the metal while idling, so the
// boiler would be at 110 C doing nothing. A degenerate answer dressed as a fit.
//
// The real mechanism is simpler and is about geometry. 37.8 ml out of a 300 ml boiler replaces 13 % of it. Cold water
// enters at the bottom, where the element is, and water leaves from the top. For a draw that small the cold front
// never reaches the outlet: the water going to the group is still the water that was already hot, and the sensor,
// which is on the boiler near that outlet, sees nothing. The deficit arrives later, as the front works its way up
// over a turnover time of Cb / (flow * c) -- about 19 s for half a boiler at 8 ml/s, which is the delay in the trace.
//
// So the two cells are stacked, not side by side:
//
//   Cb dTb/dt = P*duty + flow*c*(inlet - Tb) - Kx*(Tb - Tt)
//   Ct dTt/dt = flow*c*(Tb - Tt) + Kx*(Tb - Tt) - UA*(Tt - ambient)
//   tau dTs/dt = Tt - Ts
//
// Tb is the lower half, where the cold arrives and the element sits. Tt is the upper half, which feeds the group and
// carries the thermocouple. Kx is how fast the two mix by convection, separate from the flow that carries one into
// the other. Each cell is water plus its share of the brass, which is why a fitted capacity is larger than the water
// alone.
//
// Two consequences, because they change what a compensation should do:
//
//  - The water reaching the puck is Tt, not Ts. The sensor lags it, and during the opening phase the two are close
//    while the bottom of the boiler is already far colder than either. The brew temperature is the one temperature
//    nobody sees.
//  - A compensation timed to the opening phase is early. The sensed deficit arrives tens of seconds later, which is
//    why banking heat before a shot and spending it at the start misses the sag, while paying a measured debt back as
//    it accrues does not.
//
// Deliberately beside BoilerModel.h rather than replacing it: the shipped compensations reason with the one-node
// model and are not being changed on the strength of one machine's logs.
#pragma once

#include <cmath>

constexpr float BOILER_TWO_MASS_WATER_SPECIFIC_HEAT = 4.18f; // J/(g.C), density taken as 1 g/ml
// Past atmospheric boiling a boiler makes steam rather than storing heat, so a set of parameters implying idle water
// above this is describing something that is not a boiler. Used to reject degenerate fits.
constexpr float BOILER_TWO_MASS_CEILING_C = 100.0f;

struct BoilerTwoMassParams {
    // Starting points for the author's Silvia: a 300 ml boiler split in half, each cell holding ~150 ml of water
    // (627 J/K) plus its share of roughly 2 kg of brass (377 J/K). Priors, not measurements.
    float inletCapacityJPerK = 1000.0f;  // Cb: lower half, where cold water arrives and the element sits
    float outletCapacityJPerK = 1000.0f; // Ct: upper half, which feeds the group and carries the sensor
    float mixingWPerK = 60.0f;           // Kx: convective mixing between the halves, over and above the flow
    float lossWPerK = 1.2f;              // UA: standing loss off the upper half
    float sensorTauS = 4.0f;             // tau: thermocouple against the shell it is clamped to
    float heaterWatts = 900.0f;          // P at full duty, into the lower half
    float ambientC = 21.0f;
    float inletC = 21.0f;
};

struct BoilerTwoMassState {
    float inletCellC = 21.0f;  // Tb
    float outletCellC = 21.0f; // Tt, and what the puck actually gets
    float sensorC = 21.0f;     // Ts, the only one a real machine reports
};

struct BoilerTwoMassPower {
    float intoInletCellW = 0.0f; // element, less what the arriving cold water costs, less what mixes upward
    float advectedW = 0.0f;      // flow * c * (Tb - Tt): the front working its way up
    float mixedW = 0.0f;         // Kx * (Tb - Tt)
    float coldInW = 0.0f;        // flow * c * (inlet - Tb), negative while the tank is colder than the boiler
    float lostW = 0.0f;          // standing loss off the upper half
};

// One explicit Euler step. The fastest time constant is Cb / (flow * c + Kx), about 4 s at 8 ml/s, so the 100 ms the
// firmware samples at and the ~250 ms a log records at are both well inside it.
inline BoilerTwoMassPower boilerTwoMassStep(BoilerTwoMassState &state, const BoilerTwoMassParams &params, float dtS,
                                            float dutyFraction, float flowMlPerS) {
    BoilerTwoMassPower out;
    if (dtS <= 0.0f || params.inletCapacityJPerK <= 0.0f || params.outletCapacityJPerK <= 0.0f) {
        return out;
    }
    if (dutyFraction < 0.0f) {
        dutyFraction = 0.0f;
    } else if (dutyFraction > 1.0f) {
        dutyFraction = 1.0f;
    }
    if (flowMlPerS < 0.0f) {
        flowMlPerS = 0.0f;
    }

    const float flowW = flowMlPerS * BOILER_TWO_MASS_WATER_SPECIFIC_HEAT;
    out.coldInW = flowW * (params.inletC - state.inletCellC);
    out.mixedW = params.mixingWPerK * (state.inletCellC - state.outletCellC);
    out.advectedW = flowW * (state.inletCellC - state.outletCellC);
    out.lostW = params.lossWPerK * (state.outletCellC - params.ambientC);
    out.intoInletCellW = params.heaterWatts * dutyFraction + out.coldInW - out.mixedW;

    const float intoOutletCell = out.advectedW + out.mixedW - out.lostW;
    state.inletCellC += out.intoInletCellW * dtS / params.inletCapacityJPerK;
    state.outletCellC += intoOutletCell * dtS / params.outletCapacityJPerK;

    if (params.sensorTauS > 0.0f) {
        const float alpha = 1.0f - std::exp(-dtS / params.sensorTauS);
        state.sensorC += (state.outletCellC - state.sensorC) * alpha;
    } else {
        state.sensorC = state.outletCellC;
    }
    return out;
}

// Duty that holds the outlet at outletC indefinitely at this flow. At steady state everything the element supplies
// leaves either with the water drawn off or, off the upper half, to the room -- so this is the same arithmetic as the
// one-node model, the stratification having washed out.
inline float boilerTwoMassRequiredDuty(const BoilerTwoMassParams &params, float outletC, float flowMlPerS) {
    if (params.heaterWatts <= 0.0f) {
        return 0.0f;
    }
    const float carried = flowMlPerS * BOILER_TWO_MASS_WATER_SPECIFIC_HEAT * (outletC - params.inletC);
    const float lost = params.lossWPerK * (outletC - params.ambientC);
    return (carried + lost) / params.heaterWatts;
}

// Where the states must have been for a machine that has been idling at a sensor reading of sensorC.
//
// A replay has only the sensor to start from. With no flow, the upper half is losing UA*(Tt - ambient) to the room
// and the only thing replacing it is mixing from below, so the lower half sits above the upper by exactly the gap
// that mixing needs to carry the loss. Starting them equal hands the model a boiler with no stored gradient, and the
// replay then sags at the wrong moment.
inline BoilerTwoMassState boilerTwoMassSeedIdle(const BoilerTwoMassParams &params, float sensorC) {
    BoilerTwoMassState state;
    state.sensorC = sensorC;
    state.outletCellC = sensorC;
    const float lost = params.lossWPerK * (sensorC - params.ambientC);
    state.inletCellC = params.mixingWPerK > 0.0f ? sensorC + lost / params.mixingWPerK : sensorC;
    return state;
}

// Whether a set of parameters describes something that could be a boiler at this working temperature. The seeding
// above grows without bound as the mixing falls, so a fit left unchecked buys a flat sensor trace by putting the
// bottom of the boiler above boiling. Rejecting that is physics, not a tuning knob.
inline bool boilerTwoMassPlausible(const BoilerTwoMassParams &params, float workingC) {
    if (params.inletCapacityJPerK <= 0.0f || params.outletCapacityJPerK <= 0.0f || params.mixingWPerK <= 0.0f) {
        return false;
    }
    return boilerTwoMassSeedIdle(params, workingC).inletCellC <= BOILER_TWO_MASS_CEILING_C;
}

struct BoilerTwoMassSample {
    float dtS = 0.0f;
    float measuredC = 0.0f;    // what the thermocouple reported
    float flowMlPerS = 0.0f;
    float dutyFraction = 0.0f; // reconstructed from the control law, not logged
};

struct BoilerTwoMassReplay {
    float rmsErrorC = 0.0f;
    float maxErrorC = 0.0f;
    float minOutletC = 0.0f; // the coldest water the puck saw
    float minInletCellC = 0.0f;
    size_t samples = 0;
};

// Open-loop replay over a whole shot: seeded once from the first measurement and never corrected again. Every sample
// counts, because the duty is reconstructed throughout rather than only where it happened to be certain -- which is
// what makes the score a statement about the model instead of about a handful of saturated windows.
inline BoilerTwoMassReplay boilerTwoMassReplay(const BoilerTwoMassSample *samples, size_t count,
                                               const BoilerTwoMassParams &params) {
    BoilerTwoMassReplay out;
    if (samples == nullptr || count == 0) {
        return out;
    }
    BoilerTwoMassState state = boilerTwoMassSeedIdle(params, samples[0].measuredC);
    out.minOutletC = state.outletCellC;
    out.minInletCellC = state.inletCellC;
    float sumSquared = 0.0f;
    for (size_t i = 0; i < count; i++) {
        const BoilerTwoMassSample &s = samples[i];
        if (s.dtS <= 0.0f) {
            continue;
        }
        boilerTwoMassStep(state, params, s.dtS, s.dutyFraction, s.flowMlPerS);
        const float error = state.sensorC - s.measuredC;
        sumSquared += error * error;
        const float magnitude = std::fabs(error);
        if (magnitude > out.maxErrorC) {
            out.maxErrorC = magnitude;
        }
        if (state.outletCellC < out.minOutletC) {
            out.minOutletC = state.outletCellC;
        }
        if (state.inletCellC < out.minInletCellC) {
            out.minInletCellC = state.inletCellC;
        }
        out.samples++;
    }
    out.rmsErrorC = out.samples > 0 ? std::sqrt(sumSquared / static_cast<float>(out.samples)) : 0.0f;
    return out;
}
