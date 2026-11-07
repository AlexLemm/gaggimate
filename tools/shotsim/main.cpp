// shotsim -- fits the boiler model to a machine from its own shot logs, then says what the fitted model implies.
//
// The simulator in sim/ shows the UI running against a made-up machine. This shows the *physics* running against the
// real one: it reads .slog files off the device, identifies the boiler's parameters with the Kalman filter in
// BoilerIdentifier.h, replays the shot open-loop to say how well those parameters predict it, and then works out what
// the compensations in ThermalBank.h and ThermalRecovery.h would do with them. That last part is the point -- it turns
// "set the thermal bank to 2 and see how the coffee tastes" into a number derived from the machine's own measurements.
//
// Build and run:
//   pio run -e shotsim
//   .pio/build/shotsim/program --watts 900 --offset 5 shots/*.slog
//
// Get the logs off the machine from the shot history in the web UI, or straight from its filesystem under /h.
//
// What it cannot do: the shot log does not record heater duty, so the element's contribution has to be inferred. That
// is done two ways here, and the second is the better one.
//
//   - For the one-node filter, only samples whose duty is CERTAIN are used, which is where the controller's
//     feedforward provably saturates the output. See BoilerIdentifier::dutyIsCertainlyFull.
//   - For the two-mass fit, the whole duty trace is reconstructed by running the controller's own law over the
//     measured temperatures (HeaterControlModel.h). That is legitimate because the reconstruction is driven by what
//     the sensor reported, not by the model being fitted, so one trace scores every candidate -- and it uses every
//     sample rather than a handful of saturated windows.
//
// Why there are two models at all: the one-node model cannot explain the author's own logs, where the sensor rises
// through a 37 ml fill that removes 11 kJ. See the note at the top of BoilerTwoMass.h. The one-node fit is kept
// because the shipped compensations reason with it, so knowing how badly it fits is worth printing.
#include <display/core/constants.h>
#include <display/models/shot_log_format.h>
#include <display/util/BoilerIdentifier.h>
#include <display/util/BoilerModel.h>
#include <display/util/BoilerTwoMass.h>
#include <display/util/HeaterControlModel.h>
#include <display/util/ThermalBank.h>
#include <display/util/ThermalRecovery.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
    float heaterWatts = 900.0f;
    float inletC = static_cast<float>(DEFAULT_INLET_WATER_TEMP);
    float ambientC = static_cast<float>(DEFAULT_INLET_WATER_TEMP);
    float temperatureOffsetC = 0.0f;
    float capacityPrior = static_cast<float>(DEFAULT_BOILER_HEAT_CAPACITY);
    float lossPrior = 1.2f;
    float tauPrior = 4.0f;
    // The margin on the saturation test. 1.5 means the feedforward must ask for half again more than the output span
    // before the duty is called certain, which leaves 500 units of room for the proportional and derivative terms to
    // subtract without unpinning the element.
    float saturationMargin = 1.5f;
    int passes = 6;
    // The controller's gains, needed to reconstruct the duty. Defaults are the author's machine; read them off the
    // Machine tab. Kff is derived from --watts unless given, which keeps the one-number rule the UI enforces.
    float kp = 29.0f;
    float ki = 0.11f;
    float kd = 379.5f;
    float kff = 0.0f;
    // Stratified starting points. Only the two cell capacities and the mixing are fitted; the loss and the lag stay
    // where they start, for the same reason as in the one-node fit -- a shot cannot see them.
    float inletCellPrior = 1000.0f;
    float outletCellPrior = 1000.0f;
    float mixingPrior = 60.0f;
    std::string csvPath;
    std::vector<std::string> logs;
};

struct Shot {
    std::string name;
    std::string profile;
    std::vector<BoilerLogSample> samples;
    std::vector<BoilerTwoMassSample> twoMass; // same samples, with the reconstructed duty on every one
    float peakFlow = 0.0f;
    float startC = 0.0f;
    float minC = 0.0f;
    float targetC = 0.0f;
    float waterMl = 0.0f;
    float durationS = 0.0f;
    size_t saturated = 0;
};

HeaterControlParams controlFrom(const Options &opt) {
    HeaterControlParams c;
    c.kp = opt.kp;
    c.ki = opt.ki;
    c.kd = opt.kd;
    c.kff = opt.kff > 0.0f ? opt.kff : (opt.heaterWatts > 0.0f ? HEATER_OUTPUT_SPAN / opt.heaterWatts : 0.0f);
    c.inletC = opt.inletC;
    return c;
}

