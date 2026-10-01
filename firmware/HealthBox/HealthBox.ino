/*
  HealthBox firmware - ESP32-WROOM-32D
  Family patient monitoring box: reads Temperature, Heart Rate, SpO2, ECG,
  and EMG, shows live values on an onboard OLED, and streams them over BLE
  to the HealthBox web app.

  ---------------------------------------------------------------------------
  Wiring (per Schematic_health_box_2026-10-01.pdf + confirmed pin mapping):
  ---------------------------------------------------------------------------
    DS18B20 Temperature (OneWire) ..... GPIO23
    MAX30100 (HR / SpO2) .............. SDA=GPIO18  SCL=GPIO19  (dedicated I2C bus)
    SSD1306 OLED 128x64 ............... SDA=GPIO21  SCL=GPIO22  (dedicated I2C bus)
    ECG analog signal (AD8232-style) .. GPIO35 (ADC1_CH7, input-only)
    EMG analog signal ................. GPIO34 (ADC1_CH6, input-only)
    SW1 / SW2 .......................... EN / BOOT (board reset+flash, no firmware handling needed)

  The MAX30100 is driven with a small direct-register driver in this sketch
  instead of an Arduino library, because both available libraries hardcode
  a call to the global Wire instance inside begin() with no way to pass a
  custom TwoWire bus - they physically cannot talk to a sensor wired to a
  second I2C bus (which this board requires, since the OLED already
  occupies the global bus on 21/22):
   - "MAX30100lib" (oxullo/Arduino-MAX30100) - also hardcoded its internal
     I2C clock to 400kHz, which corrupts reads on this hardware; fixed to
     100kHz when testing, confirmed working standalone.
   - The SparkFun MAX3010x library (MAX30105.h) targets the MAX30102/
     MAX30105 register map and failed to even detect this board's MAX30100.

  The init sequence (100kHz clock, SpO2+HR mode, hi-res 100Hz/1600us
  config) and the HR/SpO2 algorithm below are ported directly from
  oxullo/Arduino-MAX30100's internals (BeatDetector + DCRemover + 1-pole
  Butterworth low-pass + log-ratio SpO2 lookup table), since that
  algorithm was confirmed working (steady ~60-85bpm HR, ~94-97% SpO2)
  against this exact sensor via firmware/MAX30100_Test/MAX30100_Test.ino
  before being ported onto this sketch's own dedicated TwoWire(1) bus.

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
  Board core: esp32 by Espressif Systems (BLEDevice.h ships with the core)
  ---------------------------------------------------------------------------
*/

// Uncomment to print raw MAX30100 FIFO/IR/Red values to Serial for debugging.
#define MAX30100_DEBUG

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

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

// ----------------------------- OLED ------------------------------------
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_RESET   -1
#define OLED_ADDR    0x3C
TwoWire oledWire = TwoWire(0);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &oledWire, OLED_RESET);

// ----------------------------- MAX30100 direct driver ------------------------------------
#define MAX30100_ADDR                  0x57
#define MAX30100_REG_INT_STATUS        0x00
#define MAX30100_REG_INT_ENABLE        0x01
#define MAX30100_REG_FIFO_WR_PTR       0x02
#define MAX30100_REG_FIFO_OVF_CTR      0x03
#define MAX30100_REG_FIFO_RD_PTR       0x04
#define MAX30100_REG_FIFO_DATA         0x05
#define MAX30100_REG_MODE_CONFIG       0x06
#define MAX30100_REG_SPO2_CONFIG       0x07
#define MAX30100_REG_LED_CONFIG        0x09
#define MAX30100_REG_PART_ID           0xFF
#define MAX30100_EXPECTED_PART_ID      0x11

TwoWire maxWire = TwoWire(1);
bool max30100Online = false;

#define FINGER_PRESENT_THRESHOLD  20000UL

// ----------------------------- MAX30100 HR/SpO2 algorithm ------------------------------------
// Ported from oxullo/Arduino-MAX30100 (MAX30100_BeatDetector / MAX30100_Filters /
// MAX30100_SpO2Calculator), confirmed working against this sensor via
// firmware/MAX30100_Test/MAX30100_Test.ino before being adapted to this sketch's own bus.

