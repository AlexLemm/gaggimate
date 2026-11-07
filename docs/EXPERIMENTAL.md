# Experimental thermal features

Everything on the web UI's **Experimental** tab, what it does, and why. Each feature is off until you turn it on, and
setting its cap back to 0 restores the previous behaviour exactly.

## The one problem all of this is about

At a 93 °C brew temperature and 21 °C tank water, every ml/s of flow needs **4.18 × 72 = 301 W** to heat. A Rancilio
Silvia's element is 900 W. So the machine can sustain about **2.5 ml/s** and no more.

The opening phase of a real profile runs at **8–10 ml/s**, which needs 2.4–3.0 kW. The missing 1.5–2.1 kW comes out of
the boiler's stored heat as a temperature drop, and nothing the PID does can change that — the element is already at
100 %. The feedforward in `Heater.cpp` correctly asks for 3 kW; the element simply does not have it.

That is the whole subject. Everything below either compensates for it, measures it, or shows it to you.

### Where that account turned out to be wrong

The arithmetic above is right about the energy. It is wrong about **when** the machine feels it, and that matters for
what a compensation should do.

Measured on the author's Silvia (shot 55, temperature offset 0), through a 37.8 ml fill at 8 ml/s:

```
removed, if water leaves at 93 °C:   11 376 J
element at 100 % for 5.2 s:           4 725 J
net:                                 -6 651 J   →  -1.9 °C expected
measured at the sensor:                            +0.5 °C
```

The sensor *rose*. It only began falling three seconds after the fill ended, and then fell 7.6 °C over the next forty
seconds at a flow of barely 1.3 ml/s. The element really was at 100 % throughout the fill — at 8 ml/s the feedforward
asks for 2822 output units against a span of 1000.

The reason is geometry. 37.8 ml out of a 300 ml boiler replaces 13 % of it. Cold water enters at the bottom, where the
element is; water leaves from the top, which is also where the thermocouple is. For a draw that small the cold front
never reaches the outlet, so neither the brew water nor the sensor sees it during the fill. The deficit arrives later,
as the front works its way up.

**So the sag is real but delayed.** A compensation timed to the opening phase is early. See `BoilerTwoMass.h`, and
the measurement again in the `tools/shotsim` section below.

## The features, and how they differ

| Feature | Question it answers | When it acts |
| --- | --- | --- |
| **Pre-Shot Thermal Bank** | How much will this shot take out? | Before the shot, from the profile |
| **Thermal Recovery** | How much did it take out, and is it back yet? | During and after, from measurements |
| **Live Model** | Is the model these two rely on actually right? | Always, as a graph |
| **Power Estimate** | What is this costing, and where is the heat going? | Always |

The bank and the recovery are **complementary, not alternatives**. They are two answers to the same question asked at
different times, and the firmware takes the larger of the two rather than their sum — both are a request for a boiler
temperature, not an amount of energy, so adding them would ask for a temperature neither intended.

### Pre-Shot Thermal Bank

Reads the selected profile, works out the flow and duration of every phase, and parks the boiler above its setpoint by
however much stored heat the shot will need. Released the instant the shot starts, because spending that heat is the
point.

What it banks is the worst **cumulative** shortfall, not the total: a bloom phase pumps nothing, so the element catches
up during it, and surpluses only refill the bank up to the setpoint.

- Needs the profile, so it covers planned shots only.
- Capped by your setting, and absolutely by a 100 °C boiler ceiling — past atmospheric boiling the boiler makes steam
  rather than storing heat.

**Mis-timed on a stratified boiler.** The bank is released at the start of the shot, to cover a sag in the opening
phase. On the author's machine the opening phase does not sag — the sensor gains 0.5 °C through it — and the deficit
turns up ten to forty seconds later, by which time the banked heat has been spent. That is a timing problem, not a
tuning one, so lowering the cap does not fix it. Leave this at 0 on a machine that behaves this way and use Thermal
Recovery, which is still holding a boost when the fall actually happens.

### Thermal Recovery

The measured half. Each telemetry frame:

```
removed  = flow · dt · c · (setpoint − inlet)     water leaving carries this away
restored = watts · duty · dt                      the element puts this back
debt    += removed − restored, floored at zero
```

Both terms are measured, so no profile and no prediction is involved. The debt is then asked for as a temperature:

