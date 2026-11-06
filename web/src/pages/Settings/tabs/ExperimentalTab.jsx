import { Trans, useLingui } from '@lingui/react/macro';
import Section from '../../../components/Card.jsx';
import { InputGroupField } from '../../../components/SettingsFormField.jsx';

// Settings that change how the machine heats, kept apart from Machine so they are not adjusted in passing. Each one
// here is reversible from this page alone: setting the bank back to 0 restores the previous behaviour exactly.
export function ExperimentalTab({ formData, onChange }) {
  const { t } = useLingui();

  return (
    <div className='space-y-4 sm:space-y-6 lg:grid lg:grid-cols-2 lg:gap-4'>
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

          <div className='grid grid-cols-1 gap-4 md:grid-cols-2'>
            <InputGroupField
              label={t`Thermal Bank`}
              htmlFor='maxThermalBank'
              unit='°C'
              helpText={t`Most the boiler may sit above the brew setpoint while it waits. 0 disables it. Needs the feedforward gain set on the Machine tab, which is where the element wattage comes from.`}
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
              helpText={t`Heat capacity of boiler, group and water. Larger values bank less, so raise it if shots start too hot. Around 3500 suits a 300 ml brass boiler.`}
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
          </div>
        </div>
      </Section>
    </div>
  );
}
