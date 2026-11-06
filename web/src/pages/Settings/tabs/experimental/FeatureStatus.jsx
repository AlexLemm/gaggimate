import { Trans } from '@lingui/react/macro';
import { FEATURE_STATE } from './thermalModel.js';

// One row of the status panel. Three states, and the distinction between the last two is the point: "off" is a
// choice, "needs setup" is a cap the machine is ignoring because something it depends on is missing -- which is the
// failure that otherwise looks like the feature being broken.
export function FeatureStatus({ name, state, detail, value }) {
  const badge = {
    [FEATURE_STATE.ON]: 'badge badge-success',
    [FEATURE_STATE.OFF]: 'badge badge-ghost',
    [FEATURE_STATE.INCOMPLETE]: 'badge badge-warning',
  }[state];
  const label = {
    [FEATURE_STATE.ON]: <Trans>On</Trans>,
    [FEATURE_STATE.OFF]: <Trans>Off</Trans>,
    [FEATURE_STATE.INCOMPLETE]: <Trans>Needs setup</Trans>,
  }[state];

  return (
    <div className='border-base-300 flex items-start justify-between gap-4 border-b py-2 last:border-b-0'>
      <div className='min-w-0'>
        <div className='text-sm font-medium'>{name}</div>
        <div className='text-base-content/70 text-xs'>{detail}</div>
      </div>
      <div className='flex shrink-0 items-center gap-2'>
        {value && <span className='font-mono text-xs tabular-nums'>{value}</span>}
        <span className={badge}>{label}</span>
      </div>
    </div>
  );
}
