#pragma once

// Pin map of the Waveshare ESP32-S3-RLCD-4.2.
// Source: https://docs.waveshare.com/ESP32-ESPHome-Tutorials/Example-RLCD-Voice
// (section 2.2 "GPIO Pin Assignment").

// --- 4.2" reflective LCD, ST7305, 300 x 400 -------------------------------
// Write-only 4-wire SPI: the panel never drives data back, so there is no MISO.
#define PIN_LCD_SCLK 11
#define PIN_LCD_MOSI 12
#define PIN_LCD_CS   40
#define PIN_LCD_DC    5
#define PIN_LCD_RST  41
// This display is reflective: it has no backlight pin, and none is needed.

// --- I2C: SHTC3 (0x70), PCF85063 RTC, ES8311 (0x18), ES7210 (0x40/0x42) ---
#define PIN_I2C_SDA  13
#define PIN_I2C_SCL  14

// --- I2S audio ------------------------------------------------------------
#define PIN_I2S_DOUT  8   // to speaker (ES8311)
#define PIN_I2S_BCLK  9
#define PIN_I2S_DIN  10   // from microphones (ES7210)
#define PIN_I2S_MCLK 16
#define PIN_I2S_LRCK 45
#define PIN_SPK_EN   46   // speaker amplifier enable, active high

// --- Buttons and battery --------------------------------------------------
#define PIN_BTN_BOOT  0   // active low
#define PIN_BTN_KEY  18   // active low
#define PIN_BAT_ADC   4   // 18650 through a 1/3 divider

// --- RTC ------------------------------------------------------------------
#define PIN_RTC_INT  15   // PCF85063A INT, active low (unused here, polling)

// --- I2C addresses on the shared bus --------------------------------------
#define I2C_ADDR_ES8311   0x18
#define I2C_ADDR_ES7210   0x40  // 0x42 on some batches
#define I2C_ADDR_PCF85063 0x51
#define I2C_ADDR_SHTC3    0x70

// --- Battery divider ------------------------------------------------------
// R21 200K / R23 100K, 1% -> the pin sees VBAT / 3.
#define BAT_DIVIDER 3.0f
