/*
  Standalone MAX30100 diagnostic sketch using the oxullo/Arduino-MAX30100
  library - no BLE, no OLED, no other sensors.

  Install via Arduino Library Manager: search "MAX30100lib" by OXullo
  Intersecans (https://github.com/oxullo/Arduino-MAX30100).

  This library's begin() hardcodes the global Wire instance (it has no
  overload accepting a custom TwoWire), so this standalone sketch wires
  the MAX30100 to the GLOBAL I2C bus (default pins 21/22 on most ESP32
  boards, or override with Wire.begin(SDA,SCL) below if your MAX30100 is
  on different pins). This is fine here because nothing else needs the
  bus in this test sketch. HealthBox.ino cannot use this library directly
  because it needs a second, independent bus for the MAX30100 (the OLED
  already occupies the global bus on 21/22) - once this sketch confirms
  real HR/SpO2 values, we'll port the proven init/read sequence into
  HealthBox.ino's own TwoWire(1) bus.

  Wiring: SDA=GPIO18  SCL=GPIO19 (set below)
*/

#include <Wire.h>
#include <MAX30100lib.h>

#define MAX_SDA 18
#define MAX_SCL 19

PulseOximeter pox;

uint32_t lastReportMs = 0;

void onBeatDetected() {
  Serial.println("Beat!");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== MAX30100 diagnostic (oxullo/Arduino-MAX30100 library) ===");

  Wire.begin(MAX_SDA, MAX_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  Serial.print("Initializing pulse oximeter... ");
  if (!pox.begin()) {
    Serial.println("FAILED - check wiring/power on SDA=18/SCL=19");
    while (true) delay(1000);
  }
  Serial.println("SUCCESS");

  pox.setOnBeatDetectedCallback(onBeatDetected);

  Serial.println("Place your finger on the sensor...\n");
}

void loop() {
  pox.update();

  if (millis() - lastReportMs > 1000) {
    lastReportMs = millis();
    Serial.printf("HR: %.1f bpm | SpO2: %.1f %%\n", pox.getHeartRate(), pox.getSpO2());
  }
}
