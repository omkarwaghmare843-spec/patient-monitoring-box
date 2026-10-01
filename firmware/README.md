# HealthBox Firmware (ESP32)

Arduino sketch for the ESP32-WROOM-32D HealthBox hardware. Reads Temperature, Heart Rate,
SpO2, ECG, and EMG, shows live values on an onboard OLED, and streams them to the web app
over Bluetooth Low Energy.

## Wiring

| Function                  | ESP32 Pin        | Notes                                   |
|----------------------------|------------------|------------------------------------------|
| DS18B20 Temperature        | GPIO23           | OneWire, needs a 4.7kΩ pull-up to 3.3V on data line |
| MAX30100 (HR / SpO2) SDA   | GPIO18           | Dedicated `TwoWire(1)` bus              |
| MAX30100 (HR / SpO2) SCL   | GPIO19           | Dedicated `TwoWire(1)` bus              |
| SSD1306 OLED 128x64 SDA    | GPIO21           | Dedicated `TwoWire(0)` bus              |
| SSD1306 OLED 128x64 SCL    | GPIO22           | Dedicated `TwoWire(0)` bus              |
| ECG signal (AD8232-style)  | GPIO35           | ADC1 input-only pin                     |
| EMG signal                 | GPIO34           | ADC1 input-only pin                     |
| SW1 (EN) / SW2 (BOOT)      | EN / GPIO0       | Board reset + flash mode, no firmware handling needed |

MAX30100 and the OLED each get their own I2C bus — same principle as a working MAX30102
reference project on similar ESP32 hardware: one dedicated `TwoWire` bus per sensor avoids
address conflicts and bus-hang resets that occur when sensors share a bus, or when a library
hardcodes the bus it uses internally.

The MAX30100 is driven by a **direct-register driver built into the sketch**, not an Arduino
library — both available libraries (`MAX30100lib`/oxullo and SparkFun's MAX3010x) hardcode the
global `Wire` instance and can't be pointed at a second bus, and the SparkFun library's
register map (built for MAX30102/MAX30105) also failed to detect this board's actual MAX30100
chip. The driver, its 100kHz/timeout bus discipline, and its HR (zero-crossing on the IR AC
signal) / SpO2 (`R = (sqrt(sumRedAc)/avgRed) / (sqrt(sumIrAc)/avgIr)`, mapped through
`-23.3*(R-0.4)+100`) formulas all mirror a working MAX30102 reference project line-for-line.
Confirmed against this exact chip via [MAX30100_Test](MAX30100_Test/MAX30100_Test.ino) — part
ID reads back correctly and the FIFO fills with a real pulsatile waveform.

**Bus discipline, matching the reference exactly:**
- 100kHz clock, not 400kHz (400kHz corrupts reads on this hardware — confirmed by testing)
- `Wire.setTimeOut(50)` on both buses, so a stuck bus times out instead of hanging the sketch
- I2C address probe *before* touching any sensor registers, so a missing/mis-wired sensor is
  skipped cleanly at boot instead of hanging
- Both buses are brought up, and the MAX30100 is probed, *before* BLE starts

## Arduino IDE setup

1. Install the **esp32** board package (Espressif Systems) via Boards Manager.
2. Select board: **ESP32 Dev Module** (or "ESP32-WROOM-DA Module" if listed), Flash Size: **8MB**.
3. Install these libraries via Library Manager:
   - `OneWire` (Paul Stoffregen)
   - `DallasTemperature` (Miles Burton)
   - `Adafruit GFX Library`
   - `Adafruit SSD1306`
4. Open `HealthBox/HealthBox.ino`, select the correct COM port, and upload.
5. Open Serial Monitor at **115200 baud** to confirm `HealthBox ready.` and check for any
   sensor init warnings (OLED/MAX30100 wiring issues print a message instead of crashing).

If a sensor doesn't show up, flash [I2C_Scanner/I2C_Scanner.ino](I2C_Scanner/I2C_Scanner.ino)
first — it scans a given SDA/SCL pin pair and prints every I2C address that responds, which
quickly confirms whether a sensor is even electrically present before digging into firmware
logic. Edit the `SCAN_SDA`/`SCAN_SCL` defines at the top to check whichever bus you're
debugging.

## BLE protocol

The firmware advertises as **`HealthBox`** with one custom service and two notify
characteristics. [public/js/ble-service.js](../public/js/ble-service.js) in the web app is
pre-configured to match these exactly:

| | UUID |
|---|---|
| Service | `4f3a0001-41a0-4a7a-9e2a-5c6b8f9d0a01` |
| Vitals characteristic (notify, ~1 Hz) | `4f3a0002-41a0-4a7a-9e2a-5c6b8f9d0a01` |
| Waveform characteristic (notify, ~10 Hz) | `4f3a0003-41a0-4a7a-9e2a-5c6b8f9d0a01` |

**Vitals payload** (slow-changing numbers, shown as the live vital cards):
```json
{ "temp": 36.8, "hr": 78, "spo2": 97 }
```
A field is `null` until that sensor has produced its first valid reading.

**Waveform payload** (raw 12-bit ADC samples for the ECG/EMG live charts, batched 10 at a
time, sampled at 100 Hz / notified at ~10 Hz):
```json
{ "ecg": [512, 515, 520, ...], "emg": [300, 305, 298, ...] }
```

Blood Pressure is **not** part of the BLE protocol — there's no BP sensor on this board, so
it's entered manually by the user in the web app and saved straight to Firestore alongside
the BLE-sourced readings.

## Changing the BLE UUIDs

If you regenerate these UUIDs, update them in both this sketch (the `#define ..._UUID` lines
near the top) and `public/js/ble-service.js` (`VITALS_SERVICE_UUID`, `VITALS_CHAR_UUID`,
`WAVEFORM_CHAR_UUID`) — they must match exactly or the web app won't find the characteristics.
