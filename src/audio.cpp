// The speaker: an ES8311 codec at I2C 0x18 into an NS4150 amplifier, used
// for exactly one thing — the chirp the pager makes when a message it has not
// shown before arrives.
//
// The register sequence is Espressif's `es8311` component (esp-bsp,
// Apache-2.0) reduced to the single clock configuration this board needs,
// rather than pulled in as a dependency: the project keeps one lib_deps entry
// and writes its drivers against the datasheet, as rtc.cpp and shtc3.cpp do.
// Waveshare's own audio example is not the model here — it reaches for
// `esp_codec_dev` and the new I2C master driver, which would fight Wire.
//
// The clock is the part that has to line up exactly. Arduino's ESP_I2S sets
// mclk_multiple = 256, so at a 16 kHz sample rate MCLK is 4.096 MHz — and
// (4096000, 16000) is a row of Espressif's coefficient table, which is why
// that rate was chosen. The dividers below are that row, nothing else:
//
//   pre_div 1 · pre_multi 1x · adc_div 1 · dac_div 1 · single speed
//   lrck 0x00ff · bclk_div 4 · adc_osr 0x10 · dac_osr 0x10
//
// The ES8311 is the I2S *slave*; the ESP generates MCLK, BCLK and LRCK.
//
// Three board traps, all from the esp32s3-rlcd42 skill:
//
//   * GPIO46 is the amplifier enable, active high, with a 10K pull-down. Low
//     means silence no matter how correct everything else is. It is raised
//     before the codec is touched, which is the order the one person known to
//     have got this chain working used.
//   * GPIO46 is also a boot/ROM-log strapping pin and GPIO45 (LRCK) is
//     VDD_SPI. Both are safe to drive at runtime — the pull-down holds 46 low
//     through reset — but nothing here may leave them driven high at a reset.
//     Hence the teardown at the end of every beep.
//   * I2S DOUT is GPIO8 (ESP -> codec). The vendor table names the pins from
//     the codec's side and reads backwards.
//
// Nothing is left running between beeps: the codec is reset, the I2S channel
// is freed and the amplifier is dropped. A notification is a few hundred
// milliseconds a day, and an idle amplifier hisses on a desk in a quiet room.

#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>

#include "app.h"
#include "board_pins.h"

// --- the one clock configuration ------------------------------------------
static const uint32_t SAMPLE_RATE = 16000;  // -> MCLK 4.096 MHz at 256x

// 0..100, straight into the ES8311's DAC volume register. Loud enough to
// carry across a room, quiet enough not to be startling at a desk.
static const int SPEAKER_VOLUME = 72;

// --- ES8311 registers (datasheet names, as in Espressif's driver) ---------
#define ES8311_RESET          0x00
#define ES8311_CLK_MANAGER01  0x01
#define ES8311_CLK_MANAGER02  0x02
#define ES8311_CLK_MANAGER03  0x03
#define ES8311_CLK_MANAGER04  0x04
#define ES8311_CLK_MANAGER05  0x05
#define ES8311_CLK_MANAGER06  0x06
#define ES8311_CLK_MANAGER07  0x07
#define ES8311_CLK_MANAGER08  0x08
#define ES8311_SDPIN09        0x09
#define ES8311_SDPOUT0A       0x0A
#define ES8311_SYSTEM0D       0x0D
#define ES8311_SYSTEM0E       0x0E
#define ES8311_SYSTEM12       0x12
#define ES8311_SYSTEM13       0x13
#define ES8311_ADC1C          0x1C
#define ES8311_DAC31          0x31
#define ES8311_DAC32          0x32
#define ES8311_DAC37          0x37

static bool s_present = false;
static I2SClass s_i2s;

static bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_ES8311);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t *val) {
  Wire.beginTransmission(I2C_ADDR_ES8311);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)I2C_ADDR_ES8311, 1) != 1) return false;
  *val = (uint8_t)Wire.read();
  return true;
}

// Read-modify-write, which the clock registers need: several of them carry
// bits this configuration must not disturb.
static bool rmw(uint8_t reg, uint8_t keepMask, uint8_t set) {
  uint8_t v;
  if (!rd(reg, &v)) return false;
  return wr(reg, (uint8_t)((v & keepMask) | set));
}

bool audioBegin() {
  // Raised here as well as in audioNotify(): the pin is a strapping pin with
  // a pull-down, and giving it a defined driven level once at boot is better
  // than leaving it floating between beeps.
  pinMode(PIN_SPK_EN, OUTPUT);
  digitalWrite(PIN_SPK_EN, LOW);

  // rtcBegin() owns Wire.begin(); this only probes.
  Wire.beginTransmission(I2C_ADDR_ES8311);
  s_present = (Wire.endTransmission() == 0);
  return s_present;
}

bool audioPresent() { return s_present; }

