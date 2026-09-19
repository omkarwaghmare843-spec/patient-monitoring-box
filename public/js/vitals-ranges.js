// Reference ranges used to badge a reading as normal / warning / danger.
// These are general adult resting reference ranges for at-home screening use only,
// not a clinical diagnostic threshold.
export const VITALS_META = {
  temperature: { label: 'Temperature', unit: '°C', icon: 'thermometer', normal: [36.1, 37.2], warning: [35.5, 38.0] },
  heartRate: { label: 'Heart Rate', unit: 'bpm', icon: 'heart', normal: [60, 100], warning: [50, 120] },
  spo2: { label: 'SpO₂', unit: '%', icon: 'droplet', normal: [95, 100], warning: [90, 100] },
  emg: { label: 'EMG', unit: 'µV', icon: 'activity', normal: [0, 600], warning: [0, 900] },
  ecg: { label: 'ECG', unit: 'mV', icon: 'pulse', normal: [0.5, 1.5], warning: [0.2, 2.0] },
};

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
