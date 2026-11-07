// The controller's own heating law, written down off the machine so the offline tools can say what duty the element
// was given at a moment they only have a log of.
//
// A shot log records temperatures, pressures, flows and weights, but never the heater duty. Everything the identifier
// concludes therefore rests on inferring it, and the first attempt inferred it from physics -- "this flow is more than
// 900 W can hold, so the element must be flat out". That is not what decides it. The element is flat out when the
// controller asks for more than it has, and on this firmware the disturbance feedforward asks for a great deal:
//
//   DistFF = (flow * c * (setpoint - inlet) + heatLoss) / efficiency * Kff        [output units]
//
// At 8 ml/s, a 93 C setpoint, 21 C inlet and Kff = 1.111 that is 2822 units against an output span of 1000. The
// element is pinned at 100 % whatever the error is doing, and stays pinned until the safety scaling cuts the
// feedforward off above +1 C of error. So a boiler sitting on its setpoint under fast flow is at full power -- exactly
// the case BoilerIdentifier::dutyIsCertainlyFull used to throw away.
//
// Mirrors Heater::calculateDisturbanceFeedforwardGain, Heater::calculateSafetyScaling and SimplePID::update. A
// separate header rather than reaching into the controller library, because that library is Arduino-only while this
// has to build natively for the tools and the tests.
#pragma once

#include <cmath>

// Heater.h: the PID output runs 0..1000, and Kff is quoted in those units per watt (1000 / wattage).
constexpr float HEATER_OUTPUT_SPAN = 1000.0f;
// Heater.h members: an immersion element is taken as 95 % efficient and the loss the feedforward allows for is a flat
// 5 W. The firmware's numbers, not ours -- the point here is to reproduce it, not to improve it.
constexpr float HEATER_FF_EFFICIENCY = 0.95f;
constexpr float HEATER_FF_HEAT_LOSS_W = 5.0f;
constexpr float HEATER_WATER_SPECIFIC_HEAT = 4.18f;
// SimplePID: setSamplingFrequency(TUNER_OUTPUT_SPAN / 1000) = 1 Hz, and update() refuses to run more often than that.
constexpr float HEATER_SAMPLE_PERIOD_S = 1.0f;
// Heater::loopPid smooths the safety factor with this weight on the new value.
constexpr float HEATER_SAFETY_ALPHA = 0.85f;

struct HeaterControlParams {
    float kp = 29.0f;     // output units per K
    float ki = 0.11f;     // output units per K per second
    float kd = 379.5f;    // output units per K per (K/s)
    float kff = 1.111f;   // output units per watt, = 1000 / element wattage
    float inletC = 21.0f; // what the controller was told the tank is at
    float samplePeriodS = HEATER_SAMPLE_PERIOD_S;
};

// Heater::calculateSafetyScaling, verbatim. Above +1 C of error the feedforward is switched off entirely, and that is
// the one condition which can unpin a saturated element, so it is the condition the duty test turns on.
inline float heaterSafetyScaling(float tempErrorC) {
    if (tempErrorC > 1.0f) {
        return 0.0f;
    }
    if (tempErrorC >= 0.0f) {
        return 0.7f + 0.3f * (1.0f - tempErrorC);
    }
    if (tempErrorC > -1.0f) {
        return 0.7f + 0.3f * std::fabs(tempErrorC);
    }
    return 1.0f;
}

// The feedforward in output units, before the safety scaling. calculateDisturbanceFeedforwardGain computes a gain per
// ml/s and SimplePID multiplies by the flow again, so the flat heat-loss allowance ends up added once rather than
// scaled with the flow. Reproduced rather than tidied, because the tools have to match the machine.
inline float heaterFeedforwardUnits(const HeaterControlParams &params, float setpointC, float flowMlPerS) {
    if (params.kff <= 0.0f || flowMlPerS <= 0.01f) {
        return 0.0f;
    }
    const float rise = setpointC - params.inletC;
    if (rise <= 0.0f) {
        return 0.0f;
    }
    const float watts = (flowMlPerS * HEATER_WATER_SPECIFIC_HEAT * rise + HEATER_FF_HEAT_LOSS_W) / HEATER_FF_EFFICIENCY;
    return watts * params.kff;
}