// The coefficient row above, written out. Returns false on the first I2C
// error — a codec that is not answering must not be left half-configured
// with the amplifier on.
static bool es8311Configure() {
  // Reset digital and clock manager, then power on.
  if (!wr(ES8311_RESET, 0x1F)) return false;
  delay(20);
  if (!wr(ES8311_RESET, 0x00)) return false;
  if (!wr(ES8311_RESET, 0x80)) return false;

  // Clock source: the MCLK pin, not BCLK, and not inverted. 0x3F enables
  // every internal clock.
  if (!wr(ES8311_CLK_MANAGER01, 0x3F)) return false;
  if (!rmw(ES8311_CLK_MANAGER06, 0xDF, 0x00)) return false;  // BCLK not inverted

  // pre_div 1, pre_multi 1x -> both fields zero, the rest of 0x02 untouched.
  if (!rmw(ES8311_CLK_MANAGER02, 0x07, 0x00)) return false;
  if (!wr(ES8311_CLK_MANAGER03, 0x10)) return false;  // single speed, adc_osr 0x10
  if (!wr(ES8311_CLK_MANAGER04, 0x10)) return false;  // dac_osr 0x10
  if (!wr(ES8311_CLK_MANAGER05, 0x00)) return false;  // adc_div 1, dac_div 1
  if (!rmw(ES8311_CLK_MANAGER06, 0xE0, 0x03)) return false;  // bclk_div 4 -> 3
  if (!rmw(ES8311_CLK_MANAGER07, 0xC0, 0x00)) return false;  // lrck high byte
  if (!wr(ES8311_CLK_MANAGER08, 0xFF)) return false;         // lrck low byte

  // Serial port: slave (bit 6 clear), I2S, 16-bit both directions.
  if (!rmw(ES8311_RESET, 0xBF, 0x00)) return false;
  if (!wr(ES8311_SDPIN09, 0x0C)) return false;
  if (!wr(ES8311_SDPOUT0A, 0x0C)) return false;

  // Analogue: power up, enable the PGA and the ADC modulator, power up the
  // DAC, drive the output. None of these are reset defaults.
  if (!wr(ES8311_SYSTEM0D, 0x01)) return false;
  if (!wr(ES8311_SYSTEM0E, 0x02)) return false;
  if (!wr(ES8311_SYSTEM12, 0x00)) return false;
  if (!wr(ES8311_SYSTEM13, 0x10)) return false;
  if (!wr(ES8311_ADC1C, 0x6A)) return false;
  if (!wr(ES8311_DAC37, 0x08)) return false;

  // Volume, then leave the DAC muted: the tone unmutes it, so the pop from
  // powering the analogue path lands in silence rather than in the beep.
  if (!wr(ES8311_DAC32, (uint8_t)((SPEAKER_VOLUME * 256 / 100) - 1))) return false;
  return rmw(ES8311_DAC31, 0xFF, 0x60);  // bits 6 and 5 = mute
}

static void es8311Mute(bool mute) {
  uint8_t v;
  if (!rd(ES8311_DAC31, &v)) return;
  wr(ES8311_DAC31, (uint8_t)(mute ? (v | 0x60) : (v & ~0x60)));
}

// --- the tone ------------------------------------------------------------

// One note, written out through I2S in small blocks so nothing large is ever
// held. Stereo 16-bit: the ES8311 has one DAC, but both slots are filled so
// the frame is whatever the codec expects to see on either.
//
// `rampMs` fades the note in and out. Without it a note that starts at full
// amplitude is a click first and a tone second — on a small speaker the click
// is the louder half.
static void playTone(float hz, uint32_t ms, uint32_t rampMs) {
  static int16_t block[128 * 2];  // 128 frames, L/R

  const uint32_t total = SAMPLE_RATE * ms / 1000;
  const uint32_t ramp = SAMPLE_RATE * rampMs / 1000;
  const float step = 2.0f * (float)M_PI * hz / (float)SAMPLE_RATE;

  float phase = 0.0f;
  uint32_t done = 0;

  while (done < total) {
    const uint32_t n = min((uint32_t)128, total - done);
    for (uint32_t i = 0; i < n; i++) {
      const uint32_t at = done + i;
      float env = 1.0f;
      if (ramp > 0) {
        if (at < ramp) {
          env = (float)at / (float)ramp;
        } else if (at + ramp > total) {
          env = (float)(total - at) / (float)ramp;
        }
      }
      const int16_t s = (int16_t)(sinf(phase) * env * 12000.0f);
      block[i * 2] = s;
      block[i * 2 + 1] = s;
      phase += step;
      if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }
    s_i2s.write((const uint8_t *)block, n * 2 * sizeof(int16_t));
    done += n;
  }
}

static void playSilence(uint32_t ms) {
  static const int16_t quiet[64 * 2] = {0};
  uint32_t done = 0;
  const uint32_t total = SAMPLE_RATE * ms / 1000;
  while (done < total) {
    const uint32_t n = min((uint32_t)64, total - done);
    s_i2s.write((const uint8_t *)quiet, n * 2 * sizeof(int16_t));
    done += n;
  }
}

bool audioNotify() {
  if (!s_present) return false;

  // Before the codec, per the board notes: the amplifier's CTRL is active
  // high behind a 10K pull-down, and getting this wrong is silence with
  // everything else correct and nothing to show for it on I2C.
  digitalWrite(PIN_SPK_EN, HIGH);
  delay(5);

  s_i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, -1 /* no capture */,
                PIN_I2S_MCLK);
  if (!s_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO)) {
    Serial.println("audio: I2S would not start");
    digitalWrite(PIN_SPK_EN, LOW);
    return false;
  }

  // The codec is configured with the clocks already running: it is the slave
  // here, and its clock manager settles against a live MCLK.
  if (!es8311Configure()) {
    Serial.println("audio: ES8311 stopped answering mid-configuration");
    s_i2s.end();
    digitalWrite(PIN_SPK_EN, LOW);
    s_present = false;
    return false;
  }

  es8311Mute(false);
  // Two rising notes, which is what a pager sounds like and what nothing else
  // in the room sounds like.
  playTone(988.0f, 110, 8);   // B5
  playSilence(45);
  playTone(1319.0f, 150, 10);  // E6
  // Trailing silence so the amplifier is not cut mid-waveform, which is its
  // own click.
  playSilence(30);
  es8311Mute(true);

  s_i2s.end();
  // Back to reset, so nothing is left powered and GPIO45/46 go back to being
  // ordinary quiet pins before any reset can catch them driven.
  wr(ES8311_RESET, 0x1F);
  digitalWrite(PIN_SPK_EN, LOW);
  return true;
}