```
boost = min(debt / C, your cap), bounded by the ceiling
```

While the debt is large the offset exceeds what the PID can reach, so the element runs at 100 % — "as fast as possible"
falls out of the arithmetic rather than being commanded. As the debt drains the offset shrinks with it, so control
hands back to the PID continuously, with no step to overshoot through.

**On subtracting the measured duty.** The obvious reading of "drive at full power for Energy ÷ Power seconds"
double-counts badly. During the sag the element is *already* saturated, so most of that energy is being repaid while
the debt is still being measured. For a 5 s fill at 9.85 ml/s: 15 849 J removed, but 4 500 J already delivered, so the
honest remaining figure is 12.6 s of full power, not 17.6 s. Running the naive figure would overshoot by 4 500 J —
about 1.4 °C on a 3 300 J/K boiler.

Covers what the bank cannot: the bloom, the pause after a shot, a flush, and any shot that ran longer or pulled more
water than planned.

**First measurement on a real machine**, at a 4 °C cap, on a 62 ml free-flow draw at 9.7 ml/s from a boiler at
86.5 °C with a profile target of 85 °C. The debt pushed the setpoint up by 2.95 °C of its 4 °C allowance by the end of
the draw, which took the element from gated off — the boiler was 1.5 °C *over* its unboosted target, where the safety
scaling cuts the feedforward — to pinned at 100 %:

```
element returned 0.9 kJ without recovery, 7.4 kJ with it, against 16.4 kJ the water carried off
```

So the compensation does what the arithmetic says. What this draw cannot show is whether it ends in the right place:
the log stops at the moment the pump does, and on a stratified boiler the deficit arrives *tens of seconds later* —
shot 55 was still falling forty seconds after its fill. Judging the setting needs a capture that keeps recording past
the end of the shot (`tools/thermalcap`, the "flush" row below).

### Live Model vs Machine (the graph)

The firmware runs the same one-node boiler model the two compensations reason about, in parallel with the real boiler,
stepped from its own previous state and **never corrected from the sensor**. So the gap between the traces is not a
display artefact — it is how wrong the model is, live.

This matters because the bank and the recovery size themselves from exactly those parameters. A model trace that runs
hot and a bank that comes out too large are the same fact seen twice.

Three temperature series, and the third is the interesting one:

- **Measured** — what the thermocouple reports.
- **Model sensor** — what the model predicts it would report.
- **Model water** — what the model thinks the *water* is at. **No machine can measure this.** The sensor sits on the
  outside of the boiler and lags it by about 4 s, so during a shot the water is already several degrees down while the
  sensor has barely moved. That gap is why the PID's derivative term brakes too late.

Open loop drifts, so the model re-seeds itself from the measurement when the error passes 5 °C. Frequent re-seeding is
itself the signal: it means the parameters do not fit.

### Power Estimate

No current sensor. Every load is either on or driven at a duty the firmware itself commands, so the draw is
(rating × duty) summed over the loads, with ratings measured once at the plug.

Measured on the user's Silvia: **900 W element, 59 W pump at full.**

- The element is 94 % of a full-load draw, purely resistive, and its duty is exactly known — so the total is good to a
  few per cent.
- The pump term is a **linear approximation**. A vibratory pump under phase-angle control is closer to the square of
  the conduction fraction. It is worth 59 W at most, and the error inside that is less. Accurate enough for "what does
  a shot cost", not accurate enough to bill anyone. `PowerEstimateParams::pumpCurve` exists to improve on it without
  touching anything else.
- **Net into boiler** is the number to watch during a shot: element minus what the water carried away minus standing
  loss. It goes sharply negative the moment a high-flow phase starts — that is the sag in watts, before the sensor has
  caught up enough to show it.

## Setting it up

1. **Get the element wattage right.** Measure it at the plug and set `Kff = 1000 ÷ watts` as the fourth PID
   coefficient on the Machine tab. At 900 W that is **1.111**. Every thermal feature derives the wattage from this one
   number, so there is a single thing to get right and nothing can disagree about it. A measured 900 W against a Kff of
   1.053 (= 950 W) makes every compensation 5 % optimistic.
