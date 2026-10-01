// Reference ranges used to badge a reading as normal / warning / danger.
// These are general adult resting reference ranges for at-home screening use only,
// not a clinical diagnostic threshold.
//
// ECG/EMG are intentionally excluded: the firmware streams raw 12-bit ADC counts
// (0-4095) rather than a calibrated mV/uV value, so there's no meaningful
// normal/danger threshold to badge them against - they're shown as live signal
// traces only (see the waveform charts), not as graded vitals.
export const VITALS_META = {
  temperature: { label: 'Temperature', unit: '°C', icon: 'thermometer', normal: [36.1, 37.2], warning: [35.5, 38.0] },
  heartRate: { label: 'Heart Rate', unit: 'bpm', icon: 'heart', normal: [60, 100], warning: [50, 120] },
  spo2: { label: 'SpO₂', unit: '%', icon: 'droplet', normal: [95, 100], warning: [90, 100] },
};

// Blood pressure needs two values (systolic/diastolic) so it can't use the
// single-range getStatus() below - graded per standard adult BP categories.
export function getBpStatus(systolic, diastolic) {
  if (!Number.isFinite(systolic) || !Number.isFinite(diastolic)) return 'neutral';
  if (systolic >= 180 || diastolic >= 120) return 'danger';
  if (systolic >= 140 || diastolic >= 90) return 'warning';
  if (systolic < 90 || diastolic < 60) return 'warning';
  return 'normal';
}

export function getStatus(key, value) {
  if (value === null || value === undefined || Number.isNaN(value)) return 'neutral';
  const meta = VITALS_META[key];
  if (!meta) return 'neutral';
  const [nLow, nHigh] = meta.normal;
  const [wLow, wHigh] = meta.warning;
  if (value >= nLow && value <= nHigh) return 'normal';
  if (value >= wLow && value <= wHigh) return 'warning';
  return 'danger';
}
