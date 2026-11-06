import { Trans, useLingui } from '@lingui/react/macro';
import Section from '../../../components/Card.jsx';
import { InputGroupField } from '../../../components/SettingsFormField.jsx';
import { FeatureStatus } from './experimental/FeatureStatus.jsx';
import { ModelTraceChart } from './experimental/ModelTraceChart.jsx';
import { PowerPanel } from './experimental/PowerPanel.jsx';
import { BLOCKER, featureState, heaterWatts, num, thermalBlocker } from './experimental/thermalModel.js';

// Settings that change how the machine heats, kept apart from Machine so they are not adjusted in passing. Each one
// here is reversible from this page alone: setting a cap back to 0 restores the previous behaviour exactly.
//
// The tab is assembled from the pieces in ./experimental/ rather than written as one file, because the features
// landing here are independent of each other and should be addable and removable the same way. Each piece reads what
// it needs from the live status or the form and knows nothing about the others.
//
// The status panel and the graph exist because these features are otherwise invisible. Nothing on the brew screen says
// whether the boiler is being parked above its setpoint, and a cap set to a sensible-looking number still does nothing
// if the feedforward gain it depends on is missing -- the failure that looks exactly like the feature not working.

export function ExperimentalTab({ formData, onChange }) {
  const { t } = useLingui();

  const watts = heaterWatts(formData);
  const capacity = num(formData.boilerHeatCapacity);
  const bank = num(formData.maxThermalBank);
  const recovery = num(formData.maxThermalRecovery);
  // thermalBlocker returns a code; the wording lives here, where the Lingui macro can actually see it.
  const blockerCode = thermalBlocker(formData);
  const blocker =
    blockerCode === BLOCKER.NEEDS_KFF
      ? t`Set the fourth PID coefficient (Kff) on the Machine tab — the element wattage comes from it.`
      : blockerCode === BLOCKER.NEEDS_CAPACITY
        ? t`Set a boiler heat capacity below.`
        : null;

  const detail = (cap, onText) => {
    if (num(cap) <= 0) return t`Disabled — the cap is 0.`;
    return blocker ?? onText;
  };
  const bankCap = bank.toFixed(1);
  const recoveryCap = recovery.toFixed(1);

  return (
    <div className='space-y-4 sm:space-y-6 lg:grid lg:grid-cols-2 lg:gap-4'>
      <Section title={t`Status`} className='h-full lg:col-span-2'>
        <div className='space-y-2'>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              What is running right now. Nothing here changes the machine; the settings below do.
            </Trans>
          </p>
          <div>
            <FeatureStatus
              name={t`Pre-Shot Thermal Bank`}
              state={featureState(bank, blockerCode)}
              value={bank > 0 ? `${bankCap} °C` : null}
              detail={detail(
                bank,
                t`Waiting above the brew setpoint, released when the shot starts.`,
              )}
            />
            <FeatureStatus
              name={t`Thermal Recovery`}
              state={featureState(recovery, blockerCode)}
              value={recovery > 0 ? `${recoveryCap} °C` : null}
              detail={detail(
                recovery,
                t`Paying measured energy back above the setpoint until the boiler is square.`,
              )}
            />
            <FeatureStatus
              name={t`Live Model & Power Estimate`}
              state={featureState(watts > 0 && capacity > 0 ? 1 : 0, blockerCode)}
              value={watts > 0 ? `${Math.round(watts)} W` : null}
              detail={
                blocker ??
                t`${Math.round(watts)} W element, ${Math.round(capacity)} J/K boiler, ${Math.round(num(formData.inletWaterTemp))} °C inlet water.`
              }
            />
          </div>
        </div>
      </Section>

      <Section title={t`Model vs Machine`} className='h-full lg:col-span-2'>
        <div className='space-y-3'>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              The firmware runs the same boiler model the compensations reason about alongside the real
              boiler, stepped from its own state and never corrected from the sensor. Where the traces
              part company, the model's parameters are wrong — and since the bank and the recovery size
              themselves from those parameters, a model that runs hot and a bank that comes out too
              large are the same fact seen twice.
            </Trans>
          </p>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              Model water is the one to watch: no machine can measure it, because the sensor sits
              outside the boiler and lags it by seconds. During a shot the water is already several
              degrees down while the sensor has barely moved, which is why the PID reacts too late.
            </Trans>
          </p>
          <ModelTraceChart />
        </div>
      </Section>

      <Section title={t`Power`} className='h-full lg:col-span-2'>
        <PowerPanel />
      </Section>

      <Section title={t`Pre-Shot Thermal Bank`} className='h-full'>
        <div className='space-y-4'>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              A shot's high-flow phases need more power than the heating element can deliver, so the
              temperature drops as cold water arrives. The profile states the flow of every phase
              before the shot starts, so the boiler can wait a little hotter and spend that stored
              heat on the shot instead of sagging through it.
            </Trans>
          </p>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              Start at 2 °C. If the first mouthful tastes hot, lower it or raise the heat capacity.
              Set the bank to 0 to turn this off.
            </Trans>
          </p>

          <InputGroupField
            label={t`Thermal Bank`}
            htmlFor='maxThermalBank'
            unit='°C'
            helpText={t`Most the boiler may sit above the brew setpoint while it waits. 0 disables it.`}
            noMargin
          >
            <input
              id='maxThermalBank'
              name='maxThermalBank'
              type='number'
              step='0.1'
              min='0'
              max='5'
              className='grow'
              placeholder='0.0'
              value={formData.maxThermalBank}
              onChange={onChange('maxThermalBank')}
            />
          </InputGroupField>
        </div>
      </Section>

      <Section title={t`Thermal Recovery`} className='h-full'>
        <div className='space-y-4'>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              The other half of the same problem, measured instead of predicted. The machine counts
              the water the pump actually moves, works out the heat it carried out of the boiler,
              subtracts what the element has already put back, and holds the boiler above its
              setpoint until that balance is square. The element runs flat out while the gap is
              large and eases off as it closes, so control hands back to the PID without a step.
            </Trans>
          </p>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              This is what recovers the bloom and the pause after a shot, and it works on a flush
              too. Start at 2 °C. Unlike the bank it needs no profile, so it also covers shots that
              run longer or pull more water than planned.
            </Trans>
          </p>

          <InputGroupField
            label={t`Thermal Recovery`}
            htmlFor='maxThermalRecovery'
            unit='°C'
            helpText={t`Most the boiler may sit above the setpoint while it pays measured energy back. 0 disables it.`}
            noMargin
          >
            <input
              id='maxThermalRecovery'
              name='maxThermalRecovery'
              type='number'
              step='0.1'
              min='0'
              max='5'
              className='grow'
              placeholder='0.0'
              value={formData.maxThermalRecovery}
              onChange={onChange('maxThermalRecovery')}
            />
          </InputGroupField>
        </div>
      </Section>

      <Section title={t`Machine Thermal Model`} className='h-full lg:col-span-2'>
        <div className='space-y-4'>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              Everything above uses these numbers, plus the element wattage taken from the
              feedforward gain (Kff) on the Machine tab — one number to get right, so the bank, the
              recovery, the live model and the power estimate cannot disagree about it. Set Kff to
              1000 divided by the wattage you measure at the plug.
            </Trans>
          </p>
          <p className='text-base-content/70 text-sm'>
            <Trans>
              Getting the heat capacity roughly right matters more than getting it exactly right: it
              only scales how much a given energy gap is worth in degrees. To measure it rather than
              guess, run tools/shotsim over a few of your own shot logs — it fits the model with a
              Kalman filter and prints the value to put here.
            </Trans>
          </p>

          <div className='grid grid-cols-1 gap-4 md:grid-cols-3'>
            <InputGroupField
              label={t`Inlet Water Temperature`}
              htmlFor='inletWaterTemp'
              unit='°C'
              helpText={t`Temperature of the water in the tank, used to work out how much heat the incoming water takes. Lower it in winter if you fill from a cold tap.`}
              noMargin
            >
              <input
                id='inletWaterTemp'
                name='inletWaterTemp'
                type='number'
                step='1'
                min='0'
                max='60'
                className='grow'
                placeholder='21'
                value={formData.inletWaterTemp}
                onChange={onChange('inletWaterTemp')}
              />
            </InputGroupField>
            <InputGroupField
              label={t`Boiler Heat Capacity`}
              htmlFor='boilerHeatCapacity'
              unit='J/K'
              helpText={t`Heat capacity of boiler, group and water. Larger values compensate less, so raise it if shots start too hot. Around 3500 suits a 300 ml brass boiler.`}
              noMargin
            >
              <input
                id='boilerHeatCapacity'
                name='boilerHeatCapacity'
                type='number'
                step='100'
                min='0'
                className='grow'
                placeholder='3500'
                value={formData.boilerHeatCapacity}
                onChange={onChange('boilerHeatCapacity')}
              />
            </InputGroupField>
            <InputGroupField
              label={t`Pump Power`}
              htmlFor='pumpRatedWatts'
              unit='W'
              helpText={t`What the pump draws at full drive, measured at the plug. Only feeds the power estimate; it changes nothing about how the machine runs. 59 W on a Rancilio Silvia.`}
              noMargin
            >
              <input
                id='pumpRatedWatts'
                name='pumpRatedWatts'
                type='number'
                step='1'
                min='0'
                max='500'
                className='grow'
                placeholder='59'
                value={formData.pumpRatedWatts}
                onChange={onChange('pumpRatedWatts')}
              />
            </InputGroupField>
          </div>
        </div>
      </Section>
    </div>
  );
}
