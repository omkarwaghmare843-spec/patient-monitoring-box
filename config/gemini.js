const { GoogleGenerativeAI } = require('@google/generative-ai');

let client = null;
function getClient() {
  if (client) return client;
  const apiKey = process.env.GEMINI_API_KEY;
  if (!apiKey) {
    throw new Error('GEMINI_API_KEY environment variable is not set.');
  }
  client = new GoogleGenerativeAI(apiKey);
  return client;
}

// Builds a short, plain-language wellness note from one checkup's readings.
// This is an at-home screening aid, not a diagnosis - the prompt explicitly
// steers the model away from diagnostic claims.
async function analyzeCheckup(vitals) {
  const genAI = getClient();
  const model = genAI.getGenerativeModel({ model: 'gemini-2.0-flash' });

  const lines = [];
  if (vitals.temperature !== null && vitals.temperature !== undefined) lines.push(`Temperature: ${vitals.temperature} °C`);
  if (vitals.heartRate !== null && vitals.heartRate !== undefined) lines.push(`Heart rate: ${vitals.heartRate} bpm`);
  if (vitals.spo2 !== null && vitals.spo2 !== undefined) lines.push(`SpO2: ${vitals.spo2} %`);
  if (vitals.bpSystolic !== null && vitals.bpSystolic !== undefined && vitals.bpDiastolic !== null && vitals.bpDiastolic !== undefined) {
    lines.push(`Blood pressure: ${vitals.bpSystolic}/${vitals.bpDiastolic} mmHg`);
  }
  if (vitals.ecg !== null && vitals.ecg !== undefined) lines.push(`ECG raw sensor reading: ${vitals.ecg} (12-bit ADC count, not calibrated mV)`);
  if (vitals.emg !== null && vitals.emg !== undefined) lines.push(`EMG raw sensor reading: ${vitals.emg} (12-bit ADC count, not calibrated)`);

  if (lines.length === 0) {
    return 'Not enough sensor data was captured for this checkup to generate an insight.';
  }

  const prompt = `You are a cautious at-home health screening assistant. A family member used a home monitoring device and recorded these readings:

${lines.join('\n')}

Write a short (3-4 sentences, plain language, no markdown) wellness note for them:
- Note anything outside typical resting adult ranges and why it might be worth a second look.
- If everything looks within typical ranges, say so reassuringly.
- Always end by reminding them this is not a medical diagnosis and a doctor should be consulted for any concerning or persistent readings.
- Do not use alarming language. Do not diagnose a condition by name.`;

  const result = await model.generateContent(prompt);
  const text = result.response.text().trim();
  return text || 'AI analysis did not return a result for this checkup.';
}

module.exports = { analyzeCheckup };
