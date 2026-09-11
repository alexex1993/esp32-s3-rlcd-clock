// 18650 sensing on GPIO4.
//
// The cell reaches the pin through a 200K/100K 1% divider, so a full 4.2 V
// battery presents ~1.40 V — above the ADC's default ~0.95 V full scale, which
// is why the attenuation call below is not optional: without it every reading
// saturates at the same number and the battery looks permanently full.
// GPIO4 is ADC1_CH3, so it keeps reading correctly with Wi-Fi up.

#include <Arduino.h>

#include "app.h"
#include "board_pins.h"

void batteryBegin() {
  // The pin has to be an ADC channel before its attenuation can be set. The
  // Arduino layer only attaches it on the first read, so setting attenuation
  // first prints "Pin is not configured as analog channel" at boot and the
  // call does nothing — leaving every reading saturated. One throwaway read
  // attaches it; the attenuation then sticks.
  (void)analogRead(PIN_BAT_ADC);
  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);  // ~3.1 V full scale
}

float batteryVolts() {
  // analogReadMilliVolts() applies the chip's eFuse ADC calibration, worth a
  // percent or two over raw counts. Average: the divider is high-impedance
  // and the SAR input is noisy.
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(PIN_BAT_ADC);
  return (float)acc / 16.0f / 1000.0f * BAT_DIVIDER;
}

// Resting discharge curve of a li-ion 18650. A straight line from 3.0 to 4.2 V
// is what Waveshare's own firmware uses, but it badly overstates the middle of
// the range, where the cell spends most of its life on a flat plateau.
// Interpolating this table instead keeps the reading honest to within a few
// percent at light load.
struct CurvePoint {
  float volts;
  uint8_t percent;
};

static const CurvePoint kCurve[] = {
    {4.20f, 100}, {4.15f, 95}, {4.10f, 90}, {4.05f, 85}, {4.00f, 80},
    {3.95f, 72},  {3.90f, 64}, {3.85f, 56}, {3.80f, 48}, {3.75f, 40},
    {3.70f, 33},  {3.65f, 27}, {3.60f, 20}, {3.50f, 12}, {3.40f, 6},
    {3.30f, 3},   {3.00f, 0},
};

int batteryPercent(float volts) {
  const size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
  if (volts >= kCurve[0].volts) return 100;
  if (volts <= kCurve[n - 1].volts) return 0;

  for (size_t i = 1; i < n; i++) {
    if (volts >= kCurve[i].volts) {
      const CurvePoint &hi = kCurve[i - 1];
      const CurvePoint &lo = kCurve[i];
      float f = (volts - lo.volts) / (hi.volts - lo.volts);
      return (int)lroundf(lo.percent + f * (hi.percent - lo.percent));
    }
  }
  return 0;
}
