// SHTC3 temperature/humidity sensor, on the board's shared I2C bus at 0x70.
//
// Like rtc.cpp this is written straight against the datasheet rather than
// pulled in as a library, so lib_deps stays a single entry.
//
// The part sleeps between measurements at ~0.5 uA and has to be woken first;
// a measurement that skips the wake-up command is NAKed, which is the usual
// reason a working sensor looks absent.
//
// Command set used here (16-bit, MSB first):
//   0x3517  wake up          (>= 240 us before the next command)
//   0xB098  sleep
//   0x7866  measure, T first, normal mode, clock stretching disabled
//   0xEFC8  read ID          (bits 5:0 and 11 identify the SHTC3)
//
// Accuracy note: the sensor sits on the same board as an ESP32-S3 that is
// running Wi-Fi in bursts, so the reading is the temperature *inside the
// enclosure*, typically a degree or two above the room. SHTC3_TEMP_OFFSET is
// there to correct that once you have something to compare against.

#include <Arduino.h>
#include <Wire.h>

#include "app.h"
#include "board_pins.h"

#define SHTC3_TEMP_OFFSET 0.0f  // degrees C, subtracted from every reading

static bool s_present = false;

static bool command(uint16_t cmd) {
  Wire.beginTransmission(I2C_ADDR_SHTC3);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

// CRC-8, polynomial 0x31, initial value 0xFF — the one the datasheet appends
// to each 16-bit word. Checking it is what tells a real reading apart from a
// bus that answered with 0xFFFF because nothing drove it.
static uint8_t crc8(uint8_t msb, uint8_t lsb) {
  uint8_t crc = 0xFF;
  const uint8_t data[2] = {msb, lsb};
  for (int i = 0; i < 2; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

bool shtc3Begin() {
  // Wire.begin() belongs to rtcBegin(), which runs first and passes this
  // board's pins. Calling it again here would be harmless but would also hide
  // the ordering, so it is deliberately not repeated.
  //
  // Answering the wake-up command is what counts as present: nothing else on
  // this board lives at 0x70, so an ACK is already conclusive.
  if (!command(0x3517)) return false;
  delayMicroseconds(300);
  s_present = true;

  // The ID is read as a cross-check only. A part that ACKs but reports an
  // unexpected ID is still worth reading from — it is far more likely to be a
  // batch this code has not seen than a different chip — so this only warns.
  if (command(0xEFC8) && Wire.requestFrom((int)I2C_ADDR_SHTC3, 3) == 3) {
    const uint8_t msb = Wire.read(), lsb = Wire.read(), crc = Wire.read();
    const uint16_t id = (uint16_t)(msb << 8 | lsb);
    // Datasheet: bit 11 and bits 5:0 are fixed for the SHTC3.
    if (crc8(msb, lsb) != crc || (id & 0x083F) != 0x0807) {
      Serial.printf("shtc3: unexpected ID 0x%04X, reading it anyway\n", id);
    }
  }

  command(0xB098);  // back to sleep
  return s_present;
}

bool shtc3Present() { return s_present; }

bool shtc3Read(float *tempC, float *humidity) {
  if (!s_present) return false;

  if (!command(0x3517)) return false;  // wake up
  delayMicroseconds(300);

  if (!command(0x7866)) return false;  // T first, normal mode, no stretching
  // Normal-mode conversion is 12.1 ms worst case. With clock stretching
  // disabled the sensor NAKs a read that arrives early, so this wait is the
  // whole synchronisation.
  delay(13);

  if (Wire.requestFrom((int)I2C_ADDR_SHTC3, 6) != 6) {
    command(0xB098);
    return false;
  }

  uint8_t tMsb = Wire.read(), tLsb = Wire.read(), tCrc = Wire.read();
  uint8_t hMsb = Wire.read(), hLsb = Wire.read(), hCrc = Wire.read();

  command(0xB098);  // sleep again

  if (crc8(tMsb, tLsb) != tCrc || crc8(hMsb, hLsb) != hCrc) return false;

  const uint16_t rawT = (uint16_t)(tMsb << 8 | tLsb);
  const uint16_t rawH = (uint16_t)(hMsb << 8 | hLsb);

  if (tempC != nullptr) {
    *tempC = -45.0f + 175.0f * (float)rawT / 65535.0f - SHTC3_TEMP_OFFSET;
  }
  if (humidity != nullptr) {
    *humidity = 100.0f * (float)rawH / 65535.0f;
  }
  return true;
}
