// Fits BoilerModel.h to a real machine from its own shot logs, with a joint extended Kalman filter: the boiler's two
// states and the three parameters are estimated together, the parameters as random-walk states with a tiny step.
//
//   x = [ T, Ts, ln C, ln UA, ln tau ]       measurement: z = Ts
//
// Logs of the parameters rather than the parameters, so a multiplicative step is a constant step and no update can
// drive a heat capacity negative.
//
// WHY A FILTER AND NOT A CURVE FIT. For fitting three numbers to one file offline, least squares would be simpler and
// slightly better. The filter is here for what it reports alongside the answer: the posterior covariance says how much
// the data actually pinned each parameter down. That matters a great deal here, because the three are not equally
// identifiable from a shot, and a fit that returns three confident-looking numbers would be misleading.
//
//   C   is pinned down well. It is the sag: how far the temperature falls for a known energy deficit, which is exactly
//       what a high-flow phase measures. This is the number that goes in the web UI's Boiler Heat Capacity field.
//   tau is pinned down reasonably. It is the delay between flow starting and the sensor moving.
//   UA  is barely identifiable from a shot at all, and the filter will say so by leaving its sigma near the prior. At
//       ~1 W/K it contributes ~90 W against a 900 W element -- a 10 % correction on the dominant term, which a 20 s
//       window cannot separate from a slightly different C. Identifying it needs an idle stretch or a warm-up, where
//       standing loss is the only term left. Reporting it with a wide sigma is the filter being honest, not failing.
//
// WHERE THE DUTY COMES FROM. The shot log records target and current temperature, pressures, flows and weights -- but
// not the heater duty, so the element's contribution is not in the file. What rescues the fit is that during the phases
// that matter the duty is not in doubt: the disturbance feedforward in Heater.cpp asks for flow*c*(T - inlet) watts, so
// once the flow needs more than the element can deliver the output is pinned at 100 % no matter what the PID thinks.
// That threshold is boilerModelMaxSustainableFlow(), a quantity this same model already provides.
//
// So the filter learns only from samples where the element is confidently saturated, and the caller says which those
// are. Everywhere else it resets the state to the measurement instead of propagating, which costs nothing (those
// stretches carry no information about C anyway) and means every saturated window starts from the truth rather than
// from whatever the filter had drifted to.
#pragma once

#include "BoilerModel.h"

#include <cmath>
#include <cstddef>

struct BoilerIdentifierConfig {
    BoilerModelParams prior;        // starting guesses; heaterWatts, ambientC and inletC are taken as known
    float capacitySigma0 = 1500.0f; // prior 1-sigma on C, J/K -- deliberately loose
    // Tight on purpose. A shot cannot separate standing loss from heat capacity -- both slow the sag -- so left free
    // it absorbs the capacity error instead of converging, and a guess 2.7x too high comes back as a wrong capacity
    // with a plausible-looking loss beside it. Held near the prior, the capacity has to account for the sag itself.
    // Widen it only for a log that can actually see standing loss: an idle stretch or a warm-up, where it is the only
    // term left.
    float lossSigma0 = 0.15f; // prior 1-sigma on UA, W/K
    float tauSigma0 = 2.5f;   // prior 1-sigma on tau, s
    float measurementSigmaC = 0.3f; // thermocouple noise; the log quantises to 0.1 °C and the sensor adds more
    // Model error on the boiler node, as a rate: the variance added per step is (stateSigmaCPerS * dt)^2. Per-rate
    // rather than per-sample because that is what it is -- the one-node model drifts from a real boiler at some rate,
    // not by some amount each time a sample happens to arrive. Stated per sample it would also have to be retuned for
    // every sampling interval, and at 100 ms a per-sample figure large enough to look reasonable (0.1 K) swamps the
    // 0.02 K per-step signal the parameters have to be learned from, so the state absorbs everything and nothing is
    // learned.
    float stateSigmaCPerS = 0.05f;
    // Per-sample log-parameter step. Small, because the parameters being estimated are constants of the machine, not
    // quantities that move during a shot: the walk is here to keep the filter from closing itself off to further
    // evidence, not to track anything. Larger values adapt faster and trust the data less.
    float paramRandomWalk = 0.0008f;
};

struct BoilerIdentifierResult {
    BoilerModelParams params;      // posterior estimate
    float capacitySigma = 0.0f;    // 1-sigma, J/K
    float lossSigma = 0.0f;        // 1-sigma, W/K
    float tauSigma = 0.0f;         // 1-sigma, s
    size_t samplesLearned = 0;     // samples the filter actually learned from
    size_t samplesSeen = 0;        // samples offered
};

