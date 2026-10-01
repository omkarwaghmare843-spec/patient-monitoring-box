/*
  Standalone MAX30100 diagnostic sketch - no BLE, no OLED, no other sensors.
  Flash this alone to isolate exactly what the MAX30100 chip is doing.

  Wiring: SDA=GPIO18  SCL=GPIO19  (adjust below if different)

  What it does:
   1. Resets the chip (MODE_CONFIG reset bit) and waits for it to settle.
   2. Reads PART_ID to confirm the chip responds (expect 0x11).
   3. Configures mode/SpO2/LED registers, reading each one back immediately
      to prove the write actually landed.
   4. Every 500ms, dumps every relevant register's live value - including
      INTERRUPT_STATUS, which on some MAX30100 units must be read at least
      once to let the chip start filling its FIFO.
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
  Serial.printf("PART_ID = 0x%02X (expected 0x11)\n", partId);

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

  Serial.println("\n=== Setup complete. Watching FIFO pointer every 500ms... ===\n");
}

void loop() {
  uint8_t wr = readReg(REG_FIFO_WR_PTR);
  uint8_t rd = readReg(REG_FIFO_RD_PTR);
  uint8_t ovf = readReg(REG_FIFO_OVF_CTR);
  uint8_t mode = readReg(REG_MODE_CONFIG);
  uint8_t intSt = readReg(REG_INT_STATUS);

  Serial.printf("wr=%u rd=%u ovf=%u MODE_CONFIG=0x%02X INT_STATUS=0x%02X\n",
    wr, rd, ovf, mode, intSt);

  delay(500);
}
