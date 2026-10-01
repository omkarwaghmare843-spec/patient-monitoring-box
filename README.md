# HealthBox — Family Patient Monitoring UI

A Node.js/Express web app for a shared ESP32-based health monitoring box. Each family member
signs in with their own account, connects to the single physical box over Bluetooth (Web
Bluetooth API), runs a checkup, and the vitals get saved to their own Firebase account — with
an AI-generated wellness note from Gemini after each checkup.

## Vitals tracked
1. Temperature (°C) — DS18B20 sensor
2. Heart Rate (bpm) — MAX30100 sensor
3. SpO₂ (%) — MAX30100 sensor
4. ECG — AD8232-style analog front-end, live waveform
5. EMG — analog front-end, live waveform
6. Blood Pressure (mmHg) — **entered manually** by the user (no BP sensor on the box)

## Stack
- **Server**: Node.js + Express + EJS (renders pages, injects Firebase config, hosts the AI analysis API)
- **Auth & Data**: Firebase Authentication (email/password) + Cloud Firestore, used directly from the browser via the Firebase Web SDK
- **AI insight**: Google Gemini, called server-side via Firebase Admin SDK after each checkup save
- **Bluetooth**: Web Bluetooth API (Chrome/Edge on desktop or Android) — connects directly from the browser to the ESP32, no extra server/bridge needed
- **Firmware**: ESP32-WROOM-32D Arduino sketch in [firmware/HealthBox](firmware/HealthBox) — see [firmware/README.md](firmware/README.md)
- **Charts**: Chart.js (loaded from CDN)

## Setup

### 1. Install dependencies
```bash
npm install
```

### 2. Create a Firebase project
1. Go to the [Firebase Console](https://console.firebase.google.com/) and create a new project.
2. Enable **Authentication** → Sign-in method → **Email/Password**.
3. Enable **Firestore Database** (start in production mode).
4. Under Project Settings → General → "Your apps", add a **Web app** and copy the config values.
5. Deploy the security rules in [firestore.rules](firestore.rules) (Firestore → Rules tab, paste and publish). These rules ensure each family member can only read/write their own profile and checkups, and that only the server (via Admin SDK, which bypasses rules) can write the AI insight fields.
6. Under Project Settings → **Service Accounts** → Generate new private key. This downloads a JSON file used by the server to verify sign-ins and read/write Firestore for the AI analysis route — **never commit this file or expose it to the browser**.

### 3. Get a Gemini API key
Get a free key from [Google AI Studio](https://aistudio.google.com/apikey). Used server-side only.

### 4. Configure environment variables
Copy `.env.example` to `.env` and fill in your Firebase config, the full service account JSON
(as one line) for `FIREBASE_SERVICE_ACCOUNT`, and your `GEMINI_API_KEY`:
```bash
cp .env.example .env
```

### 5. Run the app
```bash
npm start
# or for auto-reload during development:
npm run dev
```
Visit `http://localhost:3000`. Web Bluetooth requires either `localhost` or HTTPS — for testing on
a phone on your network, use a tool like `ngrok` or deploy behind HTTPS.

## Data model (Firestore)
```
users/{uid}                        - profile: { name, relation, email, createdAt }
users/{uid}/checkups/{checkupId}   - {
                                        temperature, heartRate, spo2, ecg, emg,
                                        bpSystolic, bpDiastolic,
                                        recordedAt,
                                        aiInsight, aiStatus  (written by the server only)
                                      }
```
Each family member has their own `uid` (from Firebase Auth) and only ever reads/writes documents
under their own `users/{uid}` path — enforced by `firestore.rules`. Clients can `create` a
checkup but never `update` one; `aiInsight`/`aiStatus` are added afterward by the server via
the Admin SDK (which Firestore rules don't apply to), so a client can never forge an AI result.

## AI health insight

After a checkup is saved, the dashboard calls `POST /api/analyze-checkup` with the saved
checkup's ID and the user's Firebase ID token. The server verifies the token, reads that
checkup from Firestore, sends the vitals to Gemini with a prompt that asks for a short,
cautious, non-diagnostic wellness note, and writes the result back to the same document as
`aiInsight`. The History page shows it as an expandable button per row. If `GEMINI_API_KEY` or
`FIREBASE_SERVICE_ACCOUNT` isn't configured, checkups still save fine — the AI insight request
just fails gracefully and the row shows "Unavailable".

## ESP32 firmware & BLE protocol

See [firmware/README.md](firmware/README.md) for the full wiring table, library list, and
upload instructions for the Arduino sketch in [firmware/HealthBox](firmware/HealthBox).

The firmware advertises as **`HealthBox`** with one BLE service and two notify characteristics,
matched exactly by [public/js/ble-service.js](public/js/ble-service.js):
- **Vitals** (~1 Hz): `{ "temp": 36.8, "hr": 78, "spo2": 97 }`
- **Waveform** (~10 Hz, batched): `{ "ecg": [...12-bit ADC samples...], "emg": [...] }`

Blood Pressure is never sent over BLE — there's no BP sensor on the board, so it's entered
manually in the web app.

## Browser support note
Web Bluetooth is supported in Chrome and Edge (desktop + Android), but **not** in Safari/iOS or
Firefox. If family members use iPhones, they can still sign in and view history/profile (and
enter BP manually), but the live "Connect to HealthBox" checkup flow needs a supported
browser/device.
