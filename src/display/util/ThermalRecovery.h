// Reactive thermal recovery: pay back the energy a shot actually removed, measured rather than predicted.
//
// Pre-shot banking (ThermalBank.h) reads the profile and parks the boiler high enough to cover the shortfall it
// predicts. This is the other half, and it answers a different question. Banking asks "how much will this shot take
// out?" before it starts; recovery asks "how much did it take out, and has the element put it back yet?" while it runs
// and after it ends.
//
// The accounting is a running energy balance on the boiler:
//
//   removed  = flow * dt * c * (setpoint - inlet)   water leaving carries this much enthalpy away
//   restored = watts * duty * dt                    the element puts this much back
//   debt    += removed - restored, floored at zero
//
// Flow and duty are both measured, so the debt needs no model of the puck, no profile, and no prediction. The floor at
// zero is what makes it a debt rather than a signed energy state: a boiler sitting at its setpoint with the element
// ticking over is square, not in credit.
//
// Acting on the debt is the part worth being careful about. The obvious reading of "drive the heater at full power for
// E/P seconds" double-counts: during the sag the element is already saturated at 100 %, so most of E is being repaid
// while the debt is still being measured. Running a further E/P seconds of full power after the shot would overshoot
// by whatever the element delivered during it -- for a 30 s shot at 900 W, 27 kJ against a debt that rarely exceeds
// 12 kJ. Subtracting the measured duty is what keeps the sum honest.
//
// What is left is handled by converting the debt back into a temperature and asking for it as an offset:
//
//   boost = min(debt / C, maxBoostK), bounded by the ceiling
//
// While the debt is large the offset exceeds what the PID can reach, so the element runs at 100 % -- "as fast as
// possible" falls out of the arithmetic rather than being commanded. As the debt drains the offset shrinks with it and
// the handover back to the PID is continuous, with no step to overshoot through. The remaining full-power time is
// (debt - maxBoostK * C) / watts, which is the user's E/P with the already-delivered energy taken out.
//
// Standing losses (~5 W) are ignored, which counts repayment as slightly faster than it is. That is the safe
// direction: recovery ends a shade early rather than parking the boiler hot.
#pragma once

// Matches THERMAL_BANK_WATER_SPECIFIC_HEAT and Heater::WATER_SPECIFIC_HEAT so every end of the compensation uses one
// number.
constexpr float THERMAL_RECOVERY_WATER_SPECIFIC_HEAT = 4.18f; // J/(g·°C), density taken as 1 g/ml

struct ThermalRecoveryParams {
    float setpointC = 0.0f;           // boiler setpoint actually commanded, temperature offset included
    float inletC = 0.0f;              // water temperature entering the boiler
    float heaterWatts = 0.0f;         // element rating
    float boilerCapacityJPerK = 0.0f; // effective heat capacity of boiler, group and water
    float maxBoostK = 0.0f;           // user's cap on how far above setpoint recovery may ask for; 0 disables it
    float ceilingC = 0.0f;            // absolute boiler temperature limit
    float deadbandK = 0.0f;           // debts worth less than this in temperature are treated as settled
    float maxDebtS = 0.0f;            // debt cap, expressed as seconds of full element output
};

// Tracks the boiler's energy debt across a shot. Holds only the debt, so parameters are passed per call and a settings
// change takes effect on the next sample rather than on the next shot.
class ThermalRecovery {
  public:
    // Forget the debt. Call when the machine leaves brew mode, loses the controller, or otherwise stops being able to
    // account for the energy it is tracking -- a stale debt would park the boiler high for no reason.
    void reset() { debt = 0.0f; }

    // Fold one interval into the balance. flowMlPerS and heaterDutyPercent are the latest telemetry; dtS is the time
    // they stood for. Zero-order hold, which is what sampled telemetry supports.
    void sample(float dtS, float flowMlPerS, float heaterDutyPercent, const ThermalRecoveryParams &params) {
        if (dtS <= 0.0f || !usable(params)) {
            debt = 0.0f;
            return;
        }

        const float deltaT = params.setpointC - params.inletC;
        if (flowMlPerS > 0.0f && deltaT > 0.0f) {
            debt += flowMlPerS * dtS * THERMAL_RECOVERY_WATER_SPECIFIC_HEAT * deltaT;
        }

        float duty = heaterDutyPercent / 100.0f;
        if (duty < 0.0f) {
            duty = 0.0f;
        } else if (duty > 1.0f) {
            duty = 1.0f;
        }
        debt -= params.heaterWatts * duty * dtS;

        if (debt < 0.0f) {
            debt = 0.0f; // square, not in credit
        }
        // Bound what a stuck flow reading can accumulate, so a sensor fault cannot park the boiler hot indefinitely.
        const float cap = params.maxDebtS > 0.0f ? params.heaterWatts * params.maxDebtS : 0.0f;
        if (cap > 0.0f && debt > cap) {
            debt = cap;
        }
    }

    // Offset to add to the setpoint, in K. Zero once the debt is settled, so the PID owns the temperature again.
    float offsetK(const ThermalRecoveryParams &params) const {
        if (!usable(params)) {
            return 0.0f;
        }
        float boost = debt / params.boilerCapacityJPerK;
        if (boost <= params.deadbandK) {
            return 0.0f;
        }
        if (boost > params.maxBoostK) {
            boost = params.maxBoostK;
        }
        if (params.ceilingC > 0.0f && params.setpointC + boost > params.ceilingC) {
            boost = params.ceilingC - params.setpointC;
        }
        return boost > 0.0f ? boost : 0.0f;
    }

    float debtJ() const { return debt; }

  private:
    // Every parameter has to be present for the balance to mean anything: without a heater rating there is no
    // repayment term, and without a heat capacity the debt cannot be turned back into a temperature.
    static bool usable(const ThermalRecoveryParams &params) {
        return params.maxBoostK > 0.0f && params.heaterWatts > 0.0f && params.boilerCapacityJPerK > 0.0f;
    }

    float debt = 0.0f; // J the boiler is short of its setpoint, by the measured balance
};
