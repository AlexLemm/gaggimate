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

// Which prerequisite is missing, as a CODE rather than a message, or null when nothing is.
//
// Deliberately not translated here. Lingui's `t` is a compile-time macro: it is rewritten at the call site by the
// transform, which only runs where the macro is imported. Passing `t` in from a component and using it as a template
// tag in this plain module leaves it untransformed, so the string comes back empty and every caller silently reports
// the wrong state -- which is exactly what the status panel did on its first run against the machine.
//
// So the decision lives here and the wording lives in the component, where the macro can see it.
export const BLOCKER = { NEEDS_KFF: 'needs-kff', NEEDS_CAPACITY: 'needs-capacity' };

export const thermalBlocker = formData => {
  if (heaterWattsFromPid(formData.pid) <= 0) {
    return BLOCKER.NEEDS_KFF;
  }
  if (num(formData.boilerHeatCapacity) <= 0) {
    return BLOCKER.NEEDS_CAPACITY;
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
