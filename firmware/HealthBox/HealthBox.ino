/*
  HealthBox firmware - ESP32-WROOM-32D
  Family patient monitoring box: reads Temperature, Heart Rate, SpO2, ECG,
  and EMG, shows live values on an onboard OLED, and streams them over BLE
  to the HealthBox web app.

  ---------------------------------------------------------------------------
  Wiring (per Schematic_health_box_2026-10-01.pdf + confirmed pin mapping):
  ---------------------------------------------------------------------------
    DS18B20 Temperature (OneWire) ..... GPIO23
    MAX30100 (HR / SpO2) .............. SDA=GPIO18  SCL=GPIO19
    SSD1306 OLED 128x64 ............... SDA=GPIO21  SCL=GPIO22
    ECG analog signal (AD8232-style) .. GPIO35 (ADC1_CH7, input-only)
    EMG analog signal ................. GPIO34 (ADC1_CH6, input-only)
    SW1 / SW2 .......................... EN / BOOT (board reset+flash, no firmware handling needed)

  The MAX30100 is driven by the real oxullo/Arduino-MAX30100 library
  (PulseOximeter, MAX30100_PulseOximeter.h), used exactly as tested -
  no custom register/algorithm code. That library hardcodes the global
  Wire instance internally, so the global Wire is assigned to the
  MAX30100 (SDA=18/SCL=19) and the OLED is moved onto its own TwoWire(1)
  bus (SDA=21/SCL=22) instead. The physical wiring is unchanged - this
  only swaps which ESP32 I2C peripheral number each sensor uses in code.

  The library's internal I2C clock is hardcoded to 400kHz, which was
  confirmed to corrupt reads on this hardware. Fixed by editing the
  installed library directly: change I2C_BUS_SPEED from 400000UL to
  100000UL in MAX30100.h (wherever the "MAX30100" library by OXullo
  Intersecans is installed). Reapply that edit if the library is
  reinstalled or updated.

  No other MAX30100-capable library works here: the SparkFun MAX3010x
  library (MAX30105.h) does accept a custom TwoWire bus, but its entire
  register map is shifted for the MAX30102/MAX30105 (e.g. FIFO_DATA at
  0x07 vs this chip's actual 0x05, MODE_CONFIG at 0x09 vs 0x06) - it's
  not just a part-ID check, so patching around the ID mismatch would
  silently read/write the wrong registers. That library is unusable
  with a true MAX30100 beyond the initial handshake.

  setupBle() runs before the MAX30100 probe (not after) so any timing
  jitter from BLE radio startup settles before I2C is touched - this
  firmware previously failed MAX30100 detection intermittently when
  combined with BLE/OLED even though a standalone sketch (no BLE, no
  OLED) detected it reliably every time.

  ---------------------------------------------------------------------------
  BLE protocol (matches public/js/ble-service.js on the web app):
  ---------------------------------------------------------------------------
    Service UUID:            4f3a0001-41a0-4a7a-9e2a-5c6b8f9d0a01

    Vitals characteristic:   4f3a0002-41a0-4a7a-9e2a-5c6b8f9d0a01  (NOTIFY, ~1 Hz)
      JSON: {"temp":36.8,"hr":78,"spo2":97}

    Waveform characteristic: 4f3a0003-41a0-4a7a-9e2a-5c6b8f9d0a01  (NOTIFY, ~10 Hz)
      JSON: {"ecg":[...10 raw ADC samples...],"emg":[...10 raw ADC samples...]}

  Required libraries (Arduino Library Manager):
    - OneWire
    - DallasTemperature
    - Adafruit GFX Library
    - Adafruit SSD1306
    - MAX30100 (OXullo Intersecans) - provides MAX30100_PulseOximeter.h
  Board core: esp32 by Espressif Systems (BLEDevice.h ships with the core)
  ---------------------------------------------------------------------------
*/

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <MAX30100_PulseOximeter.h>

// ----------------------------- Pin map -------------------------------------
#define PIN_TEMP_ONEWIRE   23
#define PIN_ECG_ADC        35
#define PIN_EMG_ADC        34
#define PIN_MAX30100_SDA   18
#define PIN_MAX30100_SCL   19
#define PIN_OLED_SDA       21
#define PIN_OLED_SCL       22

// ----------------------------- BLE UUIDs ------------------------------------
#define SERVICE_UUID            "4f3a0001-41a0-4a7a-9e2a-5c6b8f9d0a01"
#define VITALS_CHAR_UUID        "4f3a0002-41a0-4a7a-9e2a-5c6b8f9d0a01"
#define WAVEFORM_CHAR_UUID      "4f3a0003-41a0-4a7a-9e2a-5c6b8f9d0a01"
#define DEVICE_NAME             "HealthBox"

