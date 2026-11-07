// Measures a boiler's standing loss from telemetry it was producing anyway.
//
// WHY THIS IS WORTH ITS OWN FILE. Every other thermal parameter in this directory is fitted: a filter or a search is
// handed a model and asked which numbers make it match. The standing loss is the one that does not need any of that,
// because a boiler sitting at temperature with the valve shut is doing exactly one thing. Over such a stretch,
//
//   integral(P * duty) dt  =  UA * integral(T - ambient) dt  +  C * (T_end - T_start)
//
// and if the stretch is long and ends near where it started, the capacity term vanishes and UA is the ratio of the
// other two. No structure is assumed beyond "the heat went somewhere", no optimiser runs, and there is no starting
// guess for the answer to drift back towards.
//
// WHY IT MATTERS MORE THAN IT LOOKS. A shot cannot identify this. In the stratified model the idle gradient between
// the two halves of the boiler IS the loss divided by the mixing, so a shot sees only their ratio: a fit wanting less
// mixing can have it by taking the loss down too, and the only thing stopping it is that the bottom of the boiler
// would have to be above boiling. That is a bound, not a measurement, and it was what produced every mixing figure
// this project printed before this file existed. Measure the loss and the mixing comes free.
//
// It needs one thing a shot log does not contain: the duty. The websocket publishes it (hp), so tools/thermalcap can
// record it, and the firmware has it directly -- which is the other reason this is a header and not a function inside
// a tool. A machine could measure its own standing loss while it idles, and then the Experimental tab's figures would
// rest on one fewer guess.
#pragma once

#include <cmath>

// A boiler idling is not perfectly still: the duty cycles, and the sensor dithers. So windows are used rather than
// instants, and a window has to be long enough that the cycling averages out.
struct StandingLossConfig {
    float heaterWatts = 900.0f;
    float ambientC = 21.0f;
    float stillFlowMlPerS = 0.05f; // at or below this the pump is off, not merely slow
    float minWindowS = 45.0f;      // shorter than this and the duty's own cycling swamps the mean
};

// Fold telemetry in one sample at a time, then close it. Streaming rather than array-at-once so the firmware could
// run it live, and so a capture with holes in it can simply skip the intervals it did not observe.
class StandingLossMeter {
  public:
    explicit StandingLossMeter(const StandingLossConfig &config) : cfg(config) {}

    // One interval. Anything with flow above the threshold ends the current window rather than being averaged into
    // it: water leaving the boiler is a second way out for the heat, and this estimator knows about only one.
    void sample(float dtS, float sensorC, float flowMlPerS, float dutyFraction) {
        if (dtS <= 0.0f || !std::isfinite(sensorC) || !std::isfinite(dutyFraction)) {
            return;
        }
        if (flowMlPerS > cfg.stillFlowMlPerS) {
            closeWindow();
            return;
        }
        if (dutyFraction < 0.0f) {
            dutyFraction = 0.0f;
        } else if (dutyFraction > 1.0f) {
            dutyFraction = 1.0f;
        }
        if (windowS <= 0.0f) {
            windowStartC = sensorC;
        }
        windowEndC = sensorC;
        windowS += dtS;
        windowEnergyJ += static_cast<double>(cfg.heaterWatts) * dutyFraction * dtS;
        windowAboveKs += static_cast<double>(sensorC - cfg.ambientC) * dtS;
        windowWeightedC += static_cast<double>(sensorC) * dtS;
    }

    // Ends the stretch in progress. Call once after the last sample -- a window still open has not been counted.
    void finish() { closeWindow(); }

    bool identified() const { return windowCount > 0 && aboveKs > 0.0; }

    // UA, in W/K.
    float lossWPerK() const { return identified() ? static_cast<float>(energyJ / aboveKs) : 0.0f; }

    // How much of that could be the C * (T_end - T_start) term instead, given a heat capacity. This is the number
    // that says whether the answer is a measurement: a machine still settling will drift enough that the bound is a
    // large fraction of the figure, and then a longer stretch is needed rather than more arithmetic.
    float driftBoundWPerK(float capacityJPerK) const {
        return identified() ? static_cast<float>(capacityJPerK * driftK / aboveKs) : 0.0f;
    }

    // The watts it takes to hold the boiler where it was, which is the same measurement said the useful way round.
    float holdingWatts() const { return lossWPerK() * (meanTempC() - cfg.ambientC); }

    float meanTempC() const { return idleS_ > 0.0f ? static_cast<float>(weightedC / idleS_) : 0.0f; }
    float idleS() const { return idleS_; }
    int windows() const { return windowCount; }

  private:
    void closeWindow() {
        if (windowS >= cfg.minWindowS && windowAboveKs > 0.0) {
            energyJ += windowEnergyJ;
            aboveKs += windowAboveKs;
            weightedC += windowWeightedC;
            driftK += std::fabs(windowEndC - windowStartC);
            idleS_ += windowS;
            windowCount++;
        }
        windowS = 0.0f;
        windowEnergyJ = 0.0;
        windowAboveKs = 0.0;
        windowWeightedC = 0.0;
        windowStartC = 0.0f;
        windowEndC = 0.0f;
    }

    StandingLossConfig cfg;

    // Accepted windows.
    double energyJ = 0.0;
    double aboveKs = 0.0;
    double weightedC = 0.0;
    double driftK = 0.0;
    float idleS_ = 0.0f;
    int windowCount = 0;

    // The window in progress, discarded if it turns out too short.
    float windowS = 0.0f;
    double windowEnergyJ = 0.0;
    double windowAboveKs = 0.0;
    double windowWeightedC = 0.0;
    float windowStartC = 0.0f;
    float windowEndC = 0.0f;
};