// -- DC remover (one per LED channel) --
struct DCRemover {
  float alpha = 0.95f;
  float dcw = 0;
  float step(float x) {
    float oldDcw = dcw;
    dcw = x + alpha * dcw;
    return dcw - oldDcw;
  }
};
static DCRemover irDCRemover;
static DCRemover redDCRemover;

// -- 1-pole Butterworth low-pass, Fs=100Hz, Fc=6Hz (mirrors the library's FilterBuLp1) --
struct LowPassFilter {
  float v0 = 0, v1 = 0;
  float step(float x) {
    v0 = v1;
    v1 = (2.452372752527856026e-1f * x) + (0.50952544949442879485f * v0);
    return v0 + v1;
  }
};
static LowPassFilter lpf;

// -- Beat detector state machine --
enum BeatState { BEAT_INIT, BEAT_WAITING, BEAT_FOLLOWING_SLOPE, BEAT_MAYBE_DETECTED, BEAT_MASKING };
#define BEAT_INIT_HOLDOFF_MS        2000
#define BEAT_MASKING_HOLDOFF_MS     200
#define BEAT_BPFILTER_ALPHA         0.6f
#define BEAT_MIN_THRESHOLD          20.0f
#define BEAT_MAX_THRESHOLD          800.0f
#define BEAT_STEP_RESILIENCY        30.0f
#define BEAT_THRESHOLD_FALLOFF      0.3f
#define BEAT_THRESHOLD_DECAY        0.99f
#define BEAT_INVALID_READOUT_MS     2000
#define BEAT_SAMPLE_PERIOD_MS       10

static BeatState beatState = BEAT_INIT;
static float beatThreshold = BEAT_MIN_THRESHOLD;
static float beatPeriodMs = 0;
static float beatLastMaxValue = 0;
static uint32_t beatTsLastBeat = 0;

void beatDecreaseThreshold() {
  if (beatLastMaxValue > 0 && beatPeriodMs > 0) {
    beatThreshold -= beatLastMaxValue * (1 - BEAT_THRESHOLD_FALLOFF) / (beatPeriodMs / BEAT_SAMPLE_PERIOD_MS);
  } else {
    beatThreshold *= BEAT_THRESHOLD_DECAY;
  }
  if (beatThreshold < BEAT_MIN_THRESHOLD) beatThreshold = BEAT_MIN_THRESHOLD;
}

bool beatCheckForBeat(float sample) {
  bool beatDetected = false;

  switch (beatState) {
    case BEAT_INIT:
      if (millis() > BEAT_INIT_HOLDOFF_MS) beatState = BEAT_WAITING;
      break;

    case BEAT_WAITING:
      if (sample > beatThreshold) {
        beatThreshold = min(sample, BEAT_MAX_THRESHOLD);
        beatState = BEAT_FOLLOWING_SLOPE;
      }
      if (millis() - beatTsLastBeat > BEAT_INVALID_READOUT_MS) {
        beatPeriodMs = 0;
        beatLastMaxValue = 0;
      }
      beatDecreaseThreshold();
      break;

    case BEAT_FOLLOWING_SLOPE:
      if (sample < beatThreshold) {
        beatState = BEAT_MAYBE_DETECTED;
      } else {
        beatThreshold = min(sample, BEAT_MAX_THRESHOLD);
      }
      break;

    case BEAT_MAYBE_DETECTED:
      if (sample + BEAT_STEP_RESILIENCY < beatThreshold) {
        beatDetected = true;
        beatLastMaxValue = sample;
        beatState = BEAT_MASKING;
        float delta = millis() - beatTsLastBeat;
        if (delta) {
          beatPeriodMs = BEAT_BPFILTER_ALPHA * delta + (1 - BEAT_BPFILTER_ALPHA) * beatPeriodMs;
        }
        beatTsLastBeat = millis();
      } else {
        beatState = BEAT_FOLLOWING_SLOPE;
      }
      break;

    case BEAT_MASKING:
      if (millis() - beatTsLastBeat > BEAT_MASKING_HOLDOFF_MS) beatState = BEAT_WAITING;
      beatDecreaseThreshold();
      break;
  }

  return beatDetected;
}

