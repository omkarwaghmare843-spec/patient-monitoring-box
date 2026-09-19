import { requireAuth, toast } from './app-shell.js';
import { bleService } from './ble-service.js';
import { getStatus } from './vitals-ranges.js';
import { db, collection, addDoc, serverTimestamp } from './firebase-init.js';

const MAX_POINTS = 30;

const el = {
  statusDot: document.getElementById('statusDot'),
  statusTitle: document.getElementById('statusTitle'),
  statusSubtitle: document.getElementById('statusSubtitle'),
  connectBtn: document.getElementById('connectBtn'),
  disconnectBtn: document.getElementById('disconnectBtn'),
  saveBtn: document.getElementById('saveBtn'),
};

let currentUser = null;
let latestReading = null;
let hasAnyReading = false;

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

function makeChart(canvasId, color, softColor) {
  if (typeof Chart === 'undefined') {
    console.error('Chart.js failed to load; live trend charts are disabled.');
    return null;
  }
  const ctx = document.getElementById(canvasId).getContext('2d');
  return new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{ data: [], borderColor: color, backgroundColor: softColor, fill: true }],
    },
    options: chartDefaults,
  });
}

const charts = {
  hr: makeChart('chart-hr', '#ec4899', 'rgba(236,72,153,0.08)'),
  spo2: makeChart('chart-spo2', '#0ea5e9', 'rgba(14,165,233,0.08)'),
  ecg: makeChart('chart-ecg', '#e11d48', 'rgba(225,29,72,0.08)'),
  emg: makeChart('chart-emg', '#8b5cf6', 'rgba(139,92,246,0.08)'),
  temp: makeChart('chart-temp', '#f97316', 'rgba(249,115,22,0.08)'),
};

function pushPoint(chart, value) {
  if (!chart) return;
  const ds = chart.data.datasets[0];
  chart.data.labels.push('');
  ds.data.push(value);
  if (ds.data.length > MAX_POINTS) {
    ds.data.shift();
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
    el.statusSubtitle.textContent = 'Streaming live vitals from HealthBox.';
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

bleService.onData = (reading) => {
  latestReading = reading;
  hasAnyReading = true;
  el.saveBtn.disabled = false;

  document.getElementById('val-temp').textContent = reading.temperature ?? '--';
  document.getElementById('val-hr').textContent = reading.heartRate ?? '--';
  document.getElementById('val-spo2').textContent = reading.spo2 ?? '--';
  document.getElementById('val-ecg').textContent = reading.ecg ?? '--';
  document.getElementById('val-emg').textContent = reading.emg ?? '--';

  renderBadge('temperature', reading.temperature);
  renderBadge('heartRate', reading.heartRate);
  renderBadge('spo2', reading.spo2);
  renderBadge('ecg', reading.ecg);
  renderBadge('emg', reading.emg);

  if (reading.heartRate !== null) pushPoint(charts.hr, reading.heartRate);
  if (reading.spo2 !== null) pushPoint(charts.spo2, reading.spo2);
  if (reading.ecg !== null) pushPoint(charts.ecg, reading.ecg);
  if (reading.emg !== null) pushPoint(charts.emg, reading.emg);
  if (reading.temperature !== null) pushPoint(charts.temp, reading.temperature);
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
  if (!currentUser || !latestReading) return;

  el.saveBtn.disabled = true;
  el.saveBtn.innerHTML = '<span class="spinner dark"></span> Saving…';

  try {
    await addDoc(collection(db, 'users', currentUser.uid, 'checkups'), {
      temperature: latestReading.temperature,
      heartRate: latestReading.heartRate,
      spo2: latestReading.spo2,
      ecg: latestReading.ecg,
      emg: latestReading.emg,
      recordedAt: serverTimestamp(),
    });
    toast('Checkup saved successfully.', 'success');
  } catch (err) {
    console.error(err);
    toast('Failed to save checkup. Please try again.', 'error');
  } finally {
    el.saveBtn.innerHTML = `
      <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2Z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>
      Save Checkup`;
    el.saveBtn.disabled = !hasAnyReading;
  }
});
