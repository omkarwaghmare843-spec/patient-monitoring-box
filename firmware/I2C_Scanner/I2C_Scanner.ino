// Quick diagnostic: scans the I2C bus on GPIO21/22 and reports every address
// that responds. Use this to confirm the MAX30100 (expected 0x57) and OLED
// (expected 0x3C) are both actually visible on the bus before debugging the
// main HealthBox sketch further.
#include <Wire.h>

// Change these to match whichever bus you want to scan.
#define SCAN_SDA 18
#define SCAN_SCL 19

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(SCAN_SDA, SCAN_SCL);
  Serial.printf("I2C scanner starting on SDA=%d SCL=%d...\n", SCAN_SDA, SCAN_SCL);
}

void loop() {
  byte count = 0;
  Serial.println("Scanning...");
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    byte error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("Found device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      count++;
    }
  }
  if (count == 0) {
    Serial.println("No I2C devices found.");
  } else {
    Serial.print(count);
    Serial.println(" device(s) found.");
  }
  delay(3000);
}