class BoilerIdentifier {
  public:
    static constexpr size_t N = 5;
    static constexpr size_t I_T = 0, I_TS = 1, I_LN_C = 2, I_LN_UA = 3, I_LN_TAU = 4;

    explicit BoilerIdentifier(const BoilerIdentifierConfig &config) : cfg(config) {
        known = config.prior;
        x[I_LN_C] = std::log(positive(config.prior.capacityJPerK, 3500.0f));
        x[I_LN_UA] = std::log(positive(config.prior.lossWPerK, 1.0f));
        x[I_LN_TAU] = std::log(positive(config.prior.sensorTauS, 4.0f));
        beginPass();
    }

    // Starts a fresh sweep over the data, keeping the parameter estimates and discarding everything else.
    //
    // Running the filter over a log more than once is what makes it usable offline. A single sweep converges from
    // below but crawls from above, because the heat capacity's sensitivity goes as 1/C: a guess twice too high has
    // half the gradient pulling it back. Restarting from the posterior means the second sweep sees the same data from
    // a better point, which is iterated estimation rather than a trick -- the data is unchanged, only the linearisation
    // point moves.
    //
    // The parameter covariance is reset to the prior too, not carried over. Sweeping the same samples repeatedly would
    // otherwise count their information once per sweep and report a confidence interval several times tighter than one
    // shot's worth of data can support. Resetting it keeps the reported sigma meaning what it says: what a single pass
    // over this log establishes, from this starting point.
    void beginPass() {
        // Temperatures are left at zero and seeded from the first measurement instead. Starting them at a guess --
        // ambient, say -- hands the first update a residual the size of the whole warm-up, and the filter explains
        // that by slamming the sensor lag to its bound, from which neither it nor the heat capacity recovers.
        x[I_T] = 0.0f;
        x[I_TS] = 0.0f;
        seeded = false;
        learned = 0;
        seen = 0;

        for (size_t i = 0; i < N; i++) {
            for (size_t j = 0; j < N; j++) {
                P[i][j] = 0.0f;
            }
        }
        // A temperature known to measurement accuracy at the start, and the parameter priors as given. Converting a
        // 1-sigma on the parameter to one on its log is the relative uncertainty, sigma/value.
        P[I_T][I_T] = cfg.measurementSigmaC * cfg.measurementSigmaC;
        P[I_TS][I_TS] = cfg.measurementSigmaC * cfg.measurementSigmaC;
        P[I_LN_C][I_LN_C] = relativeVariance(cfg.capacitySigma0, cfg.prior.capacityJPerK);
        P[I_LN_UA][I_LN_UA] = relativeVariance(cfg.lossSigma0, cfg.prior.lossWPerK);
        P[I_LN_TAU][I_LN_TAU] = relativeVariance(cfg.tauSigma0, cfg.prior.sensorTauS);
    }

    // One sample. measuredC is the logged sensor temperature, flowMlPerS the logged pump flow, dutyFraction the
    // element duty over the interval, and dutyKnown whether that figure can be trusted. When it cannot, the state is
    // snapped to the measurement and nothing is learned -- see the note on duty above.
    void observe(float dtS, float measuredC, float flowMlPerS, float dutyFraction, bool dutyKnown) {
        seen++;
        if (dtS <= 0.0f) {
            return;
        }
        if (!dutyKnown || !seeded) {
            // Either the element's state is unknown over this interval, or this is the first sample and the filter has
            // no temperature yet. Both cases take the measurement as the truth and learn nothing from it.
            x[I_T] = measuredC;
            x[I_TS] = measuredC;
            seeded = true;
            return;
        }

        predict(dtS, dutyFraction, flowMlPerS < 0.0f ? 0.0f : flowMlPerS);
        update(measuredC);
        clampParameters();
        learned++;
    }

    BoilerIdentifierResult result() const {
        BoilerIdentifierResult out;
        out.params = known;
        out.params.capacityJPerK = std::exp(x[I_LN_C]);
        out.params.lossWPerK = std::exp(x[I_LN_UA]);
        out.params.sensorTauS = std::exp(x[I_LN_TAU]);
        // Back from log-space: sigma on the parameter is the parameter times the sigma on its log.
        out.capacitySigma = out.params.capacityJPerK * std::sqrt(nonNegative(P[I_LN_C][I_LN_C]));
        out.lossSigma = out.params.lossWPerK * std::sqrt(nonNegative(P[I_LN_UA][I_LN_UA]));
        out.tauSigma = out.params.sensorTauS * std::sqrt(nonNegative(P[I_LN_TAU][I_LN_TAU]));
        out.samplesLearned = learned;
        out.samplesSeen = seen;
        return out;
    }

