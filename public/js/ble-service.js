// Web Bluetooth service for the ESP32 patient monitoring box.
//
// Expected GATT layout on the ESP32 (customize UUIDs to match your firmware):
//   Service:        VITALS_SERVICE_UUID
//   Characteristic: VITALS_CHAR_UUID (notify) - sends a UTF-8 JSON string per reading:
//     { "temp": 36.8, "emg": 512, "ecg": 1.2, "hr": 78, "spo2": 97 }
//
// Swap the UUIDs and parsePayload() below if your firmware uses a different format.

const VITALS_SERVICE_UUID = '0000181a-0000-1000-8000-00805f9b34fb'; // placeholder: Environmental Sensing-style custom service
const VITALS_CHAR_UUID = '00002a6e-0000-1000-8000-00805f9b34fb'; // placeholder characteristic UUID
const DEVICE_NAME_PREFIX = 'HealthBox';

class BleService {
  constructor() {
    this.device = null;
    this.server = null;
    this.characteristic = null;
    this.onData = null;
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
      filters: [{ namePrefix: DEVICE_NAME_PREFIX }, { services: [VITALS_SERVICE_UUID] }],
      optionalServices: [VITALS_SERVICE_UUID],
    });

    this.device.addEventListener('gattserverdisconnected', this._boundDisconnect);

    this.server = await this.device.gatt.connect();
    const service = await this.server.getPrimaryService(VITALS_SERVICE_UUID);
    this.characteristic = await service.getCharacteristic(VITALS_CHAR_UUID);

    await this.characteristic.startNotifications();
    this.characteristic.addEventListener('characteristicvaluechanged', (event) => {
      this._handleValue(event.target.value);
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
    this.characteristic = null;
    this.server = null;
    this._setStatus('disconnected');
  }

  _handleValue(dataView) {
    try {
      const text = new TextDecoder('utf-8').decode(dataView);
      const payload = JSON.parse(text);
      const reading = this._normalize(payload);
      if (this.onData) this.onData(reading);
    } catch (err) {
      console.error('Failed to parse BLE payload:', err);
    }
  }

  _normalize(payload) {
    return {
      temperature: this._num(payload.temp ?? payload.temperature),
      emg: this._num(payload.emg),
      ecg: this._num(payload.ecg),
      heartRate: this._num(payload.hr ?? payload.heartRate),
      spo2: this._num(payload.spo2),
      timestamp: Date.now(),
    };
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