void usage() {
    std::printf("usage: shotsim [options] <shot.slog> [more.slog ...]\n"
                "\n"
                "  --watts <W>      element rating; measure it at the plug rather than trusting a label (default 900)\n"
                "  --offset <C>     the machine's temperature offset, so logged temperatures become real ones\n"
                "  --inlet <C>      tank water temperature (default %d)\n"
                "  --ambient <C>    room temperature (default %d)\n"
                "  --capacity <J/K> starting guess for the boiler heat capacity (default %d)\n"
                "  --loss <W/K>     starting guess for standing loss; a shot cannot identify this (default 1.2)\n"
                "  --tau <s>        starting guess for the sensor lag (default 4)\n"
                "  --margin <x>     how far past the output span the feedforward must ask before the duty is taken as\n"
                "                   certain (default 1.5)\n"
                "  --passes <n>     sweeps over the data; more makes the answer independent of the guess (default 6)\n"
                "  --pid <p,i,d>    the machine's PID gains, to reconstruct the duty (default 29,0.11,379.5)\n"
                "  --kff <u/W>      feedforward gain; defaults to 1000 / --watts, as the UI derives it\n"
                "  --cbottom <J/K>  starting guess for the lower boiler half's heat capacity (default 1000)\n"
                "  --ctop <J/K>     starting guess for the upper half's, which carries the sensor (default 1000)\n"
                "  --mixing <W/K>   starting guess for convective mixing between the halves (default 60)\n"
                "  --csv <path>     write measured and predicted temperatures for plotting\n",
                DEFAULT_INLET_WATER_TEMP, DEFAULT_INLET_WATER_TEMP, DEFAULT_BOILER_HEAT_CAPACITY);
}

bool parseArgs(int argc, char **argv, Options &out) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "-h" || arg == "--help") {
            return false;
        } else if (arg == "--watts" && hasValue) {
            out.heaterWatts = std::stof(argv[++i]);
        } else if (arg == "--offset" && hasValue) {
            out.temperatureOffsetC = std::stof(argv[++i]);
        } else if (arg == "--inlet" && hasValue) {
            out.inletC = std::stof(argv[++i]);
        } else if (arg == "--ambient" && hasValue) {
            out.ambientC = std::stof(argv[++i]);
        } else if (arg == "--capacity" && hasValue) {
            out.capacityPrior = std::stof(argv[++i]);
        } else if (arg == "--loss" && hasValue) {
            out.lossPrior = std::stof(argv[++i]);
        } else if (arg == "--tau" && hasValue) {
            out.tauPrior = std::stof(argv[++i]);
        } else if (arg == "--margin" && hasValue) {
            out.saturationMargin = std::stof(argv[++i]);
        } else if (arg == "--passes" && hasValue) {
            out.passes = std::atoi(argv[++i]);
        } else if (arg == "--pid" && hasValue) {
            // Same "Kp,Ki,Kd" the Machine tab shows, so it can be pasted straight across.
            if (std::sscanf(argv[++i], "%f,%f,%f", &out.kp, &out.ki, &out.kd) != 3) {
                std::fprintf(stderr, "--pid wants three comma-separated numbers, as on the Machine tab\n");
                return false;
            }
        } else if (arg == "--kff" && hasValue) {
            out.kff = std::stof(argv[++i]);
        } else if (arg == "--cbottom" && hasValue) {
            out.inletCellPrior = std::stof(argv[++i]);
        } else if (arg == "--ctop" && hasValue) {
            out.outletCellPrior = std::stof(argv[++i]);
        } else if (arg == "--mixing" && hasValue) {
            out.mixingPrior = std::stof(argv[++i]);
        } else if (arg == "--csv" && hasValue) {
            out.csvPath = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return false;
        } else {
            out.logs.push_back(arg);
        }
    }
    return !out.logs.empty();
}

BoilerModelParams priorFrom(const Options &opt) {
    BoilerModelParams p;
    p.capacityJPerK = opt.capacityPrior;
    p.lossWPerK = opt.lossPrior;
    p.sensorTauS = opt.tauPrior;
    p.heaterWatts = opt.heaterWatts;
    p.ambientC = opt.ambientC;
    p.inletC = opt.inletC;
    return p;
}

