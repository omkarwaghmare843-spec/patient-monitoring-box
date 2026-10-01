import { requireAuth, toast } from './app-shell.js';
import { bleService } from './ble-service.js';
import { getStatus } from './vitals-ranges.js';
import { db, collection, addDoc, serverTimestamp } from './firebase-init.js';

const MAX_POINTS = 30;
const MAX_WAVEFORM_POINTS = 150; // ~15s of ECG/EMG at the 10Hz batch rate

// Each parameter records for its own duration, then averages the samples
// collected during that window. Durations reflect how each sensor behaves:
// HR/SpO2 need a few seconds for the MAX30100's beat detector to lock on,
// temperature responds quickly, and ECG/EMG need a few seconds of waveform
// to be a meaningful trace rather than a single instantaneous sample.
const RECORD_DURATIONS_MS = {
  temperature: 5000,
  heartRate: 15000,
  spo2: 15000,
  ecg: 10000,
  emg: 10000,
};

const PARAM_LABELS = {
  temperature: 'Temperature',
  heartRate: 'Heart Rate',
  spo2: 'SpO2',
  ecg: 'ECG',
  emg: 'EMG',
};

const el = {
  statusDot: document.getElementById('statusDot'),
  statusTitle: document.getElementById('statusTitle'),
  statusSubtitle: document.getElementById('statusSubtitle'),
  connectBtn: document.getElementById('connectBtn'),
  disconnectBtn: document.getElementById('disconnectBtn'),
  saveBtn: document.getElementById('saveBtn'),
  bpSystolic: document.getElementById('bpSystolic'),
  bpDiastolic: document.getElementById('bpDiastolic'),
};

let currentUser = null;

// Final recorded (averaged) value per parameter - only these are saved.
// null until that parameter's recording has completed at least once.
const recorded = {
  temperature: null,
  heartRate: null,
  spo2: null,
  ecg: null,
  emg: null,
};

// Live (unrecorded) readings, used only to drive the dashboard display/charts
// while connected - never saved directly, only recorded values are saved.
const latest = {
  temperature: null,
  heartRate: null,
  spo2: null,
  ecg: null,
  emg: null,
};

// Recording session state per parameter.
const sessions = {};
for (const key of Object.keys(RECORD_DURATIONS_MS)) {
  sessions[key] = { active: false, samples: [], startedAt: 0, timerId: null, rafId: null };
}

requireAuth((user) => {
  currentUser = user;
});

if (!bleService.isSupported()) {
  el.statusSubtitle.textContent = 'Web Bluetooth is not supported in this browser. Try Chrome or Edge on desktop/Android.';
  el.connectBtn.disabled = true;
}

// ----- Chart setup -----
const chartDefaults = {
  responsive: true,
  maintainAspectRatio: false,
  animation: false,
  plugins: { legend: { display: false } },
  scales: {
    x: { display: false },
    y: { grid: { color: '#eef2f7' }, ticks: { font: { size: 11 } } },
  },
  elements: { point: { radius: 0 }, line: { tension: 0.35, borderWidth: 2 } },
};

function makeChart(canvasId, color, softColor, maxPoints) {
  if (typeof Chart === 'undefined') {
    console.error('Chart.js failed to load; live trend charts are disabled.');
    return null;
  }
  const ctx = document.getElementById(canvasId).getContext('2d');
  const chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{ data: [], borderColor: color, backgroundColor: softColor, fill: true }],
    },
    options: chartDefaults,
  });
  chart._maxPoints = maxPoints;
  return chart;
}

const charts = {
  hr: makeChart('chart-hr', '#ec4899', 'rgba(236,72,153,0.08)', MAX_POINTS),
  spo2: makeChart('chart-spo2', '#0ea5e9', 'rgba(14,165,233,0.08)', MAX_POINTS),
  ecg: makeChart('chart-ecg', '#e11d48', 'rgba(225,29,72,0.08)', MAX_WAVEFORM_POINTS),
  emg: makeChart('chart-emg', '#8b5cf6', 'rgba(139,92,246,0.08)', MAX_WAVEFORM_POINTS),
  temp: makeChart('chart-temp', '#f97316', 'rgba(249,115,22,0.08)', MAX_POINTS),
};

function pushPoint(chart, value) {
  if (!chart) return;
  const ds = chart.data.datasets[0];
  chart.data.labels.push('');
  ds.data.push(value);
  const cap = chart._maxPoints || MAX_POINTS;
  if (ds.data.length > cap) {
    ds.data.shift();
    chart.data.labels.shift();
  }
  chart.update('none');
}

function pushPoints(chart, values) {
  if (!chart || !values || !values.length) return;
  values.forEach((v) => {
    chart.data.labels.push('');
    chart.data.datasets[0].data.push(v);
  });
  const cap = chart._maxPoints || MAX_POINTS;
  while (chart.data.datasets[0].data.length > cap) {
    chart.data.datasets[0].data.shift();
    chart.data.labels.shift();
  }
  chart.update('none');
}