// ----------------------------- OLED (own TwoWire(1) bus) ------------------------------------
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_RESET   -1
#define OLED_ADDR    0x3C
TwoWire oledWire = TwoWire(1);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &oledWire, OLED_RESET);

// ----------------------------- MAX30100 (library, on the global Wire bus) ------------------------------------
PulseOximeter pox;
bool max30100Online = false;

// ----------------------------- Sensors ------------------------------------
OneWire oneWire(PIN_TEMP_ONEWIRE);
DallasTemperature tempSensor(&oneWire);

// ----------------------------- BLE state ------------------------------------
BLEServer* bleServer = nullptr;
BLECharacteristic* vitalsChar = nullptr;
BLECharacteristic* waveformChar = nullptr;
volatile bool deviceConnected = false;

// ----------------------------- Live readings ------------------------------------
float latestTemp = NAN;
float latestHr = NAN;
float latestSpo2 = NAN;

// ----------------------------- Timing ------------------------------------
const uint32_t TEMP_INTERVAL_MS     = 2000;
const uint32_t VITALS_SEND_MS       = 1000;
const uint32_t WAVEFORM_SAMPLE_MS   = 10;   // 100 Hz sampling
const uint8_t  WAVEFORM_BATCH_SIZE  = 10;   // notify every 10 samples (~10 Hz)
const uint32_t OLED_REFRESH_MS      = 500;

uint32_t lastTempRead = 0;
uint32_t lastVitalsSend = 0;
uint32_t lastWaveformSample = 0;
uint32_t lastOledRefresh = 0;

uint16_t ecgBuffer[WAVEFORM_BATCH_SIZE];
uint16_t emgBuffer[WAVEFORM_BATCH_SIZE];
uint8_t waveformIndex = 0;

// ----------------------------- BLE callbacks ------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    deviceConnected = true;
  }
  void onDisconnect(BLEServer* server) override {
    deviceConnected = false;
    // Restart advertising so the app can reconnect without a power cycle.
    BLEDevice::startAdvertising();
  }
};

void onBeatDetected() {
  Serial.println("[MAX30100] Beat!");
}

// ----------------------------- Setup ------------------------------------
void setupOled() {
  oledWire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oledWire.setTimeOut(50);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed - check wiring/address");
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("HealthBox");
  display.println("Starting...");
  display.display();
}

#define MAX30100_INIT_ATTEMPTS      6
#define MAX30100_INIT_RETRY_MS      500

bool setupMax30100() {
  Wire.begin(PIN_MAX30100_SDA, PIN_MAX30100_SCL);

  // Right after power-on, the MAX30100's internal LDO/oscillator can still be
  // settling, so the first begin() attempt(s) may fail even though the chip
  // is fine - a reset button press (no power-cycle) skips this and works on
  // the first try, which is the symptom this retry loop fixes. Settling is
  // a one-time condition, so retrying a few times covers it without masking
  // a genuine wiring/power fault (which will keep failing every attempt).
  for (uint8_t attempt = 1; attempt <= MAX30100_INIT_ATTEMPTS; attempt++) {
    if (pox.begin()) {
      pox.setOnBeatDetectedCallback(onBeatDetected);
      Serial.printf("MAX30100 initialized (attempt %u/%u).\n", attempt, MAX30100_INIT_ATTEMPTS);
      return true;
    }
    Serial.printf("MAX30100 not found on I2C bus (SDA=18/SCL=19), attempt %u/%u - retrying...\n",
      attempt, MAX30100_INIT_ATTEMPTS);
    delay(MAX30100_INIT_RETRY_MS);
  }

  Serial.println("MAX30100 init failed after all retries - check wiring/power.");
  return false;
}

