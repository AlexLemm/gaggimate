import { Trans } from '@lingui/react/macro';
import { t } from '@lingui/core/macro';
import { machine } from '../../../../services/ApiService.js';
import { num } from './thermalModel.js';

// Live power and the model's energy balance, read straight from the status frame.
//
// No current sensor is involved. Every load is either on or driven at a duty the firmware itself sets, so the draw is
// (rating x duty) summed over the loads, with the ratings measured once at the plug -- 900 W element, 59 W pump on the
// user's Silvia. See src/display/util/PowerEstimate.h for what that is and is not worth.
//
// "Net into boiler" is the one to watch during a shot. It is the element minus what the outgoing water carries away
// minus standing loss, so it goes sharply negative the moment a high-flow phase starts: that number is the sag while
// it is happening, in watts, before the sensor has caught up enough to show it.

function Readout({ label, value, unit, hint, tone = '' }) {
  return (
    <div className='border-base-300 rounded-box border p-3'>
      <div className='text-base-content/60 text-xs'>{label}</div>
      <div className={`font-mono text-xl tabular-nums ${tone}`}>
        {value}
        <span className='text-base-content/60 ml-1 text-sm'>{unit}</span>
      </div>
      {hint && <div className='text-base-content/50 mt-1 text-xs'>{hint}</div>}
    </div>
  );
}

export function PowerPanel() {
  const status = machine.value.status ?? {};
  const watts = num(status.estimatedPower);
  const energyWh = num(status.estimatedEnergyWh);
  const netW = num(status.modelNetPower);
  // The duty the estimate charged for, not the one the controller reported: with no setpoint commanded those differ,
  // and printing the reported figure beside watts computed from the other is how a standing-by machine came to
  // advertise a full element. See src/display/util/PowerEstimate.h.
  const heaterDuty = num(status.estimatedHeaterDuty);
  const debtJ = num(status.thermalRecoveryDebt);
  const bankOffset = num(status.thermalBankOffset);
  const recoveryOffset = num(status.thermalRecoveryOffset);
  const appliedOffset = Math.max(bankOffset, recoveryOffset);

  return (
    <div className='space-y-3'>
      <p className='text-base-content/70 text-sm'>
        <Trans>
          Estimated from the duties the firmware commands and the ratings measured at the plug, not
          from a current sensor. The element is 94 % of a full-load draw and its duty is known
          rather than modelled, so the total is good to a few per cent; the pump term is a linear
          approximation worth 59 W at most. It is known by the controller board, though, not here —
          the table below puts it beside the command, which is the only way to see the two
          disagree.
        </Trans>
      </p>

      <div className='grid grid-cols-2 gap-3 lg:grid-cols-4'>
        <Readout
          label={t`Drawing now`}
          value={watts.toFixed(0)}
          unit='W'
          hint={t`Element at ${heaterDuty.toFixed(0)} %`}
        />
        <Readout
          label={t`Net into boiler`}
          value={netW.toFixed(0)}
          unit='W'
          hint={netW < 0 ? t`Losing heat faster than the element can replace it` : t`Keeping up`}
          tone={netW < 0 ? 'text-warning' : ''}
        />
        <Readout
          label={t`Energy since boot`}
          value={energyWh.toFixed(1)}
          unit='Wh'
          hint={t`Everything, idle included`}
        />
        <Readout
          label={t`Offset applied`}
          value={appliedOffset.toFixed(2)}
          unit='°C'
          hint={
            debtJ > 0
              ? t`${(debtJ / 1000).toFixed(1)} kJ of measured debt outstanding`
              : t`Nothing outstanding; the PID owns the temperature`
          }
          tone={appliedOffset > 0 ? 'text-success' : ''}
        />
      </div>
    </div>
  );
}
