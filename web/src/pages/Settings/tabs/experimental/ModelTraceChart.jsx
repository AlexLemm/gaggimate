import { t } from '@lingui/core/macro';
import { ChartComponent } from '../../../../components/Chart.jsx';
import { machine } from '../../../../services/ApiService.js';

// The measurement and the model on one axis.
//
// The firmware runs the same boiler model the compensations reason about (src/display/util/BoilerObserver.h) in
// parallel with the real boiler, stepped from its own previous state and never corrected from the sensor. So the gap
// between the two traces is not a display artefact -- it is how wrong the model is, live. That matters because the
// thermal bank and the recovery compute their offsets from exactly those parameters: a bank that comes out too large
// and a model trace that runs hot are the same fact seen twice.
//
// Three temperature series, and the third is the interesting one. "Model boiler" is the water temperature, which no
// machine can measure: the sensor sits on the outside of the boiler and lags it by seconds. During a shot the water is
// already several degrees down while the sensor has barely moved, and that gap is the whole reason the PID reacts too
// late and the compensations are needed at all.
//
// Estimated power rides on its own axis, from the duties the firmware commands and the ratings measured at the plug.

// `model` marks a series the firmware only computes while the observer is running. Those are gated on the frame's own
// modelRunning flag rather than on the value, because a model that is not running reports 0 and 0 is also a
// temperature -- reading one as the other is what left this chart blank with nothing to say about why.
const SERIES = [
  { key: 'currentTemperature', label: () => t`Measured`, color: '#ef4444', axis: 'temp', width: 2 },
  {
    key: 'targetTemperature',
    label: () => t`Setpoint`,
    color: '#94a3b8',
    axis: 'temp',
    width: 1,
    dashed: true,
  },
  {
    key: 'modelTemperature',
    label: () => t`Model sensor`,
    color: '#3b82f6',
    axis: 'temp',
    width: 2,
    dashed: true,
    model: true,
  },
  {
    key: 'modelBoilerTemperature',
    label: () => t`Model water`,
    color: '#8b5cf6',
    axis: 'temp',
    width: 1,
    model: true,
  },
  {
    key: 'estimatedPower',
    label: () => t`Estimated power`,
    color: '#f59e0b',
    axis: 'power',
    width: 1,
  },
];

// A frame carries a usable model state when the firmware says the observer is running. Older firmware does not send
// the flag at all, so a non-zero model temperature stands in for it rather than hiding the trace.
const modelRan = entry =>
  entry.modelRunning === undefined
    ? Number.isFinite(entry.modelTemperature) && entry.modelTemperature > 0
    : !!entry.modelRunning;

function buildChart(history) {
  // Gaps are left as gaps (spanGaps is false below): a stretch where the model was not running, or a temperature of
  // absolute zero that can only be a missing reading, is not something to draw a line across.
  const point = (entry, series) => {
    const value = entry[series.key];
    if (!Number.isFinite(value)) {
      return null;
    }
    if (series.model ? !modelRan(entry) : value === 0 && series.axis === 'temp') {
      return null;
    }
    return { x: new Date(entry.timestamp), y: value };
  };

  return {
    type: 'line',
    data: {
      datasets: SERIES.map(series => ({
        label: series.label(),
        data: history.map(entry => point(entry, series)).filter(Boolean),
        borderColor: series.color,
        backgroundColor: series.color,
        borderWidth: series.width,
        borderDash: series.dashed ? [4, 4] : undefined,
        yAxisID: series.axis,
        pointRadius: 0,
        tension: 0.2,
        spanGaps: false,
      })),
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: { display: true, labels: { boxWidth: 12, font: { size: 11 } } },
        tooltip: { enabled: true },
      },
      scales: {
        x: {
          type: 'time',
          time: { unit: 'second' },
          ticks: {
            maxTicksLimit: 8,
            font: { size: 10 },
            callback: value => `-${Math.ceil((Date.now() - value) / 1000)}s`,
          },
        },
        temp: {
          type: 'linear',
          position: 'left',
          title: { display: true, text: '°C', font: { size: 10 } },
          ticks: { font: { size: 10 } },
        },
        power: {
          type: 'linear',
          position: 'right',
          beginAtZero: true,
          title: { display: true, text: 'W', font: { size: 10 } },
          grid: { drawOnChartArea: false },
          ticks: { font: { size: 10 } },
        },
      },
    },
  };
}

export function ModelTraceChart() {
  const history = machine.value.history ?? [];

  // Say which of the two it is. An empty chart used to mean either "no telemetry yet" or "the model cannot run with
  // the settings it has", and the message named only the second -- so a machine that was simply not connected read as
  // a misconfigured one.
  if (history.length === 0) {
    return (
      <div className='text-base-content/60 flex h-48 items-center justify-center p-4 text-center text-sm'>
        {t`No telemetry yet. The chart fills as status frames arrive from the machine.`}
      </div>
    );
  }
  if (!history.some(modelRan)) {
    return (
      <div className='text-base-content/60 flex h-48 items-center justify-center p-4 text-center text-sm'>
        {t`The model is not running. It needs the element wattage (Kff on the Machine tab) and a boiler heat capacity; with both set it runs continuously, idle included.`}
      </div>
    );
  }

  return (
    <ChartComponent
      className='h-64 w-full lg:h-80'
      chartClassName='h-full w-full'
      data={buildChart(history)}
    />
  );
}
