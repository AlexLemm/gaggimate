// Shared reads of the experimental settings, so the status panel, the chart and the power panel all agree about what
// the machine is configured to do. Kept out of the components because none of it is presentation.

export const num = value => {
  const parsed = Number.parseFloat(value);
  return Number.isFinite(parsed) ? parsed : 0;
};

// Kff is the fourth PID coefficient, "Kp,Ki,Kd,Kff" on the Machine tab. Every thermal feature derives the element
// wattage from it (watts = 1000 / Kff) rather than from a field of its own, so there is one number to get right.
// A zero means none of them can know how much power a shot has available.
export const heaterWattsFromPid = pid => {
  const kff = num(String(pid ?? '').split(',')[3]);
  return kff > 0 ? 1000 / kff : 0;
};

// The prerequisite that is missing, or null when nothing is. Reported as the specific fix rather than a generic
// failure, because the two come from different places: the wattage from autotune or by hand on the Machine tab, the
// capacity from a field on this page.
export const thermalBlocker = (formData, t) => {
  if (heaterWattsFromPid(formData.pid) <= 0) {
    return t`Set the fourth PID coefficient (Kff) on the Machine tab — the element wattage comes from it.`;
  }
  if (num(formData.boilerHeatCapacity) <= 0) {
    return t`Set a boiler heat capacity below.`;
  }
  return null;
};

export const FEATURE_STATE = { ON: 'on', OFF: 'off', INCOMPLETE: 'incomplete' };

export const featureState = (cap, blocker) => {
  if (num(cap) <= 0) {
    return FEATURE_STATE.OFF;
  }
  return blocker ? FEATURE_STATE.INCOMPLETE : FEATURE_STATE.ON;
};
