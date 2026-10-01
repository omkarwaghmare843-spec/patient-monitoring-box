/*
  Standalone MAX30100 diagnostic sketch - no BLE, no OLED, no other sensors.
  Flash this alone to isolate exactly what the MAX30100 chip is doing.
  This is the only sketch that should be edited while bringing up the
  MAX30100 - HealthBox.ino stays on its known-good direct-register driver
  (see firmware/README.md) until this sketch proves out a change.

  Wiring: SDA=GPIO18  SCL=GPIO19  (adjust below if different)

  What it does:
   1. Resets the chip (MODE_CONFIG reset bit) and waits for it to settle.
   2. Reads PART_ID to confirm the chip responds (expect 0x11).
   3. Configures mode/SpO2/LED registers, reading each one back immediately
      to prove the write actually landed.
   4. Polls the FIFO every loop (no delay) and drains every sample as soon
      as it's available, printing ir/red values live. The original version
      of this sketch polled every 500ms, which is slower than the FIFO
      fills at 100Hz (16 slots = ~160ms to fill) - it was overflowing
      between polls, not failing to sample. Draining continuously avoids
      that and prints real-time ir/red pairs so finger-presence and signal
      quality can be checked directly.
*/

#include <Wire.h>

#define MAX_SDA 18
#define MAX_SCL 19
#define MAX30100_ADDR 0x57

#define REG_INT_STATUS      0x00
#define REG_INT_ENABLE      0x01
#define REG_FIFO_WR_PTR     0x02
#define REG_FIFO_OVF_CTR    0x03
#define REG_FIFO_RD_PTR     0x04
#define REG_FIFO_DATA       0x05
#define REG_MODE_CONFIG     0x06
#define REG_SPO2_CONFIG     0x07
#define REG_LED_CONFIG      0x09
#define REG_PART_ID         0xFF
#define EXPECTED_PART_ID    0x11

#define FINGER_PRESENT_THRESHOLD  20000UL

TwoWire maxWire = TwoWire(1);

void writeReg(uint8_t reg, uint8_t val) {
  maxWire.beginTransmission(MAX30100_ADDR);
  maxWire.write(reg);
  maxWire.write(val);
  uint8_t err = maxWire.endTransmission();
  if (err != 0) {
    Serial.printf("  WRITE 0x%02X=0x%02X FAILED, I2C error %u\n", reg, val, err);
  }
}

uint8_t readReg(uint8_t reg) {
  maxWire.beginTransmission(MAX30100_ADDR);
  maxWire.write(reg);
  uint8_t err = maxWire.endTransmission(false);
  if (err != 0) {
    Serial.printf("  READ 0x%02X FAILED, I2C error %u\n", reg, err);
    return 0xFF;
  }
  maxWire.requestFrom((uint8_t)MAX30100_ADDR, (uint8_t)1);
  if (!maxWire.available()) {
    Serial.println("  READ FAILED, no data available");
    return 0xFF;
  }
  return maxWire.read();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== MAX30100 standalone diagnostic ===");

  maxWire.begin(MAX_SDA, MAX_SCL);
  maxWire.setClock(100000);
  maxWire.setTimeOut(50);

  Serial.println("\n-- Step 1: probe address --");
  maxWire.beginTransmission(MAX30100_ADDR);
  uint8_t probeErr = maxWire.endTransmission();
  Serial.printf("endTransmission() = %u (0 = device found)\n", probeErr);

  Serial.println("\n-- Step 2: part ID --");
  uint8_t partId = readReg(REG_PART_ID);
  Serial.printf("PART_ID = 0x%02X (expected 0x%02X)\n", partId, EXPECTED_PART_ID);

  Serial.println("\n-- Step 3: reset chip --");
  writeReg(REG_MODE_CONFIG, 0x40); // RESET bit (bit 6)
  delay(100);
  uint8_t afterReset = readReg(REG_MODE_CONFIG);
  Serial.printf("MODE_CONFIG after reset = 0x%02X (should self-clear to 0x00)\n", afterReset);

  Serial.println("\n-- Step 4: clear any pending interrupt --");
  uint8_t intStatus = readReg(REG_INT_STATUS);
  Serial.printf("INTERRUPT_STATUS (read clears it) = 0x%02X\n", intStatus);

  Serial.println("\n-- Step 5: configure --");
  writeReg(REG_SPO2_CONFIG, 0x47);   // hi-res, 100Hz, 1600us pulse width
  Serial.printf("SPO2_CONFIG readback = 0x%02X (expect 0x47)\n", readReg(REG_SPO2_CONFIG));

  writeReg(REG_LED_CONFIG, 0x2F);    // Red=7.6mA, IR=50mA (adjust if needed)
  Serial.printf("LED_CONFIG readback = 0x%02X (expect 0x2F)\n", readReg(REG_LED_CONFIG));

  writeReg(REG_FIFO_WR_PTR, 0x00);
  writeReg(REG_FIFO_OVF_CTR, 0x00);
  writeReg(REG_FIFO_RD_PTR, 0x00);
  Serial.printf("FIFO pointers reset: wr=0x%02X ovf=0x%02X rd=0x%02X\n",
    readReg(REG_FIFO_WR_PTR), readReg(REG_FIFO_OVF_CTR), readReg(REG_FIFO_RD_PTR));

  writeReg(REG_MODE_CONFIG, 0x03);   // SpO2 + HR mode - enables sampling
  Serial.printf("MODE_CONFIG readback = 0x%02X (expect 0x03)\n", readReg(REG_MODE_CONFIG));

  Serial.println("\n=== Setup complete. Draining FIFO continuously... ===\n");
}

void loop() {
  uint8_t writePtr = readReg(REG_FIFO_WR_PTR);
  uint8_t readPtr = readReg(REG_FIFO_RD_PTR);
  uint8_t ovf = readReg(REG_FIFO_OVF_CTR);
  int8_t samplesAvailable = (int8_t)(writePtr - readPtr) & 0x0F;

  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs > 1000) {
    lastStatusMs = millis();
    Serial.printf("[status] wr=%u rd=%u ovf=%u avail=%d MODE_CONFIG=0x%02X\n",
      writePtr, readPtr, ovf, samplesAvailable, readReg(REG_MODE_CONFIG));
  }

  for (int8_t i = 0; i < samplesAvailable; i++) {
    maxWire.beginTransmission(MAX30100_ADDR);
    maxWire.write(REG_FIFO_DATA);
    maxWire.endTransmission(false);
    maxWire.requestFrom((uint8_t)MAX30100_ADDR, (uint8_t)4);

    if (maxWire.available() < 4) break;

    uint16_t irSample = (maxWire.read() << 8) | maxWire.read();
    uint16_t redSample = (maxWire.read() << 8) | maxWire.read();

    bool fingerPresent = irSample > FINGER_PRESENT_THRESHOLD;
    Serial.printf("ir=%u red=%u %s\n", irSample, redSample, fingerPresent ? "(finger)" : "");
  }
}
