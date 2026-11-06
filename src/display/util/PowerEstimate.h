// Estimates what the machine is drawing from the wall, and how much it has used, from what the firmware already knows
// it is doing.
//
// No current sensor is involved. Every load in a single-boiler machine is either full on or driven at a duty the
// firmware itself commands, so the draw is the sum of (rating x duty) over the loads, and the ratings are measurable
// once with a plug meter. On the user's Rancilio Silvia, measured that way: 900 W boiler element, 59 W pump at full.
//
// Deliberately separate from everything else. It needs no thermal model, no PID state and no profile -- just duties
// and ratings -- so it stays a function of its inputs and can be used, tested and changed on its own.
//
// HOW GOOD IS IT. The element is the whole answer to within a few per cent: it is 94 % of the full-load draw, it is
// purely resistive, and its duty is exactly known because the firmware sets it. The pump is the soft part. A vibratory
// pump under phase-angle control does not draw power in proportion to its commanded percentage -- the relationship is
// closer to the square of the conduction fraction than to a straight line -- so the linear term below is an
// approximation worth 59 W at most. That is 6 % of full load, and the error inside it less, which is accurate enough
// for "how much does a shot cost" and not accurate enough to bill anyone. pumpCurve exists to improve on it without
// touching anything else.
#pragma once

// Measured at the plug on a Rancilio Silvia. Both are per-machine and belong in settings rather than here; these are
// the defaults a machine that has never been measured starts from.
constexpr float POWER_ESTIMATE_DEFAULT_HEATER_W = 900.0f;
constexpr float POWER_ESTIMATE_DEFAULT_PUMP_W = 59.0f;
// Controller board, display, solenoid and standing electronics. Not measured -- small enough that being wrong about it
// costs little, large enough that leaving it out makes an idle machine read as 0 W, which is plainly false.
constexpr float POWER_ESTIMATE_DEFAULT_STANDBY_W = 4.0f;
// Brew valve solenoid, energised whenever the valve is open.
constexpr float POWER_ESTIMATE_DEFAULT_VALVE_W = 8.0f;

struct PowerEstimateParams {
    float heaterWatts = POWER_ESTIMATE_DEFAULT_HEATER_W;
    float pumpWatts = POWER_ESTIMATE_DEFAULT_PUMP_W;
    float standbyWatts = POWER_ESTIMATE_DEFAULT_STANDBY_W;
    float valveWatts = POWER_ESTIMATE_DEFAULT_VALVE_W;
    // Exponent on the pump's commanded fraction. 1.0 is linear, which is what this ships with; raising it towards 2
    // bends the curve the way phase-angle control actually behaves, once someone has measured their own pump at a few
    // settings. It exists so that correction needs no change anywhere else.
    float pumpCurve = 1.0f;
};

struct PowerEstimateInputs {
    float heaterDutyPercent = 0.0f; // the controller's own duty figure, 0-100
    float pumpPowerPercent = 0.0f;  // commanded pump drive, 0-100
    bool valveOpen = false;
    bool altRelayOn = false; // second SSR: grinder or steam boiler, depending on configuration
    float altRelayWatts = 0.0f;
};

struct PowerEstimate {
    float totalW = 0.0f;
    float heaterW = 0.0f;
    float pumpW = 0.0f;
    float otherW = 0.0f; // standby, valve and the alt relay
};

inline float powerEstimateClampFraction(float percent) {
    const float fraction = percent / 100.0f;
    if (fraction < 0.0f) {
        return 0.0f;
    }
    return fraction > 1.0f ? 1.0f : fraction;
}

inline PowerEstimate estimatePower(const PowerEstimateInputs &in, const PowerEstimateParams &params) {
    PowerEstimate out;
    out.heaterW = params.heaterWatts * powerEstimateClampFraction(in.heaterDutyPercent);

    const float pumpFraction = powerEstimateClampFraction(in.pumpPowerPercent);
    // Avoid pow() for the common linear case: this runs on every telemetry frame on an ESP32.
    float pumpScaled = pumpFraction;
    if (params.pumpCurve > 1.0f && pumpFraction > 0.0f) {
        pumpScaled = pumpFraction * pumpFraction; // the one correction worth having without a measured curve
        if (params.pumpCurve < 2.0f) {
            // Blend linear and square rather than calling pow for an exponent between them.
            const float weight = params.pumpCurve - 1.0f;
            pumpScaled = pumpFraction * (1.0f - weight) + pumpScaled * weight;
        }
    }
    out.pumpW = params.pumpWatts * pumpScaled;

    out.otherW = params.standbyWatts;
    if (in.valveOpen) {
        out.otherW += params.valveWatts;
    }
    if (in.altRelayOn) {
        out.otherW += in.altRelayWatts;
    }

    out.totalW = out.heaterW + out.pumpW + out.otherW;
    return out;
}

// Running energy total. Separate from the instantaneous estimate because it is the thing with state: the estimate is a
// function, this remembers. Keeps joules internally and converts on the way out, so a long session does not lose
// resolution to a float holding kWh.
class PowerAccumulator {
  public:
    void reset() {
        joules = 0.0f;
        lastW = 0.0f;
    }

    void accumulate(float dtS, float watts) {
        if (dtS <= 0.0f || watts < 0.0f) {
            return;
        }
        // Trapezoidal: the duty changes stepwise between frames, and averaging the endpoints tracks a ramp (the pump
        // coming up to pressure, the element easing off) closer than holding either one.
        joules += 0.5f * (lastW + watts) * dtS;
        lastW = watts;
    }

    float energyJ() const { return joules; }
    float energyWh() const { return joules / 3600.0f; }

  private:
    float joules = 0.0f;
    float lastW = 0.0f;
};
