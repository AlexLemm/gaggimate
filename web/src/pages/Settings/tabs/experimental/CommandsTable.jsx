import { Trans } from '@lingui/react/macro';
import { t } from '@lingui/core/macro';
import { machine } from '../../../../services/ApiService.js';
import { num } from './thermalModel.js';

// What the firmware is commanding, what came back, and what the estimate made of it -- in three columns, because the
// interesting failures live in the gap between them.
//
// The display does not measure power. It commands a boiler setpoint and a pump drive, the controller board runs the
// PID and reports a duty, and the estimate turns that duty into watts against ratings measured once at the plug. Each
// of those is a separate claim, and until they were shown side by side a disagreement between them was invisible: the
// controller reported a duty of 100 % for an element it had been told to stop, the estimate dutifully billed 900 W,
// and the only symptom was a dashboard reading full power for a machine standing cold. Commanded and Reported now sit
// in adjacent columns, so that disagreement is a thing you can see rather than a number you have to doubt.
//
// Rows are grouped by load, then by what integrates over time. "Last shot" is the pump model's own volume figure,
// reset when a shot starts, so between shots it is the shot that just finished.

function Row({ label, commanded, reported, estimate, hint, warn = false }) {
  return (
    <>
      <tr className={warn ? 'text-warning' : ''}>
        <td className='font-medium'>{label}</td>
        <td className='font-mono tabular-nums'>{commanded}</td>
        <td className='font-mono tabular-nums'>{reported}</td>
        <td className='font-mono tabular-nums'>{estimate}</td>
      </tr>
      {hint && (
        <tr className={warn ? 'text-warning' : 'text-base-content/60'}>
          <td colSpan={4} className='pt-0 text-xs'>
            {hint}
          </td>
        </tr>
      )}
    </>
  );
}

const DASH = '—';

export function CommandsTable() {
  const status = machine.value.status ?? {};

  const setpoint = num(status.commandedBoilerSetpoint);
  const reportedDuty = num(status.currentBoilerPower);
  const chargedDuty = num(status.estimatedHeaterDuty);
  const heaterW = num(status.estimatedHeaterPower);
  const reportIgnored = !!status.heaterReportIgnored;

  const pumpCommand = num(status.commandedPumpPower);
  const pumpReported = num(status.currentPumpPower);
  const pumpW = num(status.estimatedPumpPower);
  const flow = num(status.currentFlow);

  const valveOpen = !!status.brewValveOpen;
  const otherW = num(status.estimatedOtherPower);
  const totalW = num(status.estimatedPower);
  const energyWh = num(status.estimatedEnergyWh);

  const volumeMl = num(status.waterPumped);
  const debtJ = num(status.thermalRecoveryDebt);
  const recoveryOffset = num(status.thermalRecoveryOffset);
  const bankOffset = num(status.thermalBankOffset);
  const appliedOffset = Math.max(bankOffset, recoveryOffset);

  return (
    <div className='space-y-4'>
      <div className='overflow-x-auto'>
        <table className='table-xs table w-full'>
          <thead>
            <tr>
              <th />
              <th>
                <Trans>Commanded</Trans>
              </th>
              <th>
                <Trans>Reported</Trans>
              </th>
              <th>
                <Trans>Estimated</Trans>
              </th>
            </tr>
          </thead>
          <tbody>
            <Row
              label={t`Element`}
              commanded={setpoint > 0 ? `${setpoint.toFixed(1)} °C` : t`off`}
              reported={`${reportedDuty.toFixed(1)} %`}
              estimate={`${heaterW.toFixed(0)} W`}
              warn={reportIgnored}
              hint={
                reportIgnored
                  ? t`The controller is still reporting ${reportedDuty.toFixed(1)} % for an element it has been told to stop. That is a duty left over from the last window it ran, not a reading, so it is charged as 0 W.`
                  : chargedDuty !== reportedDuty
                    ? t`Charged at ${chargedDuty.toFixed(1)} %`
                    : null
              }
            />
            <Row
              label={t`Pump`}
              commanded={pumpCommand > 0 ? `${pumpCommand.toFixed(0)} %` : t`off`}
              reported={`${pumpReported.toFixed(0)} %`}
              estimate={`${pumpW.toFixed(1)} W`}
              hint={flow > 0.05 ? t`${flow.toFixed(2)} ml/s through the pump` : null}
            />
            <Row
              label={t`Brew valve`}
              commanded={valveOpen ? t`open` : t`closed`}
              reported={DASH}
              estimate={DASH}
              hint={t`The solenoid is counted in Other below; the controller does not report it back.`}
            />
            <Row
              label={t`Other`}
              commanded={DASH}
              reported={DASH}
              estimate={`${otherW.toFixed(0)} W`}
              hint={t`Board, display and the valve while it is open. Not measured — large enough that leaving it out makes an idle machine read 0 W.`}
            />
            <Row
              label={t`Total now`}
              commanded={DASH}
              reported={DASH}
              estimate={`${totalW.toFixed(0)} W`}
              hint={t`${energyWh.toFixed(1)} Wh since boot, idle included`}
            />
          </tbody>
        </table>
      </div>

      <div className='overflow-x-auto'>
        <table className='table-xs table w-full'>
          <tbody>
            <tr>
              <td className='font-medium'>{t`Last shot`}</td>
              <td className='font-mono tabular-nums'>{volumeMl.toFixed(1)} ml</td>
              <td className='text-base-content/60 text-xs'>
                <Trans>
                  What the pump model says went through. Reset when a shot starts, so between shots
                  this is the one that just finished.
                </Trans>
              </td>
            </tr>
            <tr>
              <td className='font-medium'>{t`Energy debt`}</td>
              <td className='font-mono tabular-nums'>{(debtJ / 1000).toFixed(2)} kJ</td>
              <td className='text-base-content/60 text-xs'>
                {debtJ > 0 ? (
                  <Trans>
                    Measured: flow times the rise from inlet to setpoint, less what the element has
                    put back. Outstanding, so recovery is still asking.
                  </Trans>
                ) : (
                  <Trans>Settled. The PID owns the temperature until the next draw.</Trans>
                )}
              </td>
            </tr>
            <tr>
              <td className='font-medium'>{t`Recovery asking`}</td>
              <td className='font-mono tabular-nums'>{recoveryOffset.toFixed(2)} °C</td>
              <td className='text-base-content/60 text-xs'>
                <Trans>
                  The debt expressed as boiler temperature, capped by Max Thermal Recovery and by
                  the 100 °C ceiling.
                </Trans>
              </td>
            </tr>
            <tr>
              <td className='font-medium'>{t`Offset applied`}</td>
              <td className='font-mono tabular-nums'>{appliedOffset.toFixed(2)} °C</td>
              <td className='text-base-content/60 text-xs'>
                <Trans>
                  The larger of recovery and the pre-shot bank, never the sum: both are a request
                  for a boiler temperature, not an amount of energy to add.
                </Trans>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  );
}