// Reads one .slog. The sample layout is fixed and the header states its own size, so a log written by an older
// firmware still reads -- shotLogSampleSize returns 0 for one this build cannot interpret.
bool readShot(const std::string &path, const Options &opt, const HeaterControlParams &control, Shot &out) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "%s: cannot open\n", path.c_str());
        return false;
    }

    ShotLogHeader header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 || header.magic != SHOT_LOG_MAGIC) {
        std::fprintf(stderr, "%s: not a shot log\n", path.c_str());
        std::fclose(file);
        return false;
    }
    const uint8_t sampleSize = shotLogSampleSize(header);
    if (sampleSize == 0) {
        std::fprintf(stderr, "%s: unsupported layout (version %u, record %u)\n", path.c_str(), header.version,
                     header.reserved0);
        std::fclose(file);
        return false;
    }
    if (std::fseek(file, header.headerSize, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }

    out.name = path;
    out.profile.assign(header.profileName, strnlen(header.profileName, sizeof(header.profileName)));

    uint32_t previousMs = 0;
    bool first = true;
    // One controller, stepped alongside the log, so the duty it reports carries the integral and derivative history a
    // per-sample calculation would throw away.
    HeaterControlState heater;
    for (uint32_t i = 0; i < header.sampleCount; i++) {
        uint8_t raw[SHOT_LOG_SAMPLE_SIZE] = {0};
        if (std::fread(raw, sampleSize, 1, file) != 1) {
            break;
        }
        ShotLogSample record{};
        std::memcpy(&record, raw, sampleSize < sizeof(record) ? sampleSize : sizeof(record));

        // Logged temperatures have the machine's offset already taken out (Controller::onTempRead), so putting it back
        // is what turns them into the temperature the boiler is physically at -- which is what the model is about.
        const float measuredC = record.ct / 10.0f + opt.temperatureOffsetC;
        const float targetC = record.tt / 10.0f + opt.temperatureOffsetC;
        const float flow = record.fl / 100.0f;
        const uint32_t elapsed = shotLogHasElapsedTimestamp(header) ? record.t : i * header.sampleInterval;

        BoilerLogSample sample;
        sample.dtS = first ? header.sampleInterval / 1000.0f : (elapsed - previousMs) / 1000.0f;
        sample.measuredC = measuredC;
        sample.flowMlPerS = flow > 0.0f ? flow : 0.0f;
        // Whether the controller's feedforward provably had the element pinned. See
        // BoilerIdentifier::dutyIsCertainlyFull for why this asks about the controller and not about the physics.
        sample.dutyKnown =
            BoilerIdentifier::dutyIsCertainlyFull(control, measuredC, targetC, sample.flowMlPerS, opt.saturationMargin);
        sample.dutyFraction = sample.dutyKnown ? 1.0f : 0.0f;

        if (first) {
            // The machine has been idling at its setpoint, so the integral is already holding whatever the standing
            // loss costs. Starting it at zero would open every shot with the element off.
            BoilerModelParams idle;
            idle.heaterWatts = opt.heaterWatts;
            idle.lossWPerK = opt.lossPrior;
            idle.ambientC = opt.ambientC;
            idle.inletC = opt.inletC;
            heaterSeedIntegral(heater, control, boilerModelRequiredDuty(idle, measuredC, 0.0f));
        }
        // The duty the controller would have commanded, from the temperature it actually saw. The valve is open for
        // the whole of a shot, so flow alone decides whether the feedforward is in play.
        BoilerTwoMassSample wide;
        wide.dtS = sample.dtS;
        wide.measuredC = measuredC;
        wide.flowMlPerS = sample.flowMlPerS;
        wide.dutyFraction =
            heaterControlStep(heater, control, sample.dtS, measuredC, targetC, sample.flowMlPerS, true);

        if (sample.dtS > 0.0f && sample.dtS < 5.0f) {
            out.twoMass.push_back(wide);
            out.samples.push_back(sample);
            out.waterMl += sample.flowMlPerS * sample.dtS;
            out.durationS += sample.dtS;
            if (sample.dutyKnown) {
                out.saturated++;
            }
            if (sample.flowMlPerS > out.peakFlow) {
                out.peakFlow = sample.flowMlPerS;
            }
            if (first || measuredC < out.minC) {
                out.minC = measuredC;
            }
            if (first) {
                out.startC = measuredC;
                out.targetC = targetC;
            }
        }
        previousMs = elapsed;
        first = false;
    }

    std::fclose(file);
    return !out.samples.empty();
}