    // Whether the element is certainly saturated at this flow and temperature, with a margin so a sample sitting on
    // the boundary -- where the duty is genuinely in doubt -- is left out rather than guessed at.
    static bool elementSaturated(const BoilerModelParams &params, float boilerC, float flowMlPerS, float marginFactor) {
        const float ceiling = boilerModelMaxSustainableFlow(params, boilerC);
        return ceiling > 0.0f && flowMlPerS > ceiling * marginFactor;
    }

  private:
    static float positive(float value, float fallback) { return value > 0.0f ? value : fallback; }
    static float nonNegative(float value) { return value > 0.0f ? value : 0.0f; }

    static float relativeVariance(float sigma, float value) {
        if (value <= 0.0f || sigma <= 0.0f) {
            return 1.0f; // no usable prior: 100 % relative uncertainty
        }
        const float relative = sigma / value;
        return relative * relative;
    }

    // Propagate the state through one BoilerModel step and the covariance through its Jacobian.
    void predict(float dtS, float duty, float flow) {
        const float C = std::exp(x[I_LN_C]);
        const float UA = std::exp(x[I_LN_UA]);
        const float tau = std::exp(x[I_LN_TAU]);
        const float T = x[I_T];
        const float Ts = x[I_TS];

        if (duty < 0.0f) {
            duty = 0.0f;
        } else if (duty > 1.0f) {
            duty = 1.0f;
        }

        const float carried = flow * BOILER_MODEL_WATER_SPECIFIC_HEAT * (T - known.inletC);
        const float lost = UA * (T - known.ambientC);
        const float net = known.heaterWatts * duty - carried - lost;
        const float decay = std::exp(-dtS / tau);
        const float alpha = 1.0f - decay;

        float F[N][N] = {{0.0f}};
        for (size_t i = 0; i < N; i++) {
            F[i][i] = 1.0f;
        }
        // d(T')/dT: both loss terms pull the boiler back towards where the water and the room are.
        F[I_T][I_T] = 1.0f - dtS * (flow * BOILER_MODEL_WATER_SPECIFIC_HEAT + UA) / C;
        // A larger C turns the same net power into less temperature change, hence the sign.
        F[I_T][I_LN_C] = -dtS * net / C;
        F[I_T][I_LN_UA] = -dtS * UA * (T - known.ambientC) / C;
        F[I_T][I_TS] = 0.0f;
        // The sensor chases the boiler at a rate the lag sets.
        F[I_TS][I_T] = alpha;
        F[I_TS][I_TS] = decay;
        F[I_TS][I_LN_TAU] = -(T - Ts) * decay * dtS / tau;

        // Advance the state with the shared model so the filter and the simulator cannot drift apart.
        BoilerModelState state;
        state.boilerC = T;
        state.sensorC = Ts;
        BoilerModelParams step = known;
        step.capacityJPerK = C;
        step.lossWPerK = UA;
        step.sensorTauS = tau;
        boilerModelStep(state, step, dtS, duty, flow);
        x[I_T] = state.boilerC;
        x[I_TS] = state.sensorC;

        // P = F P F' + Q
        float FP[N][N];
        for (size_t i = 0; i < N; i++) {
            for (size_t j = 0; j < N; j++) {
                float sum = 0.0f;
                for (size_t k = 0; k < N; k++) {
                    sum += F[i][k] * P[k][j];
                }
                FP[i][j] = sum;
            }
        }
        for (size_t i = 0; i < N; i++) {
            for (size_t j = 0; j < N; j++) {
                float sum = 0.0f;
                for (size_t k = 0; k < N; k++) {
                    sum += FP[i][k] * F[j][k];
                }
                P[i][j] = sum;
            }
        }
        const float stateStep = cfg.stateSigmaCPerS * dtS;
        const float stateQ = stateStep * stateStep;
        const float paramQ = cfg.paramRandomWalk * cfg.paramRandomWalk;
        P[I_T][I_T] += stateQ;
        P[I_TS][I_TS] += stateQ;
        P[I_LN_C][I_LN_C] += paramQ;
        P[I_LN_UA][I_LN_UA] += paramQ;
        P[I_LN_TAU][I_LN_TAU] += paramQ;
    }

