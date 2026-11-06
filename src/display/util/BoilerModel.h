// The thermal model the compensations in ThermalBank.h and ThermalRecovery.h both reason about, written down once so
// the simulator, the parameter identifier and the tests all run the same physics.
//
// One well-mixed node plus a sensor lag:
//
//   C  dT/dt  = P*duty - flow*c*(T - inlet) - UA*(T - ambient)
//   tau dTs/dt = T - Ts
//
// T is the boiler, Ts is what the thermocouple reports. Three terms drive it: the element, the enthalpy the outgoing
// water carries away, and the standing loss to the room. Banking and recovery are both arithmetic on the second term,
// so this is the model they are implicitly assuming -- having it in one place is what lets it be checked against a
// real machine instead of asserted.
//
// Why one node and not two. A brass brew boiler is close enough to isothermal over a shot that splitting metal from
// water buys little, while the thermocouple sits on the outside and lags by seconds, which matters a great deal: that
// lag is why the PID's derivative term brakes too late and why the sag looks deeper than it is. So the lag gets its
// own state and the mass does not. Three parameters (C, UA, tau) is also about as much as a single shot log can
// actually identify -- see BoilerIdentifier.h.
//
// Standing loss is written as UA*(T - ambient) rather than the fixed 5 W in Heater.h. It is the same idea with the
// temperature dependence kept, which is what makes it identifiable from a warm-up or an idle stretch.
#pragma once

#include <cmath>

constexpr float BOILER_MODEL_WATER_SPECIFIC_HEAT = 4.18f; // J/(g·°C), density taken as 1 g/ml

struct BoilerModelParams {
    float capacityJPerK = 3500.0f;  // C: boiler, group and the water in it
    float lossWPerK = 1.2f;         // UA: standing loss to the room
    float sensorTauS = 4.0f;        // tau: thermocouple lag
    float heaterWatts = 900.0f;     // P: element rating at full duty
    float ambientC = 21.0f;         // room
    float inletC = 21.0f;           // water entering the boiler
};

struct BoilerModelState {
    float boilerC = 21.0f; // T
    float sensorC = 21.0f; // Ts, the only thing a real machine can measure
};

// One explicit Euler step. dt is expected to be well inside the fastest time constant in play; the firmware samples at
// 100 ms and the sensor lag is seconds, so that holds comfortably. Returns the net power into the boiler node, which
// is what makes an energy balance checkable from outside.
inline float boilerModelStep(BoilerModelState &state, const BoilerModelParams &params, float dtS, float dutyFraction,
                             float flowMlPerS) {
    if (dtS <= 0.0f || params.capacityJPerK <= 0.0f) {
        return 0.0f;
    }
    if (dutyFraction < 0.0f) {
        dutyFraction = 0.0f;
    } else if (dutyFraction > 1.0f) {
        dutyFraction = 1.0f;
    }
    if (flowMlPerS < 0.0f) {
        flowMlPerS = 0.0f;
    }

    const float heating = params.heaterWatts * dutyFraction;
    // Water enters at inlet and leaves at the boiler temperature, so the boiler pays the whole rise.
    const float carriedOut = flowMlPerS * BOILER_MODEL_WATER_SPECIFIC_HEAT * (state.boilerC - params.inletC);
    const float lost = params.lossWPerK * (state.boilerC - params.ambientC);
    const float net = heating - carriedOut - lost;

    state.boilerC += net * dtS / params.capacityJPerK;
    if (params.sensorTauS > 0.0f) {
        // Exact discrete solution of the lag rather than another Euler step: at tau = 4 s and dt = 1 s Euler would be
        // noticeably off, and the lag is the part the PID reacts to.
        const float alpha = 1.0f - std::exp(-dtS / params.sensorTauS);
        state.sensorC += (state.boilerC - state.sensorC) * alpha;
    } else {
        state.sensorC = state.boilerC;
    }
    return net;
}

// Steady-state duty that holds a given boiler temperature at a given flow. What the feedforward in Heater.cpp is
// trying to work out, and the quickest way to see where an element runs out: anything over 1.0 cannot be held.
inline float boilerModelRequiredDuty(const BoilerModelParams &params, float boilerC, float flowMlPerS) {
    if (params.heaterWatts <= 0.0f) {
        return 0.0f;
    }
    const float carriedOut = flowMlPerS * BOILER_MODEL_WATER_SPECIFIC_HEAT * (boilerC - params.inletC);
    const float lost = params.lossWPerK * (boilerC - params.ambientC);
    return (carriedOut + lost) / params.heaterWatts;
}

// Flow at which the element saturates holding boilerC. Past this the shortfall comes out of the stored heat, which is
// the whole subject of ThermalBank.h and ThermalRecovery.h.
inline float boilerModelMaxSustainableFlow(const BoilerModelParams &params, float boilerC) {
    const float rise = boilerC - params.inletC;
    if (rise <= 0.0f) {
        return 0.0f;
    }
    const float forLoss = params.lossWPerK * (boilerC - params.ambientC);
    const float forWater = params.heaterWatts - forLoss;
    return forWater > 0.0f ? forWater / (BOILER_MODEL_WATER_SPECIFIC_HEAT * rise) : 0.0f;
}
