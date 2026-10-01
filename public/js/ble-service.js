// Web Bluetooth service for the ESP32 HealthBox.
//
// Matches the GATT layout in firmware/HealthBox/HealthBox.ino exactly - see
// firmware/README.md for the protocol. If you change the UUIDs or payload
// shape in the firmware, update them here too.
//
//   Service:               SERVICE_UUID
//   Vitals characteristic:   VITALS_CHAR_UUID    (notify, ~1 Hz)
//     { "temp": 36.8, "hr": 78, "spo2": 97 }
//   Waveform characteristic: WAVEFORM_CHAR_UUID  (notify, ~10 Hz, 10 samples/batch)
//     { "ecg": [...12-bit ADC samples...], "emg": [...12-bit ADC samples...] }

const SERVICE_UUID = '4f3a0001-41a0-4a7a-9e2a-5c6b8f9d0a01';
const VITALS_CHAR_UUID = '4f3a0002-41a0-4a7a-9e2a-5c6b8f9d0a01';
const WAVEFORM_CHAR_UUID = '4f3a0003-41a0-4a7a-9e2a-5c6b8f9d0a01';
const DEVICE_NAME_PREFIX = 'HealthBox';

class BleService {
  constructor() {
    this.device = null;
    this.server = null;
    this.vitalsChar = null;
    this.waveformChar = null;
    this.onVitals = null;
    this.onWaveform = null;
    this.onStatusChange = null;
    this._boundDisconnect = this._handleDisconnect.bind(this);
  }

  isSupported() {
    return typeof navigator !== 'undefined' && !!navigator.bluetooth;
  }

  async connect() {
    if (!this.isSupported()) {
      throw new Error('Web Bluetooth is not supported in this browser. Use Chrome or Edge on desktop/Android.');
    }

    this._setStatus('connecting');

    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: DEVICE_NAME_PREFIX }, { services: [SERVICE_UUID] }],
      optionalServices: [SERVICE_UUID],
    });

    this.device.addEventListener('gattserverdisconnected', this._boundDisconnect);

    this.server = await this.device.gatt.connect();
    const service = await this.server.getPrimaryService(SERVICE_UUID);

    this.vitalsChar = await service.getCharacteristic(VITALS_CHAR_UUID);
    await this.vitalsChar.startNotifications();
    this.vitalsChar.addEventListener('characteristicvaluechanged', (event) => {
      this._handleVitals(event.target.value);
    });

    this.waveformChar = await service.getCharacteristic(WAVEFORM_CHAR_UUID);
    await this.waveformChar.startNotifications();
    this.waveformChar.addEventListener('characteristicvaluechanged', (event) => {
      this._handleWaveform(event.target.value);
    });

    this._setStatus('connected');
    return this.device.name || 'HealthBox';
  }

  disconnect() {
    if (this.device && this.device.gatt.connected) {
      this.device.gatt.disconnect();
    } else {
      this._setStatus('disconnected');
    }
  }

  _handleDisconnect() {
    this.vitalsChar = null;
    this.waveformChar = null;
    this.server = null;
    this._setStatus('disconnected');
  }

  _handleVitals(dataView) {
    try {
      const payload = JSON.parse(new TextDecoder('utf-8').decode(dataView));
      if (this.onVitals) {
        this.onVitals({
          temperature: this._num(payload.temp),
          heartRate: this._num(payload.hr),
          spo2: this._num(payload.spo2),
          timestamp: Date.now(),
        });
      }
    } catch (err) {
      console.error('Failed to parse BLE vitals payload:', err);
    }
  }

  _handleWaveform(dataView) {
    try {
      const payload = JSON.parse(new TextDecoder('utf-8').decode(dataView));
      if (this.onWaveform) {
        this.onWaveform({
          ecg: Array.isArray(payload.ecg) ? payload.ecg.map(Number) : [],
          emg: Array.isArray(payload.emg) ? payload.emg.map(Number) : [],
          timestamp: Date.now(),
        });
      }
    } catch (err) {
      console.error('Failed to parse BLE waveform payload:', err);
    }
  }

  _num(v) {
    const n = Number(v);
    return Number.isFinite(n) ? n : null;
  }

  _setStatus(status) {
    if (this.onStatusChange) this.onStatusChange(status);
  }
}

export const bleService = new BleService();
