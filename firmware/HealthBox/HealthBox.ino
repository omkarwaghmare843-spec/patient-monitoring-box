/*
  HealthBox firmware - ESP32-WROOM-32D
  Family patient monitoring box: reads Temperature, Heart Rate, SpO2, ECG,
  and EMG, shows live values on an onboard OLED, and streams them over BLE
  to the HealthBox web app.

  ---------------------------------------------------------------------------
  Wiring (per Schematic_health_box_2026-10-01.pdf + confirmed pin mapping):
  ---------------------------------------------------------------------------
    DS18B20 Temperature (OneWire) ..... GPIO23
    MAX30100 (HR / SpO2) + SSD1306 OLED, shared I2C bus .. SDA=GPIO21  SCL=GPIO22
      (MAX30100lib v1.2.x always uses the default Wire instance internally,
      so it can't run on a separate TwoWire bus - both devices share this
      one bus instead, which I2C supports fine since they use different
      addresses: MAX30100 0x57, SSD1306 0x3C. GPIO18/19 are unused.)
    ECG analog signal (AD8232-style) .. GPIO35 (ADC1_CH7, input-only)
    EMG analog signal ................. GPIO34 (ADC1_CH6, input-only)
    SW1 / SW2 .......................... EN / BOOT (board reset+flash, no firmware handling needed)

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
    - Adafruit MAX30100 (by OXullo Intersecans - "MAX30100lib")
    - Adafruit GFX Library
    - Adafruit SSD1306
  Board core: esp32 by Espressif Systems (BLEDevice.h ships with the core)
  ---------------------------------------------------------------------------
*/

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <MAX30100_PulseOximeter.h>
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
#define PIN_I2C_SDA        21  // shared bus: MAX30100 + SSD1306 OLED
#define PIN_I2C_SCL        22

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
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

// ----------------------------- Sensors ------------------------------------
OneWire oneWire(PIN_TEMP_ONEWIRE);
DallasTemperature tempSensor(&oneWire);

PulseOximeter pox; // shares the default Wire bus with the OLED

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

// ----------------------------- Setup ------------------------------------
void setupOled() {
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

void setupMax30100() {
  if (!pox.begin()) {
    Serial.println("MAX30100 init failed - check wiring");
    return;
  }
  pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
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
  delay(200);

  pinMode(PIN_ECG_ADC, INPUT);
  pinMode(PIN_EMG_ADC, INPUT);
  analogReadResolution(12); // 0-4095

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL); // shared bus for OLED + MAX30100

  tempSensor.begin();
  setupOled();
  setupMax30100();
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

void updatePulseOx() {
  pox.update();
  float hr = pox.getHeartRate();
  float spo2 = pox.getSpO2();
  if (hr > 0) latestHr = hr;
  if (spo2 > 0) latestSpo2 = spo2;
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

  updatePulseOx(); // must be called as often as possible for MAX30100 accuracy

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