    // Scalar measurement, so no matrix inverse: the whole update is divisions by one innovation variance.
    void update(float measuredC) {
        const float S = P[I_TS][I_TS] + cfg.measurementSigmaC * cfg.measurementSigmaC;
        if (S <= 0.0f) {
            return;
        }
        float K[N];
        for (size_t i = 0; i < N; i++) {
            K[i] = P[i][I_TS] / S;
        }
        const float residual = measuredC - x[I_TS];
        for (size_t i = 0; i < N; i++) {
            x[i] += K[i] * residual;
        }
        // P = (I - K H) P, with H selecting the sensor row.
        float row[N];
        for (size_t j = 0; j < N; j++) {
            row[j] = P[I_TS][j];
        }
        for (size_t i = 0; i < N; i++) {
            for (size_t j = 0; j < N; j++) {
                P[i][j] -= K[i] * row[j];
            }
        }
        // Keep it symmetric: the asymmetry the form above accumulates is what eventually makes a filter diverge.
        for (size_t i = 0; i < N; i++) {
            for (size_t j = i + 1; j < N; j++) {
                const float mean = 0.5f * (P[i][j] + P[j][i]);
                P[i][j] = mean;
                P[j][i] = mean;
            }
        }
    }

    // Physical bounds, as a backstop rather than a tuning knob: a filter pushed outside these by a bad log should be
    // held at the edge, where the reported sigma shows something went wrong, instead of producing nonsense.
    void clampParameters() {
        clampLog(x[I_LN_C], 500.0f, 20000.0f);
        clampLog(x[I_LN_UA], 0.05f, 20.0f);
        clampLog(x[I_LN_TAU], 0.2f, 60.0f);
    }

    static void clampLog(float &logValue, float low, float high) {
        const float lowLog = std::log(low);
        const float highLog = std::log(high);
        if (!(logValue == logValue)) { // NaN, from a log with no usable data behind it
            logValue = lowLog;
        } else if (logValue < lowLog) {
            logValue = lowLog;
        } else if (logValue > highLog) {
            logValue = highLog;
        }
    }

    BoilerIdentifierConfig cfg;
    BoilerModelParams known; // heaterWatts, ambientC, inletC: taken as given, not estimated
    float x[N] = {0.0f};
    float P[N][N] = {{0.0f}};
    bool seeded = false;  // whether a measurement has supplied the starting temperature
    size_t learned = 0;
    size_t seen = 0;
};

// Open-loop replay: runs the fitted model over a log without ever correcting it from the measurements and returns the
// RMS error against them. This, not the filter's own residuals, is the test of whether the parameters predict the
// machine -- a filter that is being corrected every sample tracks almost anything.
struct BoilerLogSample {
    float dtS = 0.0f;
    float measuredC = 0.0f;
    float flowMlPerS = 0.0f;
    float dutyFraction = 0.0f;
    bool dutyKnown = false;
};

struct BoilerReplayResult {
    float rmsErrorC = 0.0f;
    float maxErrorC = 0.0f;
    size_t samples = 0;
};

// Replays from the first sample whose duty is known, which is where the model has something to predict from, and
// re-seeds whenever the duty becomes unknown again: a stretch the model is not told the element's state over cannot be
// predicted through, only resumed after.
inline BoilerReplayResult boilerReplay(const BoilerLogSample *samples, size_t count, const BoilerModelParams &params) {
    BoilerReplayResult out;
    if (samples == nullptr || count == 0) {
        return out;
    }

    BoilerModelState state;
    bool seeded = false;
    float sumSquared = 0.0f;

    for (size_t i = 0; i < count; i++) {
        const BoilerLogSample &s = samples[i];
        if (!s.dutyKnown) {
            seeded = false;
            continue;
        }
        if (!seeded) {
            state.boilerC = s.measuredC;
            state.sensorC = s.measuredC;
            seeded = true;
            continue; // nothing predicted yet, so nothing to score
        }

        boilerModelStep(state, params, s.dtS, s.dutyFraction, s.flowMlPerS);
        const float error = state.sensorC - s.measuredC;
        const float magnitude = error < 0.0f ? -error : error;
        sumSquared += error * error;
        if (magnitude > out.maxErrorC) {
            out.maxErrorC = magnitude;
        }
        out.samples++;
    }

    if (out.samples > 0) {
        out.rmsErrorC = std::sqrt(sumSquared / static_cast<float>(out.samples));
    }
    return out;
}