float beatGetRate() {
  return beatPeriodMs != 0 ? (1.0f / beatPeriodMs * 1000.0f * 60.0f) : 0;
}

// -- SpO2 calculator: log-ratio of AC RMS, averaged every 3 beats, mapped through a LUT --
#define SPO2_CALC_EVERY_N_BEATS  3
static const uint8_t spO2LUT[43] = {
  100,100,100,100,99,99,99,99,99,99,98,98,98,98,
  98,97,97,97,97,97,97,96,96,96,96,96,96,95,95,
  95,95,95,95,94,94,94,94,94,93,93,93,93,93
};
static float spo2IrAcSqSum = 0, spo2RedAcSqSum = 0;
static uint8_t spo2BeatsDetected = 0;
static uint32_t spo2SamplesRecorded = 0;
static uint8_t currentSpo2 = 0;

void spo2Reset() {
  spo2SamplesRecorded = 0;
  spo2RedAcSqSum = 0;
  spo2IrAcSqSum = 0;
  spo2BeatsDetected = 0;
}

void spo2Update(float irAc, float redAc, bool beatDetected) {
  spo2IrAcSqSum += irAc * irAc;
  spo2RedAcSqSum += redAc * redAc;
  spo2SamplesRecorded++;

  if (beatDetected) {
    spo2BeatsDetected++;
    if (spo2BeatsDetected == SPO2_CALC_EVERY_N_BEATS) {
      float acSqRatio = 100.0f * log(spo2RedAcSqSum / spo2SamplesRecorded) / log(spo2IrAcSqSum / spo2SamplesRecorded);
      uint8_t index = 0;
      if (acSqRatio > 66) {
        index = (uint8_t)acSqRatio - 66;
      } else if (acSqRatio > 50) {
        index = (uint8_t)acSqRatio - 50;
      }
      spo2Reset();
      if (index < 43) currentSpo2 = spO2LUT[index];
    }
  }
}

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

// ----------------------------- MAX30100 register helpers ------------------------------------
void max30100WriteReg(uint8_t reg, uint8_t value) {
  maxWire.beginTransmission(MAX30100_ADDR);
  maxWire.write(reg);
  maxWire.write(value);
  maxWire.endTransmission();
}

uint8_t max30100ReadReg(uint8_t reg) {
  maxWire.beginTransmission(MAX30100_ADDR);
  maxWire.write(reg);
  maxWire.endTransmission(false);
  maxWire.requestFrom((uint8_t)MAX30100_ADDR, (uint8_t)1);
  return maxWire.available() ? maxWire.read() : 0;
}

bool setupMax30100() {
  uint8_t probeErr = 1;
  for (uint8_t attempt = 0; attempt < 3 && probeErr != 0; attempt++) {
    if (attempt > 0) delay(100);
    maxWire.beginTransmission(MAX30100_ADDR);
    probeErr = maxWire.endTransmission();
  }
  if (probeErr != 0) {
    Serial.println("MAX30100 not found on I2C bus (SDA=18/SCL=19) - check wiring/power.");
    return false;
  }

  uint8_t partId = max30100ReadReg(MAX30100_REG_PART_ID);
  if (partId != MAX30100_EXPECTED_PART_ID) {
    Serial.printf("MAX30100 part ID mismatch: got 0x%02X, expected 0x%02X\n", partId, MAX30100_EXPECTED_PART_ID);
    return false;
  }

  max30100WriteReg(MAX30100_REG_FIFO_WR_PTR, 0x00);
  max30100WriteReg(MAX30100_REG_FIFO_OVF_CTR, 0x00);
  max30100WriteReg(MAX30100_REG_FIFO_RD_PTR, 0x00);
  max30100WriteReg(MAX30100_REG_SPO2_CONFIG, 0x47);       // hi-res, 100Hz, 1600us pulse
  max30100WriteReg(MAX30100_REG_LED_CONFIG, 0x2F);        // IR/Red current ~7.6mA each
  delay(10);
  max30100WriteReg(MAX30100_REG_MODE_CONFIG, 0x03);       // SpO2 + HR mode, written last so sampling starts after everything else is configured
  delay(50);

#ifdef MAX30100_DEBUG
  Serial.printf("[MAX30100] readback MODE_CONFIG=0x%02X SPO2_CONFIG=0x%02X LED_CONFIG=0x%02X\n",
    max30100ReadReg(MAX30100_REG_MODE_CONFIG),
    max30100ReadReg(MAX30100_REG_SPO2_CONFIG),
    max30100ReadReg(MAX30100_REG_LED_CONFIG));
#endif

  Serial.println("MAX30100 initialized.");
  return true;
}