// ----- Badge rendering -----
function renderBadge(key, value) {
  const status = getStatus(key, value);
  const badgeEl = document.getElementById(`badge-${key}`);
  if (!badgeEl) return;
  const labelMap = { normal: 'Normal', warning: 'Watch', danger: 'Alert', neutral: 'No data' };
  badgeEl.innerHTML = `<span class="badge badge-${status}">${labelMap[status]}</span>`;
}

// ----- Recording controls (per parameter) -----
function average(arr) {
  if (!arr.length) return null;
  return arr.reduce((a, b) => a + b, 0) / arr.length;
}

function renderRecordControls(key) {
  const container = document.getElementById(`record-${key}`);
  const card = document.getElementById(`card-${cardSuffix(key)}`);
  if (!container) return;
  const session = sessions[key];
  const isDone = recorded[key] !== null && !session.active;

  card?.classList.toggle('is-recording', session.active);
  card?.classList.toggle('is-done', isDone);

  if (session.active) {
    const elapsed = Date.now() - session.startedAt;
    const duration = RECORD_DURATIONS_MS[key];
    const pct = Math.min(100, (elapsed / duration) * 100);
    const remainingS = Math.max(0, Math.ceil((duration - elapsed) / 1000));
    container.innerHTML = `
      <div class="record-progress">
        <div class="record-progress-track"><div class="record-progress-fill" style="width:${pct}%"></div></div>
        <div class="record-progress-label">Recording&hellip; ${remainingS}s left</div>
      </div>`;
    return;
  }

  if (isDone) {
    container.innerHTML = `
      <div class="record-done">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>
        Recorded
      </div>
      <button type="button" class="record-btn" data-record="${key}" style="margin-top:6px;">Record again</button>`;
    return;
  }

  container.innerHTML = `<button type="button" class="record-btn" data-record="${key}">Start Record</button>`;
}

function cardSuffix(key) {
  return key === 'heartRate' ? 'hr' : key === 'spo2' ? 'spo2' : key === 'temperature' ? 'temp' : key;
}

function startRecording(key) {
  if (!bleService.isSupported || sessions[key].active) return;
  const session = sessions[key];
  session.active = true;
  session.samples = [];
  session.startedAt = Date.now();

  renderRecordControls(key);

  const tick = () => {
    if (!session.active) return;
    renderRecordControls(key);
    session.rafId = requestAnimationFrame(tick);
  };
  session.rafId = requestAnimationFrame(tick);

  session.timerId = setTimeout(() => finishRecording(key), RECORD_DURATIONS_MS[key]);
}

function finishRecording(key) {
  const session = sessions[key];
  session.active = false;
  if (session.rafId) cancelAnimationFrame(session.rafId);
  if (session.timerId) clearTimeout(session.timerId);

  const value = average(session.samples);
  if (value === null) {
    toast(`No ${PARAM_LABELS[key]} data was captured - make sure the sensor is placed correctly and try again.`, 'error');
    renderRecordControls(key);
    updateSaveButtonState();
    return;
  }

  recorded[key] = Math.round(value * 10) / 10;
  toast(`${PARAM_LABELS[key]} recorded.`, 'success');
  renderRecordControls(key);
  updateSaveButtonState();
}

function updateSaveButtonState() {
  const allDone = Object.keys(RECORD_DURATIONS_MS).every((key) => recorded[key] !== null);
  el.saveBtn.disabled = !allDone;
  el.saveBtn.title = allDone ? '' : 'Record every parameter above before saving.';
}

document.querySelectorAll('.record-controls').forEach((container) => {
  container.addEventListener('click', (ev) => {
    const btn = ev.target.closest('[data-record]');
    if (!btn) return;
    startRecording(btn.dataset.record);
  });
});

for (const key of Object.keys(RECORD_DURATIONS_MS)) {
  renderRecordControls(key);
}
updateSaveButtonState();

// ----- BLE status handling -----
bleService.onStatusChange = (status) => {
  el.statusDot.className = 'status-dot';
  if (status === 'connecting') {
    el.statusDot.classList.add('connecting');
    el.statusTitle.textContent = 'Connecting…';
    el.statusSubtitle.textContent = 'Waiting for device selection and pairing.';
    el.connectBtn.disabled = true;
  } else if (status === 'connected') {
    el.statusDot.classList.add('connected');
    el.statusTitle.textContent = 'Connected';
    el.statusSubtitle.textContent = 'Start each test below when you\'re ready.';
    el.connectBtn.hidden = true;
    el.disconnectBtn.hidden = false;
    el.connectBtn.disabled = false;
  } else if (status === 'error') {
    el.statusDot.classList.add('error');
    el.statusTitle.textContent = 'Connection Error';
    el.statusSubtitle.textContent = 'Could not connect. Please try again.';
    resetConnectUI();
  } else {
    el.statusTitle.textContent = 'Not Connected';
    el.statusSubtitle.textContent = 'Tap "Connect" to pair with your HealthBox device';
    resetConnectUI();
  }
};

