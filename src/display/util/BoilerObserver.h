// Runs BoilerModel.h live on the machine, in parallel with the real boiler and never driving it.
//
// This is what makes the model checkable from the kitchen rather than from a log file. The observer is fed the same
// duty and flow the real boiler just saw, steps the model, and publishes both temperatures so the dashboard can draw
// them on one axis. Where they diverge, the model's parameters are wrong -- and since those parameters are what
// ThermalBank.h and ThermalRecovery.h compute their offsets from, a visible divergence is also a direct explanation of
// why a bank came out too large or too small.
//
// Open loop by design. The model is stepped from its own previous state, not from the measurement, so the two traces
// are genuinely independent and the gap between them means something. A model corrected every sample would sit on top
// of the measurement and show nothing.
//
// Open loop does drift, though, and a trace that has drifted 15 °C away stops being informative. So the observer
// re-seeds itself from the measurement when the error passes a threshold, and reports how often that happened:
// reseeds per minute is a blunt but honest summary of how badly the parameters fit. Zero means the model is keeping up
// unaided.
//
// Separate from the compensations on purpose. Nothing here feeds back into the setpoint, so it can be left running,
// read, and changed without any risk to how the machine heats.
#pragma once

#include "BoilerModel.h"

// Error at which the model is judged to have lost the plot and is restarted from the measurement. Large enough that
// ordinary parameter error does not trip it every few seconds, small enough that the trace stays on the same axis as
// the real one.
constexpr float BOILER_OBSERVER_RESEED_ERROR_C = 5.0f;
// Longest interval the held telemetry is trusted to describe. A longer gap means the loop was blocked, so the model is
// re-seeded rather than propagated across a stretch it has no inputs for.
constexpr float BOILER_OBSERVER_MAX_GAP_S = 2.0f;

class BoilerObserver {
  public:
    // Restart the model from a known temperature. Called on the first sample, after a gap, and whenever the error
    // grows past the threshold.
    void seed(float measuredC) {
        state.boilerC = measuredC;
        state.sensorC = measuredC;
        running = true;
        reseeds++;
    }

    void reset() {
        running = false;
        reseeds = 0;
        lastErrorC = 0.0f;
        netW = 0.0f;
    }

    // One interval. dutyFraction and flowMlPerS are what the real machine just did; measuredC is what its sensor
    // reports now, used only to score the model and to decide whether to re-seed.
    void sample(float dtS, float measuredC, float dutyFraction, float flowMlPerS, const BoilerModelParams &params) {
        if (params.capacityJPerK <= 0.0f || params.heaterWatts <= 0.0f) {
            running = false; // nothing to model with
            return;
        }
        if (!running || dtS <= 0.0f || dtS > BOILER_OBSERVER_MAX_GAP_S) {
            seed(measuredC);
            lastErrorC = 0.0f;
            return;
        }

        netW = boilerModelStep(state, params, dtS, dutyFraction, flowMlPerS);
        lastErrorC = state.sensorC - measuredC;

        const float magnitude = lastErrorC < 0.0f ? -lastErrorC : lastErrorC;
        if (magnitude > BOILER_OBSERVER_RESEED_ERROR_C) {
            seed(measuredC);
            lastErrorC = 0.0f;
        }
    }

    bool isRunning() const { return running; }
    // What the model thinks the thermocouple would read: the trace to plot against the real one.
    float predictedC() const { return state.sensorC; }
    // What the model thinks the water is at. Always ahead of the sensor during a shot, and the quantity the
    // compensations actually care about -- no machine can measure it.
    float predictedBoilerC() const { return state.boilerC; }
    float errorC() const { return lastErrorC; }
    // Net power into the boiler: element minus what the water carried out minus standing loss. Negative means the
    // boiler is being drained faster than the element can refill it, which is the sag as it happens.
    float netPowerW() const { return netW; }
    unsigned long reseedCount() const { return reseeds; }

  private:
    BoilerModelState state;
    bool running = false;
    float lastErrorC = 0.0f;
    float netW = 0.0f;
    unsigned long reseeds = 0;
};
