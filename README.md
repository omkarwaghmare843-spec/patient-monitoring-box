# HealthBox — Family Patient Monitoring UI

A Node.js/Express web app for a shared ESP32-based health monitoring box. Each family member
signs in with their own account, connects to the single physical box over Bluetooth (Web
Bluetooth API), runs a checkup, and the 5 vitals get saved to their own Firebase account.

## Vitals tracked
1. Temperature (°C)
2. EMG (µV)
3. ECG (mV)
4. Heart Rate (bpm)
5. SpO₂ (%)

## Stack
- **Server**: Node.js + Express + EJS (renders pages, injects Firebase config)
- **Auth & Data**: Firebase Authentication (email/password) + Cloud Firestore, used directly from the browser via the Firebase Web SDK
- **Bluetooth**: Web Bluetooth API (Chrome/Edge on desktop or Android) — connects directly from the browser to the ESP32, no extra server/bridge needed
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
5. Deploy the security rules in [firestore.rules](firestore.rules) (Firestore → Rules tab, paste and publish). These rules ensure each family member can only read/write their own profile and checkups.

### 3. Configure environment variables
Copy `.env.example` to `.env` and fill in your Firebase config values:
```bash
cp .env.example .env
```

### 4. Run the app
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
users/{uid}/checkups/{checkupId}   - { temperature, heartRate, spo2, ecg, emg, recordedAt }
```
Each family member has their own `uid` (from Firebase Auth) and only ever reads/writes documents
under their own `users/{uid}` path — enforced by `firestore.rules`.

## ESP32 firmware contract
The frontend expects a single BLE GATT characteristic that **notifies** a UTF-8 JSON string per
reading:
```json
{ "temp": 36.8, "emg": 512, "ecg": 1.2, "hr": 78, "spo2": 97 }
```

Configure these in [public/js/ble-service.js](public/js/ble-service.js) to match your firmware:
- `VITALS_SERVICE_UUID` — your custom BLE service UUID
- `VITALS_CHAR_UUID` — the notify characteristic UUID
- `DEVICE_NAME_PREFIX` — the BLE advertised name prefix (default: `HealthBox`)

If your firmware instead sends packed binary data, update `_handleValue()` in the same file to
decode it with `DataView` instead of `JSON.parse`.

## Browser support note
Web Bluetooth is supported in Chrome and Edge (desktop + Android), but **not** in Safari/iOS or
Firefox. If family members use iPhones, they can still sign in and view history/profile — only the
live "Connect to HealthBox" checkup flow needs a supported browser/device.
