/*
  Standalone MAX30100 diagnostic sketch using the oxullo/Arduino-MAX30100
  library - no BLE, no OLED, no other sensors.

  Install via Arduino Library Manager: search "MAX30100" by OXullo
  Intersecans (https://github.com/oxullo/Arduino-MAX30100). Provides
  MAX30100_PulseOximeter.h / class PulseOximeter (not MAX30100.h, which is
  only the low-level FIFO driver this library builds on top of).

  This library's begin() hardcodes the global Wire instance (it has no
  overload accepting a custom TwoWire). Its internal Wire.begin() call
  (no pin args) re-uses whichever pins were already set by our own
  Wire.begin(SDA,SCL) call beforehand - ESP32's Wire.begin(-1,-1) reuses
  the last configured pins rather than resetting to board defaults, so
  that part is fine.

  The real bug this sketch worked around: MAX30100.h hardcodes
  I2C_BUS_SPEED to 400000 (400kHz) and the library applies that speed
  unconditionally in begin(), silently overriding any Wire.setClock()
  call made beforehand. We already proved 400kHz causes corrupted
  reads/bus crashes on this exact hardware (see HealthBox.ino's own
  100kHz fix). Fixed by editing the installed library at
  C:\ArduinoLibs\libraries\MAX30100\src\MAX30100.h, changing
  I2C_BUS_SPEED from 400000UL to 100000UL - if you reinstall or update
  this library later, reapply that one-line change.

  This is fine to use the global bus here because nothing else needs it
  in this test sketch. HealthBox.ino does NOT use this library - it
  needs a second, independent bus for the MAX30100 (the OLED has its
  own dedicated bus too), which this library can't target. HealthBox.ino
  uses a direct-register driver instead (see its header comment), whose
  init sequence and HR/SpO2 math mirror a separately-proven working
  MAX30102 reference project. This sketch stays useful purely as a
  library-based cross-check: if HR/SpO2 work here but not in
  HealthBox.ino, the problem is in the direct driver, not the sensor.

  Wiring: SDA=GPIO18  SCL=GPIO19 (set below)
*/

#include <Wire.h>
#include <MAX30100_PulseOximeter.h>

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
    Serial.printf("HR: %.1f bpm | SpO2: %u %%\n", pox.getHeartRate(), pox.getSpO2());
  }
}