2. **Set the inlet water temperature** to your tank's. Lower it in winter if you fill from a cold tap.
3. **Set the boiler heat capacity.** ~3500 J/K suits a 300 ml brass boiler. To measure it rather than guess, see
   `tools/shotsim` below.
4. **Turn one feature on at a time**, starting at 2 °C, and watch the Status panel to confirm it says `On` rather than
   `Needs setup`.

If the first mouthful tastes hot, lower the cap or raise the heat capacity — a capacity set too low over-compensates.

### Watch the ceiling

The 100 °C boiler ceiling bounds the **real** boiler temperature, temperature offset included, because past
atmospheric boiling the boiler makes steam rather than storing heat.

How much room that leaves depends entirely on your offset, and it is worth knowing which case you are in:

- **Offset 0** — the displayed temperature *is* the boiler temperature. A 93 °C profile leaves 7 °C of headroom, so
  the 5 °C caps on the bank and the recovery bind long before the ceiling does. This is the simpler arrangement, and
  it is why the offset is worth setting to 0 and working from real values.
- **A non-zero offset** — the boiler sits that much hotter than the UI shows, and the headroom shrinks by the same
  amount. With a 5 °C offset the same 93 °C profile already has the boiler at 98 °C, leaving room for 2 °C and no
  more; asking for 3 is silently clipped to 2.

Nothing warns you when the ceiling clips a cap, so check the arithmetic rather than assuming the number you typed is
the number in force.

## Measuring your machine instead of guessing: `tools/shotsim`

```bash
pio run -e shotsim
.pio/build/shotsim/program --watts 900 --offset 5 shots/*.slog
```

Reads `.slog` files off the machine (shot history in the web UI, or `/h` on its filesystem), fits the boiler model with
a joint extended Kalman filter, replays each shot open-loop to score the fit, and prints the settings the fit implies.

**Two models, and it prints both.** The stratified fit (`BoilerTwoMass.h`) comes first because it uses every sample:
it reconstructs the whole duty trace by running the controller's own law over the measured temperatures, which is
legitimate because the reconstruction is driven by what the sensor reported rather than by the model being fitted. The
one-node fit follows, for comparison, because the shipped compensations reason with that model and how badly it fits is
worth knowing. On the author's two clean shots:

```
Stratified    open-loop RMS 0.260 °C   (0.909 °C for the starting guess)
One node      heat capacity 20000 ± 8100 J/K, resting on a clamp — not a measurement
```

A 20 kJ/K boiler is six times anything a Silvia contains. That is the one-node model being asked to explain a sensor
that barely moves, and having only one knob to do it with.

**What it can and cannot identify.** The shot log records temperatures, pressures, flows and weights — but **not heater
duty**. For the one-node filter only samples whose duty is *certain* are used, and what makes it certain is the
controller, not the physics: above about 3.2 ml/s the feedforward alone exceeds the output span, so the element is
pinned even with the boiler sitting exactly on its setpoint. Requiring it to be *below* the setpoint — an earlier
version of this test — threw away every usable fill phase and reported "nothing usable" on the only good logs.

**Thermal Recovery changes what a shot log means.** The log records the **profile** target. With recovery switched on,
the setpoint the machine was actually holding is that plus whatever the recovery balance was asking for, and the log
does not contain it — so `--recovery <C>` has to be passed or the reconstructed duty is wrong by the whole element.
On shot 57 of the author's machine, taken with Thermal Recovery at 4 °C:

```
--recovery 0     0/36 samples at full duty, mean duty  9.5 % — "usually a temperature offset, check --offset"
--recovery 4    18/36 samples at full duty, mean duty 76.4 %, recovery asked for up to 2.95 °C
```

The sensor read 86.5 °C against a logged target of 85. Against that target it is 1.5 °C over, where the safety scaling
switches the feedforward off; against the 88 °C recovery had pushed the setpoint to, it is 1.5 °C under and the element
was pinned. Same measurement, opposite answer. `test_a_recovery_boosted_setpoint_flips_the_gate` holds it.

**The mixing was pinned by a bound, and now is not.** The fit used to land with its mixing at 12 W/K, which was not a
measurement: the idle gradient is the standing loss divided by the mixing, so a shot sees only their ratio, and the
only thing stopping the fit from taking less of both was the 100 °C ceiling. Measuring the loss (below) removed that —
at 0.57 W/K rather than the 1.2 W/K prior, the ceiling no longer binds. The fit then walks into the *next* bound
instead, with the upper cell on its 200 J/K floor, and the tool now names whichever bound produced a figure:

