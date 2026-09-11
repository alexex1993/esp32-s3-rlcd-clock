// PCF85063A real-time clock, on the board's shared I2C bus.
//
// The driver is written against the datasheet rather than pulled in as a
// library, so the project keeps a single lib_deps entry (U8g2).
//
// Register map used here:
//   0x00 Control_1  bit5 STOP, bit1 12_24 (0 = 24-hour)
//   0x04 Seconds    bit7 OS = oscillator stopped, time not trustworthy
//   0x05..0x0A      Minutes, Hours, Days, Weekdays, Months, Years (BCD)

#include <Arduino.h>
#include <Wire.h>

#include "app.h"
#include "board_pins.h"

static bool s_present = false;

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_PCF85063);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool rtcBegin() {
  // Always pass the pins. The ESP32-S3 Arduino variant defaults Wire to
  // SDA = 8, SCL = 9, which on this board are the I2S data and bit clock —
  // the scan then finds nothing while the chips sit there answering.
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);

  Wire.beginTransmission(I2C_ADDR_PCF85063);
  s_present = (Wire.endTransmission() == 0);
  if (s_present) {
    // Running, 24-hour mode, no capacitor/alarm trickery.
    writeReg(0x00, 0x00);
  }
  return s_present;
}

bool rtcPresent() { return s_present; }

bool rtcReadTime(struct tm *out) {
  if (!s_present) return false;

  Wire.beginTransmission(I2C_ADDR_PCF85063);
  Wire.write(0x04);  // Seconds
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_PCF85063, 7) != 7) return false;

  uint8_t sec = Wire.read(), min = Wire.read(), hour = Wire.read();
  uint8_t day = Wire.read(), wday = Wire.read();
  uint8_t mon = Wire.read(), year = Wire.read();

  out->tm_sec  = bcd2dec(sec & 0x7F);
  out->tm_min  = bcd2dec(min & 0x7F);
  out->tm_hour = bcd2dec(hour & 0x3F);
  out->tm_mday = bcd2dec(day & 0x3F);
  out->tm_wday = wday & 0x07;
  out->tm_mon  = bcd2dec(mon & 0x1F) - 1;
  out->tm_year = bcd2dec(year) + 100;  // struct tm counts from 1900
  out->tm_isdst = 0;

  // Bit 7 of Seconds is OS. Set means the oscillator stopped at some point,
  // so the calendar above is whatever it happened to land on.
  return (sec & 0x80) == 0;
}

bool rtcSetTime(const struct tm *t) {
  if (!s_present) return false;

  // Datasheet order: stop the counter, load the registers, start it again.
  if (!writeReg(0x00, 0x20)) return false;  // Control_1: STOP = 1

  Wire.beginTransmission(I2C_ADDR_PCF85063);
  Wire.write(0x04);
  Wire.write(dec2bcd((uint8_t)t->tm_sec));   // bit 7 = 0 clears the OS flag
  Wire.write(dec2bcd((uint8_t)t->tm_min));
  Wire.write(dec2bcd((uint8_t)t->tm_hour));
  Wire.write(dec2bcd((uint8_t)t->tm_mday));
  Wire.write((uint8_t)(t->tm_wday & 0x07));
  Wire.write(dec2bcd((uint8_t)(t->tm_mon + 1)));
  Wire.write(dec2bcd((uint8_t)(t->tm_year % 100)));
  bool ok = Wire.endTransmission() == 0;

  ok = writeReg(0x00, 0x00) && ok;  // STOP = 0, 24-hour mode
  return ok;
}