void updateMax30100() {
  if (!max30100Online) return;

  uint8_t writePtr = max30100ReadReg(MAX30100_REG_FIFO_WR_PTR);
  uint8_t readPtr = max30100ReadReg(MAX30100_REG_FIFO_RD_PTR);
  int8_t samplesAvailable = (int8_t)(writePtr - readPtr) & 0x0F;

#ifdef MAX30100_DEBUG
  static uint32_t lastDebugMs = 0;
  if (millis() - lastDebugMs > 1000) {
    lastDebugMs = millis();
    Serial.printf("[MAX30100] wrPtr=%u rdPtr=%u avail=%d\n", writePtr, readPtr, samplesAvailable);
  }
#endif

  for (int8_t i = 0; i < samplesAvailable; i++) {
    maxWire.beginTransmission(MAX30100_ADDR);
    maxWire.write(MAX30100_REG_FIFO_DATA);
    maxWire.endTransmission(false);
    maxWire.requestFrom((uint8_t)MAX30100_ADDR, (uint8_t)4);

    if (maxWire.available() < 4) break;

    uint16_t irSample = (maxWire.read() << 8) | maxWire.read();
    uint16_t redSample = (maxWire.read() << 8) | maxWire.read();

#ifdef MAX30100_DEBUG
    Serial.printf("[MAX30100] ir=%u red=%u\n", irSample, redSample);
#endif

    bool fingerPresent = irSample > FINGER_PRESENT_THRESHOLD;
    if (!fingerPresent) {
      continue;
    }

    float irAc = irDCRemover.step((float)irSample);
    float redAc = redDCRemover.step((float)redSample);

    // Signal fed to the beat detector is mirrored (cleanest monotonic spike is below zero).
    float filteredPulse = lpf.step(-irAc);
    bool beatDetected = beatCheckForBeat(filteredPulse);

    if (beatGetRate() > 0) {
      latestHr = beatGetRate();
      spo2Update(irAc, redAc, beatDetected);
      if (currentSpo2 > 0) latestSpo2 = currentSpo2;
    }

#ifdef MAX30100_DEBUG
    if (beatDetected) Serial.println("[MAX30100] Beat!");
#endif
  }
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
  delay(1500); // let I2C bus/sensors fully power up before probing (200ms was too short - MAX30100_Test.ino uses 1500ms and detects the sensor reliably)

  pinMode(PIN_ECG_ADC, INPUT);
  pinMode(PIN_EMG_ADC, INPUT);
  analogReadResolution(12); // 0-4095

  maxWire.begin(PIN_MAX30100_SDA, PIN_MAX30100_SCL);
  maxWire.setClock(100000); // conservative speed; 400kHz was producing noisy/corrupted reads
  maxWire.setTimeOut(50);   // bus hang -> timeout instead of a hard hang/crash

  tempSensor.begin();
  setupOled();
  max30100Online = setupMax30100();
  setupBle();

  Serial.println("HealthBox ready.");
}

// ----------------------------- Readers ------------------------------------
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
    isnan(latestHr) ? "null" : String(latestHr, 0).c_str(),
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

  display.print("HR   : ");
  display.print(isnan(latestHr) ? "--" : String(latestHr, 0));
  display.println(" bpm");

  display.print("SpO2 : ");
  display.print(isnan(latestSpo2) ? "--" : String(latestSpo2, 0));
  display.println(" %");

  display.display();
}

// ----------------------------- Loop ------------------------------------
void loop() {
  uint32_t now = millis();

  updateMax30100(); // must be called often to drain the FIFO before it overflows

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