function resetConnectUI() {
  el.connectBtn.hidden = false;
  el.connectBtn.disabled = false;
  el.disconnectBtn.hidden = true;
}

// ----- Vitals (temp/hr/spo2, ~1Hz) -----
bleService.onVitals = (reading) => {
  latest.temperature = reading.temperature;
  latest.heartRate = reading.heartRate;
  latest.spo2 = reading.spo2;

  document.getElementById('val-temp').textContent = reading.temperature ?? '--';
  document.getElementById('val-hr').textContent = reading.heartRate ?? '--';
  document.getElementById('val-spo2').textContent = reading.spo2 ?? '--';

  renderBadge('temperature', reading.temperature);
  renderBadge('heartRate', reading.heartRate);
  renderBadge('spo2', reading.spo2);

  if (reading.heartRate !== null) pushPoint(charts.hr, reading.heartRate);
  if (reading.spo2 !== null) pushPoint(charts.spo2, reading.spo2);
  if (reading.temperature !== null) pushPoint(charts.temp, reading.temperature);

  if (sessions.temperature.active && reading.temperature !== null) sessions.temperature.samples.push(reading.temperature);
  if (sessions.heartRate.active && reading.heartRate !== null) sessions.heartRate.samples.push(reading.heartRate);
  if (sessions.spo2.active && reading.spo2 !== null) sessions.spo2.samples.push(reading.spo2);
};

// ----- Waveform (ecg/emg sample batches, ~10Hz) -----
bleService.onWaveform = (batch) => {
  if (batch.ecg.length) {
    latest.ecg = batch.ecg[batch.ecg.length - 1];
    document.getElementById('val-ecg').textContent = latest.ecg;
    pushPoints(charts.ecg, batch.ecg);
    if (sessions.ecg.active) sessions.ecg.samples.push(...batch.ecg);
  }

  if (batch.emg.length) {
    latest.emg = batch.emg[batch.emg.length - 1];
    document.getElementById('val-emg').textContent = latest.emg;
    pushPoints(charts.emg, batch.emg);
    if (sessions.emg.active) sessions.emg.samples.push(...batch.emg);
  }
};

// ----- Connect / Disconnect actions -----
el.connectBtn.addEventListener('click', async () => {
  try {
    const deviceName = await bleService.connect();
    toast(`Connected to ${deviceName}`, 'success');
  } catch (err) {
    console.error(err);
    if (err.name !== 'NotFoundError') {
      toast(err.message || 'Failed to connect to HealthBox.', 'error');
    }
    bleService.onStatusChange('disconnected');
  }
});

el.disconnectBtn.addEventListener('click', () => {
  bleService.disconnect();
});

// ----- Save checkup to Firestore -----
el.saveBtn.addEventListener('click', async () => {
  if (!currentUser) return;

  const systolic = el.bpSystolic.value ? Number(el.bpSystolic.value) : null;
  const diastolic = el.bpDiastolic.value ? Number(el.bpDiastolic.value) : null;

  el.saveBtn.disabled = true;
  el.saveBtn.innerHTML = '<span class="spinner dark"></span> Saving…';

  try {
    const docRef = await addDoc(collection(db, 'users', currentUser.uid, 'checkups'), {
      temperature: recorded.temperature,
      heartRate: recorded.heartRate,
      spo2: recorded.spo2,
      ecg: recorded.ecg,
      emg: recorded.emg,
      bpSystolic: systolic,
      bpDiastolic: diastolic,
      recordedAt: serverTimestamp(),
    });
    toast('Checkup saved. Analyzing with AI…', 'success');
    el.bpSystolic.value = '';
    el.bpDiastolic.value = '';

    // Reset recorded values so the next checkup starts fresh.
    for (const key of Object.keys(recorded)) recorded[key] = null;
    for (const key of Object.keys(RECORD_DURATIONS_MS)) renderRecordControls(key);

    requestAiAnalysis(currentUser.uid, docRef.id);
  } catch (err) {
    console.error(err);
    toast('Failed to save checkup. Please try again.', 'error');
  } finally {
    el.saveBtn.innerHTML = `
      <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2Z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>
      Save Checkup`;
    updateSaveButtonState();
  }
});

// ----- AI analysis (fire-and-forget; result shows up next time History loads) -----
async function requestAiAnalysis(uid, checkupId) {
  try {
    const idToken = await currentUser.getIdToken();
    const res = await fetch('/api/analyze-checkup', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${idToken}` },
      body: JSON.stringify({ checkupId }),
    });
    if (!res.ok) throw new Error(`Analysis request failed (${res.status})`);
    toast('AI health insight ready — view it in History.', 'success');
  } catch (err) {
    console.error('AI analysis request failed:', err);
    toast('Could not generate AI insight for this checkup.', 'error');
  }
}
