# HealthBox Firmware (ESP32)

Arduino sketch for the ESP32-WROOM-32D HealthBox hardware. Reads Temperature, Heart Rate,
SpO2, ECG, and EMG, shows live values on an onboard OLED, and streams them to the web app
over Bluetooth Low Energy.

## Wiring

| Function                  | ESP32 Pin        | Notes                                   |
|----------------------------|------------------|------------------------------------------|
| DS18B20 Temperature        | GPIO23           | OneWire, needs a 4.7kΩ pull-up to 3.3V on data line |
| MAX30100 (HR / SpO2) SDA   | GPIO18           | Global `Wire` bus (required by the MAX30100 library) |
| MAX30100 (HR / SpO2) SCL   | GPIO19           | Global `Wire` bus (required by the MAX30100 library) |
| SSD1306 OLED 128x64 SDA    | GPIO21           | Dedicated `TwoWire(1)` bus              |
| SSD1306 OLED 128x64 SCL    | GPIO22           | Dedicated `TwoWire(1)` bus              |
| ECG signal (AD8232-style)  | GPIO35           | ADC1 input-only pin                     |
| EMG signal                 | GPIO34           | ADC1 input-only pin                     |
| SW1 (EN) / SW2 (BOOT)      | EN / GPIO0       | Board reset + flash mode, no firmware handling needed |

Physical wiring is unchanged from the schematic (MAX30100 on 18/19, OLED on 21/22) — only
which ESP32 I2C peripheral number each sensor uses in code has changed.

The firmware drives the MAX30100 with the real
**[oxullo/Arduino-MAX30100](https://github.com/oxullo/Arduino-MAX30100)** library
(`PulseOximeter`, `MAX30100_PulseOximeter.h`) exactly as published — no custom register or
algorithm code. That library hardcodes the **global `Wire`** instance internally and has the
correct register map for a true MAX30100 chip, so the global `Wire` is assigned to the
MAX30100 here, and the OLED — which would normally sit on the global bus — is moved onto its
own `TwoWire(1)` instead (`Adafruit_SSD1306` supports a custom `TwoWire` pointer, so this
needed no OLED logic changes).

No other option was viable: the SparkFun MAX3010x library (`MAX30105.h`) does accept a custom
`TwoWire` bus, but its entire register map is shifted for the MAX30102/MAX30105 (e.g.
`FIFO_DATA` at `0x07` vs this chip's actual `0x05`, `MODE_CONFIG` at `0x09` vs `0x06`) — not
just a part-ID mismatch, so it can't be patched to work safely with a true MAX30100. A
from-scratch direct-register driver was also tried and technically worked at the raw I2C level
(confirmed part ID + a real pulsatile FIFO waveform via
[MAX30100_Test](MAX30100_Test/MAX30100_Test.ino)), but never reliably produced HR/SpO2 output
once combined with BLE/OLED in the full sketch, whereas the real library's own beat-detector
algorithm does.

**Known library bug, already patched locally:** `MAX30100.h` hardcodes its internal I2C clock
to 400kHz in `begin()`, which corrupts reads on this hardware. Fixed by editing the installed
library file directly — change `I2C_BUS_SPEED` from `400000UL` to `100000UL` in `MAX30100.h`
wherever the `MAX30100` library (OXullo Intersecans) is installed. Reapply this one-line edit
if the library is ever reinstalled or updated.

**Boot-time robustness:** `setupMax30100()` retries `pox.begin()` up to 6 times (500ms apart)
since the MAX30100's internal LDO/oscillator can still be settling right after power-on; if it
still isn't found, `updateMax30100()` keeps retrying every 5 seconds in the background for the
life of the sketch, so the sensor can come online without a manual reset. `setupBle()` also
runs *before* the MAX30100 probe, so any timing jitter from BLE radio startup settles first.

## Arduino IDE setup

1. Install the **esp32** board package (Espressif Systems) via Boards Manager.
2. Select board: **ESP32 Dev Module** (or "ESP32-WROOM-DA Module" if listed), Flash Size: **8MB**.
3. Install these libraries via Library Manager:
   - `OneWire` (Paul Stoffregen)
   - `DallasTemperature` (Miles Burton)
   - `Adafruit GFX Library`
   - `Adafruit SSD1306`
   - `MAX30100` (OXullo Intersecans) — remember to patch `I2C_BUS_SPEED` to `100000UL` (see above)
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
