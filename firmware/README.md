# HealthBox Firmware (ESP32)

Arduino sketch for the ESP32-WROOM-32D HealthBox hardware. Reads Temperature, Heart Rate,
SpO2, ECG, and EMG, shows live values on an onboard OLED, and streams them to the web app
over Bluetooth Low Energy.

## Wiring

| Function                  | ESP32 Pin        | Notes                                   |
|----------------------------|------------------|------------------------------------------|
| DS18B20 Temperature        | GPIO23           | OneWire, needs a 4.7kΩ pull-up to 3.3V on data line |
| MAX30100 (HR / SpO2) SDA   | GPIO18           | Dedicated I2C bus (ESP32 I2C peripheral #1) |
| MAX30100 (HR / SpO2) SCL   | GPIO19           | Dedicated I2C bus (ESP32 I2C peripheral #1) |
| SSD1306 OLED 128x64 SDA    | GPIO21           | Dedicated I2C bus (ESP32 I2C peripheral #0) |
| SSD1306 OLED 128x64 SCL    | GPIO22           | Dedicated I2C bus (ESP32 I2C peripheral #0) |
| ECG signal (AD8232-style)  | GPIO35           | ADC1 input-only pin                     |
| EMG signal                 | GPIO34           | ADC1 input-only pin                     |
| SW1 (EN) / SW2 (BOOT)      | EN / GPIO0       | Board reset + flash mode, no firmware handling needed |

MAX30100 and the OLED each get their own I2C bus, matching the original schematic. The
firmware talks to the MAX30100 through a small **direct-register driver built into the sketch**,
not an Arduino library:
- `MAX30100lib`'s `MAX30100::begin()` hardcodes the global `Wire` instance and can never talk
  to a sensor wired to a second I2C bus.
- The SparkFun MAX3010x library (`MAX30105.h`) does support a custom `TwoWire` bus, but its
  register map/init sequence targets the MAX30102/MAX30105 and failed `begin()` against this
  board's actual MAX30100 chip during testing ("MAX30100 not found on I2C bus").

Heart rate (zero-crossing detection on the IR AC signal) and SpO2 (ratio-of-ratios of the
Red/IR AC-over-DC ratios) are computed directly from the raw FIFO samples read over I2C, the
same approach proven working on a reference MAX30102 project. This direct-driver version is
confirmed working against the real hardware — see
[MAX30100_Test](MAX30100_Test/MAX30100_Test.ino) for the standalone diagnostic sketch used to
verify the part ID and FIFO fill independent of the main firmware. Keep hardware-level MAX30100
debugging in that standalone sketch rather than `HealthBox.ino` so the main firmware stays on
the known-good driver.

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