```
Resting on a bound: upper half capacity.
The search wanted to go further than a boiler can, so the bound -- not the data -- is what
produced that figure. Read it as "at least this far", not as a measurement.
```

That trade — a tiny upper cell against a long sensor lag — is what a warm-up capture breaks.

Consequently:

- **Heat capacity** is pinned down well — it *is* the sag, which is exactly what a high-flow phase measures. From a 9×
  range of starting guesses the filter lands within 1 % on synthetic data. This is the number to put in the web UI.
- **Sensor lag** is pinned down reasonably — it is the delay between flow starting and the sensor moving.
- **Standing loss** is not identifiable from a shot at all, and is now measured instead of fitted — see
  `tools/thermalcap` below. Left free in a shot-only fit it absorbs the capacity error instead of converging, so the
  shot-only fit holds it at its prior and says so.

The reported ±1σ figures are the point of using a filter rather than a curve fit: they say how much the data actually
established, which is not the same for the three parameters.

**The replay RMS is the real test.** It runs the fitted model with no correction at all and scores it against
measurements it was never shown. The filter's own residuals would flatter it, since it is corrected every sample. Under
0.1 °C means the model predicts your machine; over 0.5 °C means check `--watts` and `--offset` before trusting
anything.

A shot that never saturated the element will report that it taught the filter nothing, rather than returning the
starting guesses dressed up as an answer.

## Recording what a shot cannot show: `tools/thermalcap`

```bash
node tools/thermalcap/thermalcap.mjs --host 192.168.1.177 --out idle.csv --minutes 5 --note "idling at 85 C"
.pio/build/shotsim/program --watts 900 --capture idle.csv shots/*.slog
```

A shot log has two gaps no amount of fitting can close: it does not record heater duty, and it is only ever thirty
seconds of a boiler being disturbed. The websocket closes both. It already publishes, twice a second, the **measured**
duty (`hp`), the thermocouple, the profile target, the offset each compensation is asking for (`tro`, `tbo`), the flow,
and what the firmware's own live model believes. `thermalcap` writes all of it to a CSV; `shotsim --capture` reads it.
Captures and `.slog` files mix freely in one run.

The tool only ever listens. It writes no setting and starts no process — turning the heater on or off, and deciding
when the machine is cold, are the operator's business.

**What to record, and what each one pins:**

| Capture | Why |
| --- | --- |
| **idle** — sitting at temperature, pump off | The duty *is* the standing loss. Pins UA, which frees the mixing. |
| **cooldown** — heater off, machine hot | The loss is the only term left, and the decay rate also sees the capacity. |
| **warm-up** — from cold to working temperature | Duty is a known 100 % the whole way up over seventy degrees, so the rise rate is the capacity and nothing else. |
| **flush** — a draw with no basket, logged well past the end | Shows the delayed sag a shot log cuts off at the end of the shot. |

**The standing loss is measured, not fitted** (`StandingLoss.h`). A boiler sitting at temperature with the valve shut
is doing one thing, so over such a stretch

```
∫ P·duty dt  =  UA·∫ (T − ambient) dt  +  C·(T_end − T_start)
```

and if the stretch is long and ends near where it started, the capacity term vanishes and UA is the ratio of the other
two. No model, no optimiser, no starting guess to drift back towards. The leftover term is not assumed away but
bounded and printed — a stretch that drifted enough for the bound to matter is one whose answer should not be trusted.
Five minutes of the author's machine idling at 85 °C:

```
Standing loss, measured off 275 s of idle in 1 window
  0.57 W/K at a mean 85.2 C, i.e. 37 W to hold the boiler there.
  Drift over those windows could account for 0.01 W/K of that, taking the capacity as 3500 J/K.
```

Less than half the 1.2 W/K that had been assumed, and the one-node filter independently lands on 0.60 ± 0.13 W/K from
the same capture. The measurement replaces the prior for everything fitted afterwards.

**The fitted dimension count follows the data.** Three parameters from shots alone; the standing loss and the sensor
lag join them only once a capture is present, because only then can anything see them. Handing a search two more
parameters than the data can resolve does not produce a cautious answer, it produces a confident wrong one — which is
how an earlier version of the two-mass fit arrived at a 5 W/K coupling with the water idling at 110 °C.