// Prints a shot, and -- when nothing in it was usable -- which of the two conditions ruled it out. Naming the right
// one matters: a log whose flow never outran the element needs a different profile, while one that never got below its
// setpoint needs a machine that was not already too hot. Blaming the flow for a setpoint problem sent the first real
// run looking in entirely the wrong place, since the flow had peaked at 8 ml/s.
void describe(const Shot &shot, const HeaterControlParams &control, const Options &opt) {
    const float setpoint = shot.targetC > 0.0f ? shot.targetC : shot.startC;
    const float needed = heaterSaturatingFlow(control, setpoint, opt.saturationMargin);
    std::printf("  %-20s %5.1f s %6.1f ml  peak %4.2f ml/s  %5.1f -> %5.1f C (target %5.1f)  %zu/%zu at full duty\n",
                shot.profile.empty() ? "(unnamed)" : shot.profile.c_str(), shot.durationS, shot.waterMl, shot.peakFlow,
                shot.startC, shot.minC, shot.targetC, shot.saturated, shot.samples.size());

    if (shot.saturated > 0) {
        return;
    }
    // Which of the two conditions ruled it out, named rather than guessed at: a log whose flow never got there needs a
    // different profile, while one that was simply too hot needs the offset checked.
    if (shot.peakFlow < needed) {
        std::printf("    no certain duty: the flow never passed %.2f ml/s, which is where the feedforward alone\n"
                    "    saturates the output. Pull a shot with a faster fill phase.\n",
                    needed);
    } else {
        std::printf("    no certain duty: the flow was fast enough, but the sensor stayed more than 0.8 C above the\n"
                    "    setpoint throughout, where the safety scaling switches the feedforward off. A machine\n"
                    "    sitting that far over its setpoint is usually a temperature offset -- check --offset.\n");
    }
}

// ---------------------------------------------------------------------------
// Two-mass fit
// ---------------------------------------------------------------------------

struct TwoMassFit {
    BoilerTwoMassParams params;
    float rmsErrorC = 0.0f;
    float priorRmsErrorC = 0.0f;
    float minOutletC = 0.0f;     // coldest water the puck saw, which no machine can measure
    float sensorThenC = 0.0f;    // what the sensor was reading at that moment
    float minInletCellC = 0.0f;  // how cold the bottom of the boiler got
    int evaluations = 0;
};

BoilerTwoMassParams twoMassFrom(const Options &opt, double lnBottom, double lnTop, double lnMixing) {
    BoilerTwoMassParams p;
    p.inletCapacityJPerK = static_cast<float>(std::exp(lnBottom));
    p.outletCapacityJPerK = static_cast<float>(std::exp(lnTop));
    p.mixingWPerK = static_cast<float>(std::exp(lnMixing));
    p.lossWPerK = opt.lossPrior;
    p.sensorTauS = opt.tauPrior;
    p.heaterWatts = opt.heaterWatts;
    p.ambientC = opt.ambientC;
    p.inletC = opt.inletC;
    return p;
}

// RMS over every sample of every shot, pooled rather than averaged per shot, so a long shot counts for more than a
// short one.
float twoMassScore(const std::vector<Shot> &shots, const BoilerTwoMassParams &p, float workingC) {
    // Outside these a fit has stopped describing an espresso machine, and letting the search wander there buys a good
    // score with a number that means nothing.
    if (p.inletCapacityJPerK < 200.0f || p.inletCapacityJPerK > 12000.0f || p.outletCapacityJPerK < 200.0f ||
        p.outletCapacityJPerK > 12000.0f || p.mixingWPerK < 2.0f || p.mixingWPerK > 3000.0f) {
        return 1.0e6f;
    }
    // And the stored gradient the seeding implies has to be one a boiler could actually hold. Without this the search
    // flattens the sensor trace by putting the bottom of the boiler above boiling, which is how the first version of
    // this fit arrived at a 5 W/K coupling and water idling at 110 C.
    if (!boilerTwoMassPlausible(p, workingC)) {
        return 1.0e6f;
    }
    double sumSquared = 0.0;
    size_t count = 0;
    for (const auto &shot : shots) {
        const auto replay = boilerTwoMassReplay(shot.twoMass.data(), shot.twoMass.size(), p);
        sumSquared += static_cast<double>(replay.rmsErrorC) * replay.rmsErrorC * static_cast<double>(replay.samples);
        count += replay.samples;
    }
    if (count == 0) {
        return 1.0e6f;
    }
    const double rms = std::sqrt(sumSquared / static_cast<double>(count));
    return std::isfinite(rms) ? static_cast<float>(rms) : 1.0e6f;
}