void setupBle() {
  BLEDevice::init(DEVICE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService* service = bleServer->createService(SERVICE_UUID);

  vitalsChar = service->createCharacteristic(
    VITALS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  vitalsChar->addDescriptor(new BLE2902());

  waveformChar = service->createCharacteristic(
    WAVEFORM_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  waveformChar->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("BLE advertising started as \"" DEVICE_NAME "\"");
}

void setup() {
  Serial.begin(115200);
  delay(1500); // let I2C bus/sensors fully power up before probing

  pinMode(PIN_ECG_ADC, INPUT);
  pinMode(PIN_EMG_ADC, INPUT);
  analogReadResolution(12); // 0-4095

  tempSensor.begin();
  setupOled();

  // BLE radio startup is initialized before the MAX30100 I2C probe: starting
  // BLE can introduce brief timing jitter on shared interrupt/RTC resources,
  // and probing I2C while that's happening was a suspected cause of the
  // sensor working standalone but failing once BLE/other peripherals were
  // active. Getting BLE fully up first, then settling briefly, avoids that
  // window entirely.
  setupBle();
  delay(200);

  max30100Online = setupMax30100();

  Serial.println("HealthBox ready.");
}

// ----------------------------- Readers ------------------------------------
const uint32_t MAX30100_RETRY_INTERVAL_MS = 5000;
uint32_t lastMax30100RetryMs = 0;

void updateMax30100() {
  if (!max30100Online) {
    // Keep retrying in the background instead of requiring a manual reset -
    // covers the case where the sensor was still settling at boot (or was
    // unplugged/replugged) and becomes available later.
    uint32_t now = millis();
    if (now - lastMax30100RetryMs >= MAX30100_RETRY_INTERVAL_MS) {
      lastMax30100RetryMs = now;
      Serial.println("[MAX30100] Retrying init...");
      max30100Online = pox.begin();
      if (max30100Online) {
        pox.setOnBeatDetectedCallback(onBeatDetected);
        Serial.println("MAX30100 initialized (background retry).");
      }
    }
    return;
  }
  pox.update();
  latestHr = pox.getHeartRate();
  uint8_t spo2 = pox.getSpO2();
  if (spo2 > 0) latestSpo2 = spo2;
}

void updateTemperature() {
  tempSensor.requestTemperatures();
  float c = tempSensor.getTempCByIndex(0);
  if (c > -100.0f && c < 125.0f) { // DEVICE_DISCONNECTED_C guard
    latestTemp = c;
  }
}

void sampleWaveform() {
  ecgBuffer[waveformIndex] = analogRead(PIN_ECG_ADC);
  emgBuffer[waveformIndex] = analogRead(PIN_EMG_ADC);
  waveformIndex++;

  if (waveformIndex >= WAVEFORM_BATCH_SIZE) {
    sendWaveformBatch();
    waveformIndex = 0;
  }
}

// ----------------------------- BLE senders ------------------------------------
void sendVitals() {
  if (!deviceConnected) return;

  char json[96];
  // isnan()-guarded fields fall back to null so the app can show "no data"
  // instead of a stale/garbage number when a sensor hasn't reported yet.
  snprintf(json, sizeof(json),
    "{\"temp\":%s,\"hr\":%s,\"spo2\":%s}",
    isnan(latestTemp) ? "null" : String(latestTemp, 1).c_str(),
    (isnan(latestHr) || latestHr <= 0) ? "null" : String(latestHr, 0).c_str(),
    isnan(latestSpo2) ? "null" : String(latestSpo2, 0).c_str()
  );

  vitalsChar->setValue((uint8_t*)json, strlen(json));
  vitalsChar->notify();
}

void sendWaveformBatch() {
  if (!deviceConnected) return;

  String json = "{\"ecg\":[";
  for (uint8_t i = 0; i < WAVEFORM_BATCH_SIZE; i++) {
    json += ecgBuffer[i];
    if (i < WAVEFORM_BATCH_SIZE - 1) json += ",";
  }
  json += "],\"emg\":[";
  for (uint8_t i = 0; i < WAVEFORM_BATCH_SIZE; i++) {
    json += emgBuffer[i];
    if (i < WAVEFORM_BATCH_SIZE - 1) json += ",";
  }
  json += "]}";

  waveformChar->setValue((uint8_t*)json.c_str(), json.length());
  waveformChar->notify();
}

// ----------------------------- OLED ------------------------------------
void refreshOled() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println("HealthBox");
  display.println(deviceConnected ? "Status: Connected" : "Status: Advertising");
  display.println();

  display.print("Temp : ");
  display.print(isnan(latestTemp) ? "--" : String(latestTemp, 1));
  display.println(" C");

  if (max30100Online) {
    display.print("HR   : ");
    display.print((isnan(latestHr) || latestHr <= 0) ? "--" : String(latestHr, 0));
    display.println(" bpm");

    display.print("SpO2 : ");
    display.print(isnan(latestSpo2) ? "--" : String(latestSpo2, 0));
    display.println(" %");
  } else {
    display.println("HR   : sensor offline");
    display.println("SpO2 : retrying...");
  }

  display.display();
}

// ----------------------------- Loop ------------------------------------
void loop() {
  uint32_t now = millis();

  updateMax30100(); // pox.update() must be called often to drain the FIFO before it overflows

  if (now - lastTempRead >= TEMP_INTERVAL_MS) {
    lastTempRead = now;
    updateTemperature();
  }

  if (now - lastWaveformSample >= WAVEFORM_SAMPLE_MS) {
    lastWaveformSample = now;
    sampleWaveform();
  }

  if (now - lastVitalsSend >= VITALS_SEND_MS) {
    lastVitalsSend = now;
    sendVitals();
  }

  if (now - lastOledRefresh >= OLED_REFRESH_MS) {
    lastOledRefresh = now;
    refreshOled();
  }
}
