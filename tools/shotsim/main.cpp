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
// What it cannot do: the shot log does not record heater duty, so the element's contribution has to be inferred. It is
// only known for certain while the flow exceeds what the element can hold, where the feedforward pins the output at
// 100 %; everywhere else the fit learns nothing. See the note at the top of BoilerIdentifier.h. A log of a gentle shot
// that never saturates the element will honestly report that it taught the filter nothing.
#include <display/core/constants.h>
#include <display/models/shot_log_format.h>
#include <display/util/BoilerIdentifier.h>
#include <display/util/BoilerModel.h>
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
    // The margin on the saturation test. 1.2 leaves out samples near the boundary, where the duty is genuinely in
    // doubt and a guess of 100 % would be the filter learning from fiction.
    float saturationMargin = 1.2f;
    int passes = 6;
    std::string csvPath;
    std::vector<std::string> logs;
};

// How far below its setpoint the boiler must sit before the element is taken to be certainly on. Covers the PID
// easing off as it closes on the setpoint, and the temperature offset being slightly out.
constexpr float SETPOINT_MARGIN_K = 2.0f;

struct Shot {
    std::string name;
    std::string profile;
    std::vector<BoilerLogSample> samples;
    float peakFlow = 0.0f;
    float startC = 0.0f;
    float minC = 0.0f;
    float targetC = 0.0f;
    float waterMl = 0.0f;
    float durationS = 0.0f;
    size_t saturated = 0;
};

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
                "  --margin <x>     how far past the element's limit a sample must be to count (default 1.2)\n"
                "  --passes <n>     sweeps over the data; more makes the answer independent of the guess (default 6)\n"
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
bool readShot(const std::string &path, const Options &opt, const BoilerModelParams &prior, Shot &out) {
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
        // Both conditions: the flow must outrun the element AND the boiler must be below its setpoint, or the duty is
        // not known to be 100 %. See BoilerIdentifier::dutyIsCertainlyFull -- leaving the second one out is what made
        // the first run against real logs return half the true heat capacity.
        sample.dutyKnown = BoilerIdentifier::dutyIsCertainlyFull(prior, measuredC, targetC, sample.flowMlPerS,
                                                                 opt.saturationMargin, SETPOINT_MARGIN_K);
        sample.dutyFraction = sample.dutyKnown ? 1.0f : 0.0f;

        if (sample.dtS > 0.0f && sample.dtS < 5.0f) {
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
void describe(const Shot &shot, const BoilerModelParams &params) {
    const float limit = boilerModelMaxSustainableFlow(params, shot.targetC > 0.0f ? shot.targetC : shot.startC);
    std::printf("  %-20s %5.1f s %6.1f ml  peak %4.2f ml/s  %5.1f -> %5.1f C (target %5.1f)  %zu/%zu usable\n",
                shot.profile.empty() ? "(unnamed)" : shot.profile.c_str(), shot.durationS, shot.waterMl, shot.peakFlow,
                shot.startC, shot.minC, shot.targetC, shot.saturated, shot.samples.size());

    if (shot.startC > shot.targetC + SETPOINT_MARGIN_K) {
        std::printf("    started %.1f C above the setpoint, so most of that fall is the machine cooling back to it\n"
                    "    rather than the water taking heat out.\n",
                    shot.startC - shot.targetC);
    }
    if (shot.saturated > 0) {
        return;
    }
    if (shot.peakFlow <= limit * 1.2f) {
        std::printf("    nothing usable: the flow never passed %.2f ml/s, so the element was never certainly flat\n"
                    "    out. Pull a shot with a faster fill phase.\n",
                    limit * 1.2f);
    } else {
        std::printf("    nothing usable: the flow was fast enough, but the boiler never got %.0f C below its\n"
                    "    setpoint while it was, so the element may have been switched off throughout. Pull a shot\n"
                    "    from a machine sitting at its setpoint rather than above it.\n",
                    SETPOINT_MARGIN_K);
    }
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
    std::vector<Shot> shots;
    for (const auto &path : opt.logs) {
        Shot shot;
        if (readShot(path, opt, prior, shot)) {
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
        describe(shot, prior);
        saturatedTotal += shot.saturated;
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

    std::printf("\nFitted model (%zu of %zu samples taught the filter anything)\n", fit.samplesLearned,
                fit.samplesSeen);
    std::printf("  Heat capacity   %6.0f +- %4.0f J/K   (started from %.0f)\n", fit.params.capacityJPerK,
                fit.capacitySigma, opt.capacityPrior);
    std::printf("  Sensor lag      %6.2f +- %4.2f s     (started from %.2f)\n", fit.params.sensorTauS, fit.tauSigma,
                opt.tauPrior);
    std::printf("  Standing loss   %6.2f +- %4.2f W/K   (held near the %.2f it started from -- a shot cannot see it)\n",
                fit.params.lossWPerK, fit.lossSigma, opt.lossPrior);
    std::printf("  Element         %6.0f W, taken as given\n", fit.params.heaterWatts);

    if (saturatedTotal == 0) {
        std::printf("\nNothing was identified: no shot here ran the element flat out for certain, so the duty was\n"
                    "unknown throughout and the figures above are still the starting guesses.\n");
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