// Nelder-Mead over the logs of the three fitted parameters.
//
// A simplex search rather than another Kalman filter, and deliberately so. The filter in BoilerIdentifier.h earns its
// place by reporting a covariance, but it needs the duty to be certain, which confines it to the saturated windows. A
// direct search on the open-loop replay error uses the whole shot and optimises exactly the number worth judging the
// model by. What it gives up is the error bars, so what it prints below is a score, not a measurement with a sigma.
TwoMassFit fitTwoMass(const std::vector<Shot> &shots, const Options &opt) {
    TwoMassFit out;
    // The temperature the plausibility check is applied at: where this machine actually sits while it works, since
    // that is where the stored gradient has to be one a boiler could hold.
    const float workingC = shots.front().targetC > 0.0f ? shots.front().targetC : shots.front().startC;
    const double start[3] = {std::log(opt.inletCellPrior), std::log(opt.outletCellPrior), std::log(opt.mixingPrior)};
    out.priorRmsErrorC = twoMassScore(shots, twoMassFrom(opt, start[0], start[1], start[2]), workingC);

    // Four vertices for three dimensions, the extras offset by a factor of e^0.4 (about 1.5x) on one axis each.
    double simplex[4][3];
    double score[4];
    for (int v = 0; v < 4; v++) {
        for (int d = 0; d < 3; d++) {
            simplex[v][d] = start[d] + (v - 1 == d ? 0.4 : 0.0);
        }
        score[v] = twoMassScore(shots, twoMassFrom(opt, simplex[v][0], simplex[v][1], simplex[v][2]), workingC);
        out.evaluations++;
    }

    for (int iteration = 0; iteration < 400; iteration++) {
        int best = 0, worst = 0, second = 0;
        for (int v = 1; v < 4; v++) {
            if (score[v] < score[best]) {
                best = v;
            }
            if (score[v] > score[worst]) {
                worst = v;
            }
        }
        for (int v = 0; v < 4; v++) {
            if (v != worst && score[v] > score[second == worst ? best : second]) {
                second = v;
            }
        }
        // Converged once the simplex has collapsed: 0.002 in log space is 0.2 % on a parameter, far finer than the
        // data can justify.
        double spread = 0.0;
        for (int d = 0; d < 3; d++) {
            spread += std::fabs(simplex[worst][d] - simplex[best][d]);
        }
        if (spread < 0.002) {
            break;
        }

        double centroid[3] = {0.0, 0.0, 0.0};
        for (int v = 0; v < 4; v++) {
            if (v == worst) {
                continue;
            }
            for (int d = 0; d < 3; d++) {
                centroid[d] += simplex[v][d] / 3.0;
            }
        }

        double trial[3];
        for (int d = 0; d < 3; d++) {
            trial[d] = centroid[d] + (centroid[d] - simplex[worst][d]); // reflect
        }
        float trialScore = twoMassScore(shots, twoMassFrom(opt, trial[0], trial[1], trial[2]), workingC);
        out.evaluations++;

        if (trialScore < score[best]) {
            double expanded[3];
            for (int d = 0; d < 3; d++) {
                expanded[d] = centroid[d] + 2.0 * (centroid[d] - simplex[worst][d]);
            }
            const float expandedScore = twoMassScore(shots, twoMassFrom(opt, expanded[0], expanded[1], expanded[2]), workingC);
            out.evaluations++;
            if (expandedScore < trialScore) {
                for (int d = 0; d < 3; d++) {
                    trial[d] = expanded[d];
                }
                trialScore = expandedScore;
            }
        } else if (trialScore >= score[second]) {
            for (int d = 0; d < 3; d++) {
                trial[d] = centroid[d] + 0.5 * (simplex[worst][d] - centroid[d]); // contract
            }
            trialScore = twoMassScore(shots, twoMassFrom(opt, trial[0], trial[1], trial[2]), workingC);
            out.evaluations++;
            if (trialScore >= score[worst]) {
                // Shrink everything toward the best vertex.
                for (int v = 0; v < 4; v++) {
                    if (v == best) {
                        continue;
                    }
                    for (int d = 0; d < 3; d++) {
                        simplex[v][d] = simplex[best][d] + 0.5 * (simplex[v][d] - simplex[best][d]);
                    }
                    score[v] = twoMassScore(shots, twoMassFrom(opt, simplex[v][0], simplex[v][1], simplex[v][2]), workingC);
                    out.evaluations++;
                }
                continue;
            }
        }
        for (int d = 0; d < 3; d++) {
            simplex[worst][d] = trial[d];
        }
        score[worst] = trialScore;
    }

    int best = 0;
    for (int v = 1; v < 4; v++) {
        if (score[v] < score[best]) {
            best = v;
        }
    }
    out.params = twoMassFrom(opt, simplex[best][0], simplex[best][1], simplex[best][2]);
    out.rmsErrorC = score[best];

    // Where the brew water actually got, which is the number this model exists to produce.
    out.minOutletC = 1.0e6f;
    out.minInletCellC = 1.0e6f;
    for (const auto &shot : shots) {
        BoilerTwoMassState state = boilerTwoMassSeedIdle(out.params, shot.twoMass.front().measuredC);
        for (const auto &s : shot.twoMass) {
            boilerTwoMassStep(state, out.params, s.dtS, s.dutyFraction, s.flowMlPerS);
            if (state.outletCellC < out.minOutletC) {
                out.minOutletC = state.outletCellC;
                out.sensorThenC = s.measuredC;
            }
            if (state.inletCellC < out.minInletCellC) {
                out.minInletCellC = state.inletCellC;
            }
        }
    }
    return out;
}