// Whether the element was certainly at 100 % over this sample, judged from what the controller must have asked for.
//
// Two conditions, both about the controller rather than the boiler:
//
//  - the sensor is not more than 1 C above the setpoint, so the safety scaling has not cut the feedforward off;
//  - the feedforward alone, at the lowest scaling it can carry while active, still exceeds the span by marginFactor.
//
// The margin is what makes the proportional and derivative terms irrelevant. They can only subtract while the boiler
// is above its setpoint and rising, and a 1.5x margin leaves 500 units of room -- far more than Kp and Kd can remove
// across the small errors this test allows. Without a margin, a sample sitting exactly at the span would be a guess.
inline bool heaterDutyCertainlyFull(const HeaterControlParams &params, float sensorC, float setpointC, float flowMlPerS,
                                    float marginFactor) {
    if (setpointC <= 0.0f || flowMlPerS <= 0.01f) {
        return false;
    }
    const float error = sensorC - setpointC;
    // A little room below the +1 C cut-off: the factor is smoothed across samples, so a boiler that has just come down
    // from above the cut-off is still carrying some of the zero.
    if (error > 0.8f) {
        return false;
    }
    const float ff = heaterFeedforwardUnits(params, setpointC, flowMlPerS);
    // 0.7 is the floor of heaterSafetyScaling wherever it is non-zero, so this is the least the feedforward can be
    // worth while it is active.
    return ff * 0.7f >= HEATER_OUTPUT_SPAN * marginFactor;
}

// The flow above which the feedforward alone saturates the output: the test above solved for flow, so a tool can
// report "fast enough" in the units a profile is actually written in.
inline float heaterSaturatingFlow(const HeaterControlParams &params, float setpointC, float marginFactor) {
    const float rise = setpointC - params.inletC;
    if (params.kff <= 0.0f || rise <= 0.0f) {
        return 0.0f;
    }
    const float wattsNeeded = HEATER_OUTPUT_SPAN * marginFactor / (0.7f * params.kff);
    const float flowWatts = wattsNeeded * HEATER_FF_EFFICIENCY - HEATER_FF_HEAT_LOSS_W;
    return flowWatts > 0.0f ? flowWatts / (HEATER_WATER_SPECIFIC_HEAT * rise) : 0.0f;
}

// The whole loop, for reconstructing a duty trace from a log. Driven by the measured sensor temperature rather than by
// a model's, so the duty it produces does not depend on whichever model is being fitted -- which is what lets one
// reconstructed trace score every candidate.
struct HeaterControlState {
    float integral = 0.0f; // SimplePID::feedback_integralState, in K*s
    float previousError = 0.0f;
    float safetyFactor = 1.0f;
    bool started = false;
    float sinceUpdateS = 0.0f;
    float duty = 0.0f; // last commanded duty, held between PID ticks
};

// Puts the integral where it must already have been for the element to be holding steady at this temperature. A
// machine that has been idling at its setpoint is not carrying a zero integral, and starting from zero would have the
// reconstruction open every shot with the element off.
inline void heaterSeedIntegral(HeaterControlState &state, const HeaterControlParams &params, float holdingDuty) {
    if (params.ki <= 0.0f) {
        state.integral = 0.0f;
        return;
    }
    state.integral = holdingDuty * HEATER_OUTPUT_SPAN / params.ki;
}

// One step. Returns the duty in force over the interval; the PID only recomputes on its own 1 Hz boundary, so between
// ticks the previous value is held, exactly as the software PWM does.
inline float heaterControlStep(HeaterControlState &state, const HeaterControlParams &params, float dtS, float sensorC,
                               float setpointC, float flowMlPerS, bool valveOpen) {
    if (dtS <= 0.0f) {
        return state.duty;
    }
    state.sinceUpdateS += dtS;
    if (state.started && state.sinceUpdateS < params.samplePeriodS) {
        return state.duty;
    }
    const float period = state.started ? state.sinceUpdateS : params.samplePeriodS;
    state.sinceUpdateS = 0.0f;

    if (setpointC <= 0.0f) {
        state.duty = 0.0f;
        state.started = true;
        return 0.0f;
    }

    float distFF = 0.0f;
    if (valveOpen && flowMlPerS > 0.01f) {
        const float raw = heaterSafetyScaling(sensorC - setpointC);
        state.safetyFactor = HEATER_SAFETY_ALPHA * raw + (1.0f - HEATER_SAFETY_ALPHA) * state.safetyFactor;
        distFF = heaterFeedforwardUnits(params, setpointC, flowMlPerS) * state.safetyFactor;
    }

    const float error = setpointC - sensorC;
    if (!state.started) {
        state.previousError = error;
        state.started = true;
    }

    const float pOut = params.kp * error;
    state.integral += error * period;
    float iOut = params.ki * state.integral;
    const float dOut = params.kd * (error - state.previousError) / period;

    float sum = pOut + iOut + dOut + distFF;
    const bool saturated = sum < 0.0f || sum > HEATER_OUTPUT_SPAN;
    const bool sameSign = (error > 0.0f && sum > 0.0f) || (error < 0.0f && sum < 0.0f);
    if (saturated && sameSign) {
        // SimplePID's conditional integration: undo the step that pushed it further into the stop.
        state.integral -= error * period;
        iOut = params.ki * state.integral;
        sum = pOut + iOut + dOut + distFF;
    }

    state.previousError = error;
    if (sum < 0.0f) {
        sum = 0.0f;
    } else if (sum > HEATER_OUTPUT_SPAN) {
        sum = HEATER_OUTPUT_SPAN;
    }
    state.duty = sum / HEATER_OUTPUT_SPAN;
    return state.duty;
}
