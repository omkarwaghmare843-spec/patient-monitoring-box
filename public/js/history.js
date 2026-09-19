import { requireAuth } from './app-shell.js';
import { getStatus } from './vitals-ranges.js';
import { db, collection, query, orderBy, limit, getDocs } from './firebase-init.js';

const tbody = document.getElementById('historyBody');
const emptyState = document.getElementById('emptyState');
const tableCard = document.getElementById('tableCard');

const chartOptions = {
  responsive: true,
  maintainAspectRatio: false,
  plugins: { legend: { display: false } },
  scales: {
    x: { grid: { display: false }, ticks: { font: { size: 10 }, maxRotation: 0 } },
    y: { grid: { color: '#eef2f7' }, ticks: { font: { size: 11 } } },
  },
  elements: { point: { radius: 2 }, line: { tension: 0.35, borderWidth: 2 } },
};

function makeTrendChart(canvasId, color, softColor) {
  if (typeof Chart === 'undefined') {
    console.error('Chart.js failed to load; trend charts are disabled.');
    return null;
  }
  const ctx = document.getElementById(canvasId).getContext('2d');
  return new Chart(ctx, {
    type: 'line',
    data: { labels: [], datasets: [{ data: [], borderColor: color, backgroundColor: softColor, fill: true }] },
    options: chartOptions,
  });
}

const trendHr = makeTrendChart('trend-hr', '#ec4899', 'rgba(236,72,153,0.08)');
const trendSpo2 = makeTrendChart('trend-spo2', '#0ea5e9', 'rgba(14,165,233,0.08)');
const trendTemp = makeTrendChart('trend-temp', '#f97316', 'rgba(249,115,22,0.08)');

function formatDate(ts) {
  if (!ts) return '—';
  const date = ts.toDate ? ts.toDate() : new Date(ts);
  return date.toLocaleString(undefined, {
    year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit',
  });
}

function badgeCell(key, value, unit) {
  if (value === null || value === undefined) return `<span class="text-muted">—</span>`;
  const status = getStatus(key, value);
  return `<span class="badge badge-${status}">${value} ${unit}</span>`;
}

requireAuth(async (user) => {
  try {
    const q = query(
      collection(db, 'users', user.uid, 'checkups'),
      orderBy('recordedAt', 'desc'),
      limit(50)
    );
    const snap = await getDocs(q);

    if (snap.empty) {
      tableCard.querySelector('.table-wrap').hidden = true;
      emptyState.hidden = false;
      return;
    }

    const rows = [];
    snap.forEach((docSnap) => rows.push(docSnap.data()));

    // Table: most recent first
    tbody.innerHTML = rows
      .map(
        (r) => `
        <tr>
          <td>${formatDate(r.recordedAt)}</td>
          <td>${badgeCell('temperature', r.temperature, '°C')}</td>
          <td>${badgeCell('heartRate', r.heartRate, 'bpm')}</td>
          <td>${badgeCell('spo2', r.spo2, '%')}</td>
          <td>${r.ecg ?? '—'}</td>
          <td>${r.emg ?? '—'}</td>
        </tr>`
      )
      .join('');

    // Charts: chronological order (oldest -> newest)
    const chrono = [...rows].reverse();
    const labels = chrono.map((r) => {
      const d = r.recordedAt && r.recordedAt.toDate ? r.recordedAt.toDate() : new Date();
      return d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
    });

    if (trendHr) {
      trendHr.data.labels = labels;
      trendHr.data.datasets[0].data = chrono.map((r) => r.heartRate);
      trendHr.update();
    }

    if (trendSpo2) {
      trendSpo2.data.labels = labels;
      trendSpo2.data.datasets[0].data = chrono.map((r) => r.spo2);
      trendSpo2.update();
    }

    if (trendTemp) {
      trendTemp.data.labels = labels;
      trendTemp.data.datasets[0].data = chrono.map((r) => r.temperature);
      trendTemp.update();
    }
  } catch (err) {
    console.error('Failed to load history:', err);
    tableCard.querySelector('.table-wrap').hidden = true;
    emptyState.hidden = false;
    emptyState.querySelector('h4').textContent = 'Could not load history';
    emptyState.querySelector('p').textContent = 'Please check your connection and try refreshing the page.';
  }
});