// What the fitted model says the two compensations should be set to, from this shot's own flow trace.
void recommend(const std::vector<Shot> &shots, const BoilerModelParams &fitted, const Options &opt) {
    const float sustainable = boilerModelMaxSustainableFlow(fitted, shots.front().targetC);
    std::printf("\nWhat this machine can hold\n");
    std::printf("  %.2f ml/s at %.1f C before the element runs out.\n", sustainable, shots.front().targetC);
    std::printf("  Every further ml/s costs %.0f W, which comes out of stored heat as a temperature drop.\n",
                BOILER_MODEL_WATER_SPECIFIC_HEAT * (shots.front().targetC - fitted.inletC));

    float worstBank = 0.0f;
    float worstRecovery = 0.0f;
    for (const auto &shot : shots) {
        // Banking: the worst cumulative shortfall over the shot, read off the measured flow rather than the profile's
        // plan, so it reflects what the shot actually did.
        std::vector<ThermalBankPhase> phases;
        phases.reserve(shot.samples.size());
        for (const auto &s : shot.samples) {
            phases.push_back({s.dtS, s.flowMlPerS});
        }
        ThermalBankParams bank;
        bank.setpointC = shot.targetC;
        bank.inletC = fitted.inletC;
        bank.heaterWatts = fitted.heaterWatts;
        bank.boilerCapacityJPerK = fitted.capacityJPerK;
        bank.maxBankK = MAX_THERMAL_BANK_LIMIT;
        bank.ceilingC = THERMAL_BANK_CEILING_C;
        const float bankK = planThermalBank(phases.data(), phases.size(), bank).bankK;

        // Recovery: the deepest debt the measured flow builds, with the element taken as flat out whenever the flow
        // needs more than it can give -- the same assumption the fit itself rests on.
        ThermalRecoveryParams rec;
        rec.setpointC = shot.targetC;
        rec.inletC = fitted.inletC;
        rec.heaterWatts = fitted.heaterWatts;
        rec.boilerCapacityJPerK = fitted.capacityJPerK;
        rec.maxBoostK = MAX_THERMAL_RECOVERY_LIMIT;
        rec.ceilingC = THERMAL_BANK_CEILING_C;
        rec.deadbandK = THERMAL_RECOVERY_DEADBAND_K;
        rec.maxDebtS = THERMAL_RECOVERY_MAX_DEBT_S;
        ThermalRecovery recovery;
        float peakDebt = 0.0f;
        for (const auto &s : shot.samples) {
            const float duty = boilerModelRequiredDuty(fitted, s.measuredC, s.flowMlPerS);
            recovery.sample(s.dtS, s.flowMlPerS, (duty > 1.0f ? 1.0f : duty) * 100.0f, rec);
            if (recovery.debtJ() > peakDebt) {
                peakDebt = recovery.debtJ();
            }
        }
        const float recoveryK = peakDebt / fitted.capacityJPerK;

        if (bankK > worstBank) {
            worstBank = bankK;
        }
        if (recoveryK > worstRecovery) {
            worstRecovery = recoveryK;
        }
    }

    const float headroom = THERMAL_BANK_CEILING_C - shots.front().targetC;
    std::printf("\nSettings this implies (Experimental tab)\n");
    std::printf("  Boiler Heat Capacity  %5.0f J/K\n", fitted.capacityJPerK);
    std::printf("  Thermal Bank          %5.2f C   (the worst shortfall these shots build before they start)\n",
                worstBank);
    std::printf("  Thermal Recovery      %5.2f C   (the deepest debt they leave behind)\n", worstRecovery);
    if (headroom < worstBank || headroom < worstRecovery) {
        std::printf("  Capped by the %.0f C ceiling: with a %.0f C offset the boiler already sits at %.1f C, leaving\n"
                    "  %.2f C of headroom. Past that it makes steam rather than storing heat.\n",
                    THERMAL_BANK_CEILING_C, opt.temperatureOffsetC, shots.front().targetC, headroom);
    }
}