**A capture where nothing happened says so.** An idle capture fits beautifully — 0.092 °C RMS — and establishes no
capacity whatever, because the trace is flat and the search is free to put the capacities anywhere. The tool prints
that rather than four significant figures of noise:

```
But nothing happened: the boiler moved 0.4 C and the pump never ran, so the capacities above
are whatever the search settled on and the small RMS only says the trace was flat.
```

**It reconnects, and counts the holes.** The machine drops a websocket client that falls behind its send queue, which
it does to a listener that only listens — about every six seconds. The web UI copes by reconnecting with backoff and
never mentioning it, so a capture does the same or it records six seconds and stops. Each reconnect costs a sample or
two; the gaps are counted, the worst is recorded in the file's footer, and intervals spanning one are dropped rather
than interpolated across.

## Where the code lives

Each piece is a header with no Arduino dependency, so all of it builds and is tested natively.

| File | What it is |
| --- | --- |
| `src/display/util/ThermalBank.h` | Pre-shot banking arithmetic |
| `src/display/util/ThermalRecovery.h` | Measured energy balance and the offset it asks for |
| `src/display/util/BoilerModel.h` | The one-node thermal model all of the above assume |
| `src/display/util/BoilerTwoMass.h` | Two cells in series, for the stratification one node cannot show |
| `src/display/util/HeaterControlModel.h` | The controller's own law, so a tool can say what duty it commanded |
| `src/display/util/StandingLoss.h` | Measures UA off an idle stretch — the one parameter here that is not fitted |
| `src/display/util/BoilerObserver.h` | Runs the one-node model live beside the real boiler |
| `src/display/util/BoilerIdentifier.h` | The joint EKF that fits the one-node model to a machine |
| `src/display/util/PowerEstimate.h` | Power and energy from commanded duties |
| `tools/shotsim/main.cpp` | Offline: read logs and captures, fit, replay, recommend |
| `tools/thermalcap/thermalcap.mjs` | Records live telemetry, measured duty included, to a CSV `shotsim` reads |
| `web/src/pages/Settings/tabs/experimental/` | Status rows, graph, power panel |

Run the tests with:

```bash
pio test -e native -f test_thermal_bank -f test_thermal_recovery -f test_boiler_model -f test_boiler_two_mass -f test_heater_control -f test_standing_loss
```

## Change log

Newest first. Each entry is one commit.

| Change | What it does |
| --- | --- |
| `feat(tools): measure the standing loss instead of guessing at it` | `tools/thermalcap`, `StandingLoss.h`, `shotsim --capture`, and the recovery-aware duty reconstruction |
| `feat(boiler): model the boiler as two cells in series, and fix the duty gate` | `BoilerTwoMass.h`, `HeaterControlModel.h`, the corrected `dutyIsCertainlyFull`, and the stratified fit in `shotsim` |
| `feat(web): graph the live model and the estimated power` | The Experimental dashboard: status rows, model-vs-machine graph, power panel |
| `feat(boiler): run the thermal model live and estimate power draw` | `BoilerObserver` and `PowerEstimate`, wired as observers that command nothing |
| `feat(tools): fit the boiler model to a machine from its shot logs` | `tools/shotsim` and the `shotsim` build environment |
| `feat(boiler): identify the boiler model from shot logs with a Kalman filter` | `BoilerModel` and `BoilerIdentifier` plus their tests |
| `feat(web): show what is on and off on the Experimental tab` | Status panel, and the tab split into modules |
| `feat(boiler): pay back the energy a shot measurably removed` | `ThermalRecovery` and its wiring |
| `docs: record the build traps that cost a session` | `CLAUDE.md` |
| `fix(boiler): bank against the real boiler setpoint, not the displayed one` | The temperature-offset and ceiling fix |
| `feat(web): move the banking settings to an Experimental tab` | The tab itself |
| `feat(ota): install controller firmware from a file over Wi-Fi` | Controller OTA over the network |
| `feat(boiler): bank heat before a shot so high-flow phases do not sag` | `ThermalBank` |
| `fix(web): stop the stall watchdog cancelling healthy firmware uploads` | The unsigned-time race in the upload path |
