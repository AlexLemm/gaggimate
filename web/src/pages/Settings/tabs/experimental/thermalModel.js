// Shared reads of the experimental settings, so the status panel, the chart and the power panel all agree about what
// the machine is configured to do. Kept out of the components because none of it is presentation.

export const num = value => {
  const parsed = Number.parseFloat(value);
  return Number.isFinite(parsed) ? parsed : 0;
};

// Every thermal feature derives the element wattage from the feedforward gain, Kff, as 1000 divided by it, rather
// than from a field of its own -- one number to get right, and no way for them to disagree about it. A zero means
// none of them can know how much power a shot has available.
//
// Takes the whole form rather than a pid string, because the Settings page does not keep one.
// transformFetchedSettings splits the stored "Kp,Ki,Kd,Kff" into `pid`, holding the first three, and `kf` holding the
// gain, then rejoins them on save. Reading a fourth part of `pid` therefore finds nothing and silently reports a
// machine with no element -- which is how the status panel came to say "set Kff" on a machine whose Kff was 1.111,
// while the power readout beside it was correctly showing 904 W from the same settings.
export const heaterWatts = formData => {
  const kff = num(formData?.kf ?? String(formData?.pid ?? '').split(',')[3]);
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
  if (heaterWatts(formData) <= 0) {
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