void writeCsv(const std::string &path, const std::vector<Shot> &shots, const BoilerModelParams &fitted) {
    std::FILE *out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "%s: cannot write\n", path.c_str());
        return;
    }
    std::fprintf(out, "shot,t_s,measured_c,predicted_c,flow_ml_s,duty,saturated\n");
    for (size_t i = 0; i < shots.size(); i++) {
        const Shot &shot = shots[i];
        BoilerModelState state;
        bool seeded = false;
        float t = 0.0f;
        for (const auto &s : shot.samples) {
            t += s.dtS;
            if (!s.dutyKnown) {
                seeded = false;
            } else if (!seeded) {
                state.boilerC = s.measuredC;
                state.sensorC = s.measuredC;
                seeded = true;
            } else {
                boilerModelStep(state, fitted, s.dtS, s.dutyFraction, s.flowMlPerS);
            }
            std::fprintf(out, "%zu,%.3f,%.2f,%.2f,%.3f,%.3f,%d\n", i, t, s.measuredC,
                         seeded ? state.sensorC : s.measuredC, s.flowMlPerS, s.dutyFraction, s.dutyKnown ? 1 : 0);
        }
    }
    std::fclose(out);
    std::printf("\nWrote %s -- plot measured_c against predicted_c to see where the model and the machine part ways.\n",
                path.c_str());
}

} // namespace

int main(int argc, char **argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        usage();
        return 1;
    }

    const BoilerModelParams prior = priorFrom(opt);
    const HeaterControlParams control = controlFrom(opt);
    std::vector<Shot> shots;
    for (const auto &path : opt.logs) {
        Shot shot;
        if (readShot(path, opt, control, shot)) {
            shots.push_back(std::move(shot));
        }
    }
    if (shots.empty()) {
        std::fprintf(stderr, "no readable shot logs\n");
        return 1;
    }

    std::printf("Shots read\n");
    size_t saturatedTotal = 0;
    for (const auto &shot : shots) {
        describe(shot, control, opt);
        saturatedTotal += shot.saturated;
    }

    // The two-mass fit first, because it is the one that uses the whole shot. The one-node figures below are kept for
    // comparison: the compensations that ship reason with that model, so how badly it fits is worth knowing.
    const TwoMassFit wide = fitTwoMass(shots, opt);
    std::printf("\nStratified fit (every sample, duty reconstructed from the controller's own law)\n");
    std::printf("  Lower half capacity    %6.0f J/K   (started from %.0f)\n", wide.params.inletCapacityJPerK,
                opt.inletCellPrior);
    std::printf("  Upper half capacity    %6.0f J/K   (started from %.0f)\n", wide.params.outletCapacityJPerK,
                opt.outletCellPrior);
    std::printf("  Mixing between them    %6.0f W/K   (started from %.0f)\n", wide.params.mixingWPerK,
                opt.mixingPrior);
    std::printf("  Standing loss and lag  held at %.2f W/K and %.1f s -- a shot cannot see either\n",
                wide.params.lossWPerK, wide.params.sensorTauS);
    std::printf("  Open-loop RMS          %6.3f C    (%.3f C for the starting guess, %d evaluations)\n",
                wide.rmsErrorC, wide.priorRmsErrorC, wide.evaluations);
    std::printf("  Brew water bottomed at %6.1f C   while the sensor read %.1f C; the bottom of the boiler got to\n"
                "                         %6.1f C\n",
                wide.minOutletC, wide.sensorThenC, wide.minInletCellC);
    std::printf("  That first gap is the point of this model: the water reaching the puck is what the shot tastes\n"
                "  of, and no machine measures it.\n");
    if (wide.rmsErrorC > 0.5f) {
        std::printf("  Over 0.5 C out, so this structure does not describe the machine either. Check --watts and\n"
                    "  --pid against the Machine tab before reading anything into the capacities.\n");
    }
    // The idle gradient is loss / mixing, so the two are not separable from a shot: a fit wanting less mixing can
    // only have it if the loss is smaller, and the ceiling is what stops it taking both. Saying when that bound is
    // what produced the number matters more than the number.
    const float seededBottomC = boilerTwoMassSeedIdle(wide.params, shots.front().targetC).inletCellC;
    if (seededBottomC > BOILER_TWO_MASS_CEILING_C - 0.3f) {
        std::printf("  The mixing is pinned by the %.0f C ceiling, not by the data: at a standing loss of %.2f W/K\n"
                    "  anything lower would need the bottom of the boiler above boiling while it idles. The two are\n"
                    "  not separable from a shot -- their ratio IS the idle gradient. Log an idle stretch or a\n"
                    "  cooldown, where the loss is the only term left, and run that first.\n",
                    BOILER_TWO_MASS_CEILING_C, wide.params.lossWPerK);
    }

    BoilerIdentifierConfig cfg;
    cfg.prior = prior;
    BoilerIdentifier filter(cfg);
    for (int pass = 0; pass < (opt.passes > 0 ? opt.passes : 1); pass++) {
        filter.beginPass();
        for (const auto &shot : shots) {
            for (const auto &s : shot.samples) {
                filter.observe(s.dtS, s.measuredC, s.flowMlPerS, s.dutyFraction, s.dutyKnown);
            }
        }
    }
    const BoilerIdentifierResult fit = filter.result();

    std::printf("\nOne-node fit, for comparison (%zu of %zu samples taught the filter anything)\n", fit.samplesLearned,
                fit.samplesSeen);
    std::printf("  Heat capacity   %6.0f +- %4.0f J/K   (started from %.0f)\n", fit.params.capacityJPerK,
                fit.capacitySigma, opt.capacityPrior);
    std::printf("  Sensor lag      %6.2f +- %4.2f s     (started from %.2f)\n", fit.params.sensorTauS, fit.tauSigma,
                opt.tauPrior);
    std::printf("  Standing loss   %6.2f +- %4.2f W/K   (held near the %.2f it started from -- a shot cannot see it)\n",
                fit.params.lossWPerK, fit.lossSigma, opt.lossPrior);
    std::printf("  Element         %6.0f W, taken as given\n", fit.params.heaterWatts);

    if (saturatedTotal == 0) {
        std::printf("\nThe one-node figures above are still its starting guesses: no shot here held the element at a\n"
                    "certain 100 %%, so the filter had nothing to learn from. The two-mass fit does not need that --\n"
                    "it reconstructs the duty throughout -- so read it instead.\n");
        return 2;
    }

    // Open-loop replay is the test of the fit: the model runs on its own and is scored against measurements it was
    // never shown. The filter's own residuals would flatter it, since it is corrected every sample.
    std::printf("\nHow well it predicts (open loop, never corrected from the measurements)\n");
    float worstRms = 0.0f;
    for (const auto &shot : shots) {
        const auto fitted = boilerReplay(shot.samples.data(), shot.samples.size(), fit.params);
        const auto guessed = boilerReplay(shot.samples.data(), shot.samples.size(), prior);
        std::printf("  %-28s RMS %5.3f C (max %5.3f) against %5.3f C for the starting guess\n",
                    shot.profile.empty() ? "(unnamed profile)" : shot.profile.c_str(), fitted.rmsErrorC,
                    fitted.maxErrorC, guessed.rmsErrorC);
        if (fitted.rmsErrorC > worstRms) {
            worstRms = fitted.rmsErrorC;
        }
    }
    if (worstRms > 0.5f) {
        std::printf("  Over 0.5 C out, which is more than the one-node model should be. Check --watts against a plug\n"
                    "  meter and --offset against the machine's setting before trusting the capacity.\n");
    }

    // Refuse to recommend a number the data did not establish. The whole reason for using a filter is that it reports
    // how much each parameter was pinned down, and the one way to waste that is to print the estimate anyway.
    //
    // Two ways a fit can be worthless while still returning a number. The relative sigma says the data barely moved
    // the parameter: 25 % is generous, since a capacity known to a quarter is enough to set a bank to within half a
    // degree and no better. And an estimate resting on a clamp means the filter wanted to go further than any boiler
    // can, so the clamp -- not the data -- is what produced the figure.
    const float relativeSigma =
        fit.params.capacityJPerK > 0.0f ? fit.capacitySigma / fit.params.capacityJPerK : 1.0f;
    const bool atClamp = fit.params.capacityJPerK >= 19999.0f || fit.params.capacityJPerK <= 501.0f;
    if (relativeSigma > 0.25f || atClamp) {
        std::printf("\nNot identified. The heat capacity came back as %.0f +- %.0f J/K (%.0f %%)%s, which is not a\n"
                    "measurement -- only %zu samples had a duty the filter could trust, out of %zu.\n",
                    fit.params.capacityJPerK, fit.capacitySigma, relativeSigma * 100.0f,
                    atClamp ? " and is resting on a clamp" : "", fit.samplesLearned, fit.samplesSeen);
        std::printf("Leave the Experimental tab's Boiler Heat Capacity where it is and pull logs that can settle it:\n"
                    "a shot started with the machine AT its brew setpoint, not above it, with a fill phase fast\n"
                    "enough to outrun the element (over about %.1f ml/s).\n",
                    boilerModelMaxSustainableFlow(prior, shots.front().targetC) * 1.2f);
        return 3;
    }

    recommend(shots, fit.params, opt);
    if (!opt.csvPath.empty()) {
        writeCsv(opt.csvPath, shots, fit.params);
    }
    return 0;
}
