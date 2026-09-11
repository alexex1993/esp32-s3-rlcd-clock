// The face, on the 4.2" reflective ST7305 panel.
//
// The panel is 1 bpp and holds its image with no host involvement, so a redraw
// costs nothing to *keep* — only to write. The whole face is rebuilt from a
// 15 KB full-frame buffer and pushed in one sendBuffer().
//
// 400 x 300, top to bottom:
//
//   0..25    status bar: date and a small battery gauge
//   40..132  the clock, HH:MM huge with the seconds set alongside
//   144..158 outdoors right now: temperature, humidity, pressure
//   168      rule
//   179..272 twelve hourly columns: hour, glyph, temperature, chance of rain
//   279..294 today's sunset, and when the forecast was last fetched
//
// The bands below the rule are spaced to land the sunset line one text gap
// off the bottom edge rather than leaving the strip floating: everything
// between the clock and the footer sits as low as the 300 px allows.
//
// There is no band naming the place or the range: the face only ever shows
// one place and one range, so the strip of pixels that said so went to the
// clock, which is what this thing is read from across the room.
//
// Everything is drawn from primitives rather than from bitmaps: the weather
// glyphs below are ~28 px of discs, boxes and lines, which stays sharp on a
// reflective panel with no grey levels to hide behind.

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <math.h>

#include "app.h"
#include "board_pins.h"

// The controller's native orientation is 300 wide x 400 tall; U8G2_R1 rotates
// that into the 400 x 300 landscape the board is read in. Use U8G2_R3 if the
// face comes out upside down in your enclosure.
static U8G2_ST7305_300X400_F_4W_HW_SPI lcd(U8G2_R1, PIN_LCD_CS, PIN_LCD_DC,
                                           PIN_LCD_RST);

static const int W = 400;
static const int H = 300;

static const int BAR_H = 26;
static const int CLOCK_BASE = 132;
static const int SENSOR_BASE = 158;
static const int RULE_Y = 168;
static const int HOUR_BASE = 189;
static const int ICON_Y = 196;
static const int ICON_SIZE = 28;
static const int TEMP_BASE = 248;
static const int POP_BASE = 270;
// The 10x20 face descends 5 px, so 294 is the lowest baseline that still
// clears the 300 px edge.
static const int SUNSET_BASE = 294;

// Twelve columns, 32 px each, centred in the 400 px width.
static const int COL_W = 32;
static const int COL_X0 = (W - WEATHER_HOURS * COL_W) / 2;

// --- the two list screens -------------------------------------------------
// The world headlines and the Telegram pager are the same screen with a
// different heading and a different source: both are a stack of short pieces
// of Russian text, each with somebody's name and a time on the end. So both
// go through drawFeedScreen() below.
//
// Same 26 px bar as the clock, and a footer on the same baseline, so the three
// screens do not jump against each other as KEY steps through them. Between
// them the items *flow*: each takes as many lines as it needs and the next
// starts underneath, so a screenful of short ones shows more of them than a
// screenful of long ones. Nothing is clipped mid-line: the item that meets
// the bottom is wrapped again into the lines left, and says "..." about it.
static const int FEED_PAD = 10;
static const int FEED_MARK_W = 3;                       // rule down the left
static const int FEED_TEXT_X = FEED_PAD + FEED_MARK_W + 7;
static const int FEED_TOP = BAR_H + 8;
static const int FEED_LEAD = 16;                        // 9x15, baseline to baseline
static const int FEED_ITEM_GAP = 9;
static const int FEED_BOTTOM = SUNSET_BASE - 18;
// Every line the list area holds: fifteen. A page may take all of them — it
// is somebody talking to you, and there is nowhere else to read the rest —
// while a headline is held to three, so one long one cannot push the others
// off the screen.
static const int FEED_AREA_LINES = (FEED_BOTTOM - FEED_TOP) / FEED_LEAD;
static const int NEWS_MAX_LINES = 3;
static const int PAGER_MAX_LINES = FEED_AREA_LINES;
// Who it is from and when, when the text leaves no room for them on its last
// line and they take a line of their own: 6x12, so a tighter lead than the
// text's — 14 is the least that keeps a Й on that line one pixel clear of a
// descender on the line above. Only the pager does that — a page with no
// sender is a page from nobody, while a headline without its outlet is still
// the headline, and a line each would cost the news screen a headline.
static const int FEED_META_LEAD = 14;
static const bool NEWS_META_LINE = false;
static const bool PAGER_META_LINE = true;
// 41 cells of the 9x15 face fit the text column, and Cyrillic is two bytes a
// cell.
static const int FEED_LINE_CAP = 96;

// The date is written in Russian, so the month is in the genitive that follows
// a day number ("10 сентября"), not the nominative a calendar would use.
// tm_wday counts from Sunday.
static const char *const kWeekdays[7] = {
    "Воскресенье", "Понедельник", "Вторник", "Среда",
    "Четверг",     "Пятница",     "Суббота"};
static const char *const kMonths[12] = {
    "января",   "февраля", "марта",    "апреля",  "мая",    "июня",
    "июля",     "августа", "сентября", "октября", "ноября", "декабря"};

void displayBegin() {
  // U8g2 drives the panel through the global SPI object but never assigns its
  // pins, and the ESP32-S3 defaults are not this board's. Claiming the bus
  // here first makes U8g2's own SPI.begin() a no-op. Skip it and the panel
  // stays blank with no error anywhere.
  SPI.begin(PIN_LCD_SCLK, -1 /* no MISO: the ST7305 is write-only */,
            PIN_LCD_MOSI, -1 /* CS belongs to U8g2 */);

  lcd.begin();
  // The GPIO matrix caps SPI2 at ~40 MHz and the ST7305 wants a write period
  // of at least 30 ns. 24 MHz clears both, and is what Waveshare's own driver
  // uses.
  lcd.setBusClock(24000000);
  lcd.setContrast(0);  // no-op on this panel; there is no backlight to dim
}

// --- small text helpers ---------------------------------------------------
// drawUTF8 throughout, because the temperatures carry a degree sign; it falls
// back to drawStr's behaviour for plain ASCII.

static void drawRight(const char *s, int xRight, int y) {
  lcd.drawUTF8(xRight - lcd.getUTF8Width(s), y, s);
}

static void drawCentered(const char *s, int cx, int y) {
  lcd.drawUTF8(cx - lcd.getUTF8Width(s) / 2, y, s);
}

// Draws one segment in the given face and hands back the x the next segment
// starts at. Needed because no U8g2 t_cyrillic face carries the degree sign
// and no helv face carries Cyrillic, so a row that mixes Russian words with a
// temperature has to change font mid-line.
// No U8g2 t_cyrillic face has a bold cut above 6x13B, so weight is added with
// a second pass one pixel over. On a 1 bpp panel that *is* bold — it is how
// the X11 faces these are cut from were emboldened in the first place.
static void drawBoldUTF8(int x, int y, const char *s) {
  lcd.drawUTF8(x, y, s);
  lcd.drawUTF8(x + 1, y, s);
}

static int drawRun(int x, int y, const uint8_t *font, const char *s) {
  lcd.setFont(font);
  lcd.drawUTF8(x, y, s);
  return x + lcd.getUTF8Width(s);
}

// --- battery --------------------------------------------------------------

// A 24 x 11 gauge drawn at its top-left corner, in whatever the current draw
// colour is — inside the status bar that is 0, so it comes out knocked out of
// the black. Small on purpose: it is a status-bar detail, not a headline.
static void drawBatteryIcon(int x, int y, int percent) {
  const int w = 22, h = 11;
  lcd.drawFrame(x, y, w, h);
  lcd.drawBox(x + w, y + 3, 2, h - 6);  // the positive terminal nub

  const int innerW = w - 4;
  int fill = (int)lroundf(innerW * (percent / 100.0f));
  if (fill < 0) fill = 0;
  if (fill > innerW) fill = innerW;
  if (fill > 0) lcd.drawBox(x + 2, y + 2, fill, h - 4);
}

// --- weather glyphs -------------------------------------------------------

enum IconKind {
  ICON_SUN,
  ICON_MOON,
  ICON_SUN_CLOUD,
  ICON_MOON_CLOUD,
  ICON_CLOUD,
  ICON_FOG,
  ICON_DRIZZLE,
  ICON_RAIN,
  ICON_SNOW,
  ICON_STORM,
};

// WMO 4677, as Open-Meteo reports it, collapsed onto the ten glyphs above.
static IconKind wmoIcon(uint8_t code, bool day) {
  switch (code) {
    case 0:  return day ? ICON_SUN : ICON_MOON;
    case 1:
    case 2:  return day ? ICON_SUN_CLOUD : ICON_MOON_CLOUD;
    case 3:  return ICON_CLOUD;
    case 45:
    case 48: return ICON_FOG;
    case 51: case 53: case 55:            // drizzle
    case 56: case 57: return ICON_DRIZZLE;  // freezing drizzle
    case 61: case 63: case 65:            // rain
    case 66: case 67:                     // freezing rain
    case 80: case 81: case 82: return ICON_RAIN;  // showers
    case 71: case 73: case 75: case 77:   // snow
    case 85: case 86: return ICON_SNOW;   // snow showers
    case 95: case 96: case 99: return ICON_STORM;
    default: return ICON_CLOUD;
  }
}

// A filled sun: disc plus eight rays. The ray offsets are the unit circle at
// 45-degree steps, scaled to the inner and outer radius and rounded.
static void drawSun(int cx, int cy, int r) {
  lcd.drawDisc(cx, cy, r);

  const int in = r + 3;
  const int out = r + 6;
  const int inD = (int)lroundf(in * 0.7071f);
  const int outD = (int)lroundf(out * 0.7071f);

  lcd.drawLine(cx, cy - in, cx, cy - out);
  lcd.drawLine(cx, cy + in, cx, cy + out);
  lcd.drawLine(cx - in, cy, cx - out, cy);
  lcd.drawLine(cx + in, cy, cx + out, cy);
  lcd.drawLine(cx - inD, cy - inD, cx - outD, cy - outD);
  lcd.drawLine(cx + inD, cy - inD, cx + outD, cy - outD);
  lcd.drawLine(cx - inD, cy + inD, cx - outD, cy + outD);
  lcd.drawLine(cx + inD, cy + inD, cx + outD, cy + outD);
}

// A crescent: a disc with a smaller one erased out of its upper right.
static void drawMoon(int cx, int cy, int r) {
  lcd.drawDisc(cx, cy, r);
  lcd.setDrawColor(0);
  lcd.drawDisc(cx + r / 2 + 1, cy - r / 2 - 1, r - 1);
  lcd.setDrawColor(1);
}

// Half a sun behind a horizon that runs out past it either side: a disc, the
// part below the line erased, then the line drawn back over the erasure.
// `baseY` is the horizon, so it lines up with a text baseline.
static void drawSunsetGlyph(int cx, int baseY, int r) {
  const int reach = r + 6;
  lcd.drawDisc(cx, baseY, r);
  lcd.setDrawColor(0);
  lcd.drawBox(cx - r - 1, baseY + 1, 2 * r + 3, r + 2);
  lcd.setDrawColor(1);
  lcd.drawHLine(cx - reach, baseY, 2 * reach + 1);
}

// A cloud whose bounding box is w x (2w/3), at its top-left corner: a large
// lobe on the right, a small one on the left, joined along a flat base.
//
// `halo` first stamps the same shape 2 px fatter in white. That is what keeps
// a cloud from merging into the sun or moon it is drawn over — on a 1 bpp
// panel two overlapping filled shapes are otherwise one shape.
static void drawCloudAt(int x, int y, int w, bool halo) {
  const int h = w * 2 / 3;
  const int rb = h / 2;  // big lobe, right
  const int rs = h / 3;  // small lobe, left

  const int bx = x + w - rb - 1, by = y + h - rb - 1;
  const int sx = x + rs + 1, sy = y + h - rs - 1;

  if (halo) {
    lcd.setDrawColor(0);
    lcd.drawDisc(bx, by, rb + 2);
    lcd.drawDisc(sx, sy, rs + 2);
    lcd.drawBox(x + rs - 2, y + h - rs - 3, w - rs - rb + 3, rs + 4);
    lcd.setDrawColor(1);
  }

  lcd.drawDisc(bx, by, rb);
  lcd.drawDisc(sx, sy, rs);
  lcd.drawBox(x + rs, y + h - rs - 1, w - rs - rb - 1, rs + 1);
}

// One snowflake: a six-pointed star five pixels across.
static void drawFlake(int cx, int cy) {
  lcd.drawHLine(cx - 2, cy, 5);
  lcd.drawVLine(cx, cy - 2, 5);
  lcd.drawPixel(cx - 2, cy - 2);
  lcd.drawPixel(cx + 2, cy - 2);
  lcd.drawPixel(cx - 2, cy + 2);
  lcd.drawPixel(cx + 2, cy + 2);
}

// A falling streak, two pixels wide so it survives the panel's contrast.
static void drawDrop(int x, int yTop, int len) {
  lcd.drawLine(x, yTop, x - len / 3, yTop + len);
  lcd.drawLine(x + 1, yTop, x - len / 3 + 1, yTop + len);
}

// Draws one 28 x 28 glyph at its top-left corner.
static void drawWeatherIcon(int x, int y, IconKind kind) {
  switch (kind) {
    case ICON_SUN:
      drawSun(x + 14, y + 14, 6);
      break;

    case ICON_MOON:
      drawMoon(x + 14, y + 14, 10);
      break;

    case ICON_SUN_CLOUD:
      drawSun(x + 19, y + 8, 4);
      drawCloudAt(x + 1, y + 11, 22, true);
      break;

    case ICON_MOON_CLOUD:
      drawMoon(x + 19, y + 8, 6);
      drawCloudAt(x + 1, y + 11, 22, true);
      break;

    case ICON_CLOUD:
      drawCloudAt(x + 1, y + 6, 26, false);
      break;

    case ICON_FOG:
      // The cloud is lifted clear of the bars: at 28 px a cloud sitting on
      // them merges into one blob and the glyph stops reading as fog.
      drawCloudAt(x + 3, y + 0, 22, false);
      lcd.drawBox(x + 3, y + 18, 22, 2);
      lcd.drawBox(x + 7, y + 22, 17, 2);
      lcd.drawBox(x + 4, y + 26, 20, 2);
      break;

    case ICON_DRIZZLE:
      drawCloudAt(x + 1, y + 2, 26, false);
      drawDrop(x + 8, y + 22, 4);
      drawDrop(x + 15, y + 24, 4);
      drawDrop(x + 22, y + 22, 4);
      break;

    case ICON_RAIN:
      drawCloudAt(x + 1, y + 2, 26, false);
      drawDrop(x + 9, y + 21, 7);
      drawDrop(x + 16, y + 21, 7);
      drawDrop(x + 23, y + 21, 7);
      break;

    case ICON_SNOW:
      drawCloudAt(x + 1, y + 2, 26, false);
      drawFlake(x + 8, y + 24);
      drawFlake(x + 15, y + 27);
      drawFlake(x + 22, y + 24);
      break;

    case ICON_STORM:
      // A shorter cloud than the rain glyphs use, to leave the bolt the 13 px
      // it needs — below that it reads as an arrowhead rather than lightning.
      drawCloudAt(x + 2, y + 0, 24, false);
      lcd.drawTriangle(x + 17, y + 14, x + 7, y + 23, x + 15, y + 23);
      lcd.drawTriangle(x + 13, y + 20, x + 22, y + 20, x + 10, y + 30);
      break;
  }
}

// --- the panels -----------------------------------------------------------

static void drawStatusBar(const UiState &s) {
  char buf[64];  // UTF-8: "Понедельник, 10 сентября" is 43 bytes

  lcd.drawBox(0, 0, W, BAR_H);
  lcd.setDrawColor(0);  // knock everything below out of the filled bar

  snprintf(buf, sizeof(buf), "%d%%", s.percent);
  lcd.setFont(u8g2_font_helvB10_tf);
  const int pctW = lcd.getUTF8Width(buf);
  lcd.drawUTF8(W - 10 - pctW, 18, buf);

  const int iconX = W - 10 - pctW - 5 - 24;
  drawBatteryIcon(iconX, 7, s.percent);

  const struct tm &t = s.time;
  const char *wd = kWeekdays[(t.tm_wday >= 0 && t.tm_wday < 7) ? t.tm_wday : 0];
  const char *mo = kMonths[(t.tm_mon >= 0 && t.tm_mon < 12) ? t.tm_mon : 0];
  snprintf(buf, sizeof(buf), "%s, %d %s", wd, t.tm_mday, mo);
  // The helv faces carry no Cyrillic; 10x20 is the largest U8g2 face that
  // does. Worst case ("Понедельник, 10 сентября") is 24 cells of a 10 px
  // monospace, which still clears the gauge.
  lcd.setFont(u8g2_font_10x20_t_cyrillic);
  drawRight(buf, iconX - 12, 20);

  lcd.setDrawColor(1);
}

static void drawClock(const UiState &s) {
  // HH:MM huge, with the seconds set much smaller on the same baseline so the
  // minutes stay the thing you read from across the room.
  char hm[8], ss[4];
  snprintf(hm, sizeof(hm), "%02d:%02d", s.time.tm_hour, s.time.tm_min);
  snprintf(ss, sizeof(ss), "%02d", s.time.tm_sec);

  // 92 px is the largest logisoso U8g2 ships, so HH:MM cannot grow any
  // further: "23:59" measures 271 px of the 400, and the 92 px ascent hangs
  // off a 132 px baseline, clear of both the 26 px status bar and the outdoor
  // row below. The seconds carry the rest of the weight — at 42 px they read
  // from across the room too, and still cannot be mistaken for the minutes.
  lcd.setFont(u8g2_font_logisoso92_tn);
  const int wHM = lcd.getStrWidth(hm);
  lcd.setFont(u8g2_font_logisoso42_tn);
  const int wSS = lcd.getStrWidth(ss);

  const int gap = 14;
  const int x0 = (W - (wHM + gap + wSS)) / 2;

  lcd.setFont(u8g2_font_logisoso92_tn);
  lcd.drawStr(x0, CLOCK_BASE, hm);
  lcd.setFont(u8g2_font_logisoso42_tn);
  lcd.drawStr(x0 + wHM + gap, CLOCK_BASE, ss);
}

// What it is doing outside on the left, whatever needs saying on the right.
// This used to be the on-board SHTC3's indoor reading; it is now the same
// Open-Meteo `current` block the forecast strip comes from, so the whole face
// talks about one place. The SHTC3 is still readable at the console with "?".
static void drawOutdoorRow(const UiState &s) {
  char buf[48];

  // Label and unit in Cyrillic, the reading itself in the heavier helv face
  // that has the degree sign. The unit is the bare "мм": spelling out
  // "мм рт.ст." costs 60 px and pushes the note off the right of the row.
  int x = drawRun(10, SENSOR_BASE, u8g2_font_9x15_t_cyrillic, "НА УЛИЦЕ ");

  WeatherNow now;
  if (weatherNow(&now)) {
    char t[12] = "--", rh[12] = "--", mm[12] = "--";
    if (!isnan(now.temp)) {
      snprintf(t, sizeof(t), "%.1f\xC2\xB0" "C", now.temp);
    }
    if (!isnan(now.humidity)) {
      snprintf(rh, sizeof(rh), "%.0f%%", now.humidity);
    }
    if (!isnan(now.pressure)) {
      // Millimetres of mercury: what a forecast is quoted in here, and what
      // the barometer on the wall next to this clock reads.
      snprintf(mm, sizeof(mm), "%.0f", now.pressure * 0.750062f);
    }
    snprintf(buf, sizeof(buf), "%s  %s  %s ", t, rh, mm);
    x = drawRun(x, SENSOR_BASE, u8g2_font_helvB14_tf, buf);
    x = drawRun(x, SENSOR_BASE, u8g2_font_9x15_t_cyrillic, "мм");
  } else {
    x = drawRun(x, SENSOR_BASE, u8g2_font_9x15_t_cyrillic, "нет данных");
  }

  const char *right = nullptr;
  if (s.note != nullptr && s.note[0] != '\0') {
    right = s.note;
  } else if (!s.timeValid) {
    // Short on purpose: it shares the row with the outdoor reading, and the
    // console already prints the long version with the command to fix it.
    right = "ЧАСЫ НЕ ЗАДАНЫ";
  }
  if (right != nullptr) {
    lcd.setFont(u8g2_font_7x13_t_cyrillic);
    // Both halves are variable-width, so the fit is checked rather than
    // assumed — overlapping text on a 1 bpp panel is unreadable, not just ugly.
    if (x + 12 + lcd.getUTF8Width(right) <= W - 10) {
      drawRight(right, W - 10, SENSOR_BASE);
    }
  }
}

// The strip under the columns: today's sunset centred and bold, with the age
// of the forecast set small on the right. The stamp used to sit up in the
// title band; it is a footnote, and this is where footnotes go.
static void drawFooter() {
  char buf[32];  // UTF-8: "обновлено 21:04" is 24 bytes

  const char *at = weatherSunset();
  // Drawn only once there is a value: an empty label is worse than no label.
  if (at[0] != '\0') {
    snprintf(buf, sizeof(buf), "ЗАКАТ  %s", at);
    lcd.setFont(u8g2_font_10x20_t_cyrillic);

    const int r = 6;
    const int glyphW = 2 * (r + 6) + 1;
    const int gap = 9;
    const int textW = lcd.getUTF8Width(buf) + 1;  // +1 for the bold pass
    const int x0 = (W - (glyphW + gap + textW)) / 2;

    drawSunsetGlyph(x0 + glyphW / 2, SUNSET_BASE - 4, r);
    drawBoldUTF8(x0 + glyphW + gap, SUNSET_BASE, buf);
  }

  lcd.setFont(u8g2_font_6x12_t_cyrillic);
  if (weatherValid()) {
    snprintf(buf, sizeof(buf), "обновлено %s", weatherStamp());
  } else {
    snprintf(buf, sizeof(buf), "нет данных");
  }
  drawRight(buf, W - 10, SUNSET_BASE);
}

static void drawForecast(const UiState &s) {
  char buf[24];

  lcd.drawHLine(0, RULE_Y, W);

  const int n = weatherCount();
  if (n <= 0) {
    lcd.setFont(u8g2_font_9x15_t_cyrillic);
#ifdef WIFI_SSID
    drawCentered("ждём первый прогноз по Wi-Fi...", W / 2, 220);
#else
    drawCentered("нет учётных данных Wi-Fi в .env", W / 2, 220);
#endif
    return;
  }

  const WeatherHour *h = weatherHours();
  for (int i = 0; i < n && i < WEATHER_HOURS; i++) {
    const int cx = COL_X0 + i * COL_W + COL_W / 2;

    // The first column is the hour in progress: box it so "now" is obvious.
    snprintf(buf, sizeof(buf), "%02d", h[i].hour);
    lcd.setFont(u8g2_font_helvB10_tf);
    if (i == 0) {
      lcd.drawRBox(cx - 13, HOUR_BASE - 12, 26, 16, 3);
      lcd.setDrawColor(0);
      drawCentered(buf, cx, HOUR_BASE);
      lcd.setDrawColor(1);
    } else {
      drawCentered(buf, cx, HOUR_BASE);
    }

    drawWeatherIcon(cx - ICON_SIZE / 2, ICON_Y, wmoIcon(h[i].code, h[i].day));

    if (isnan(h[i].temp)) {
      snprintf(buf, sizeof(buf), "--");
    } else {
      snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lroundf(h[i].temp));
    }
    lcd.setFont(u8g2_font_helvB12_tf);
    drawCentered(buf, cx, TEMP_BASE);

    // A zero chance of rain is left blank: twelve "0%" in a row is noise.
    if (h[i].pop > 0) {
      snprintf(buf, sizeof(buf), "%u%%", h[i].pop);
      lcd.setFont(u8g2_font_helvR08_tf);
      drawCentered(buf, cx, POP_BASE);
    }
  }
}

// --- the list screens -----------------------------------------------------

// The two headings. Every other Russian label on the panel lives in this
// file, so these do too.
static const char kNewsHeading[] = "В МИРЕ";
static const char kPagerHeading[] = "ПЕЙДЖЕР";

// Splits `s` into at most maxLines lines no wider than maxW pixels, breaking
// on spaces; the caller has already selected the font. Returns the line count.
//
// Two things it cannot assume. A break must land on a character boundary, not
// a byte one, or a Cyrillic pair is cut in half and the line ends in a stray
// byte. And the trailing "..." that marks a headline as cut has a width of its
// own, so the line is trimmed a character at a time until the marker fits too
// rather than divided out arithmetically — these faces are variable-width for
// the ASCII half of a mixed headline.
static int wrapText(const char *s, int maxW, int maxLines,
                    char out[][FEED_LINE_CAP]) {
  int lines = 0;

  while (*s != '\0' && lines < maxLines) {
    while (*s == ' ') s++;
    if (*s == '\0') break;

    char *line = out[lines];
    const char *p = s;
    const char *breakAt = nullptr;
    size_t len = 0, breakLen = 0;

    line[0] = '\0';
    while (*p != '\0') {
      size_t add = 1;  // one whole UTF-8 character
      while (((uint8_t)p[add] & 0xC0) == 0x80) add++;
      if (len + add + 1 > (size_t)FEED_LINE_CAP) break;

      memcpy(line + len, p, add);
      line[len + add] = '\0';
      if (lcd.getUTF8Width(line) > maxW) {
        line[len] = '\0';  // put the line back the way it was
        break;
      }
      len += add;
      if (*p == ' ') {
        breakLen = len - add;  // the line without its trailing space
        breakAt = p + add;
      }
      p += add;
    }

    if (*p == '\0') {
      s = p;  // the rest of the headline fitted
    } else if (breakAt != nullptr) {
      line[breakLen] = '\0';
      s = breakAt;
    } else {
      s = p;  // a single word wider than the whole column
    }
    lines++;
  }

  while (*s == ' ') s++;
  if (*s != '\0' && lines > 0) {
    char *line = out[lines - 1];
    size_t len = strlen(line);

    // Room for the marker first, so the copy back below cannot overrun.
    while (len > 0 && len + 4 > (size_t)FEED_LINE_CAP) {
      do {
        len--;
      } while (len > 0 && ((uint8_t)line[len] & 0xC0) == 0x80);
      line[len] = '\0';
    }
    for (;;) {
      char probe[FEED_LINE_CAP];
      snprintf(probe, sizeof(probe), "%s...", line);
      if (len == 0 || lcd.getUTF8Width(probe) <= maxW) {
        memcpy(line, probe, strlen(probe) + 1);
        break;
      }
      do {
        len--;
      } while (len > 0 && ((uint8_t)line[len] & 0xC0) == 0x80);
      line[len] = '\0';
    }
  }

  return lines;
}

// The section name on the left, and the clock on the right — the clock is what
// you gave up to look at this screen, so it does not disappear entirely.
static void drawFeedBar(const UiState &s, const char *heading) {
  char buf[16];

  lcd.drawBox(0, 0, W, BAR_H);
  lcd.setDrawColor(0);

  snprintf(buf, sizeof(buf), "%d%%", s.percent);
  lcd.setFont(u8g2_font_helvB10_tf);
  const int pctW = lcd.getUTF8Width(buf);
  lcd.drawUTF8(W - 10 - pctW, 18, buf);

  const int iconX = W - 10 - pctW - 5 - 24;
  drawBatteryIcon(iconX, 7, s.percent);

  snprintf(buf, sizeof(buf), "%02d:%02d", s.time.tm_hour, s.time.tm_min);
  lcd.setFont(u8g2_font_helvB12_tf);
  const int clockX = iconX - 12 - lcd.getUTF8Width(buf);
  lcd.drawUTF8(clockX, 19, buf);

  // Fit-checked rather than assumed, like the outdoor row: both halves are
  // variable-width, and overlapping text at 1 bpp is unreadable, not untidy.
  lcd.setFont(u8g2_font_10x20_t_cyrillic);
  if (FEED_PAD + lcd.getUTF8Width(heading) <= clockX - 10) {
    lcd.drawUTF8(FEED_PAD, 20, heading);
  }

  lcd.setDrawColor(1);
}

// Where the list stands on the left, and what KEY does next on the right.
// `next` names the screen the button goes to from here, so the three of them
// read as a cycle rather than as a pair of dead ends.
static void drawFeedFooter(const char *left, const char *next) {
  char buf[48];

  lcd.drawHLine(0, FEED_BOTTOM + 6, W);
  lcd.setFont(u8g2_font_6x12_t_cyrillic);
  lcd.drawUTF8(FEED_PAD, SUNSET_BASE, left);

  snprintf(buf, sizeof(buf), "KEY: %s", next);
  drawRight(buf, W - FEED_PAD, SUNSET_BASE);
}

// The screen both lists share. `why` is what to say when there is nothing to
// list — which reason it is decides what the user has to go and fix, so the
// callers below never collapse them into one message.
//
// `maxLines` is how tall one item may grow. The number of items is not fixed:
// each takes what it needs and the next starts underneath, so a screenful of
// short ones shows more of them. The one that meets the bottom of the list is
// wrapped again into the lines that are left and ends in "...", rather than
// being dropped and leaving that space empty — with pages allowed a whole
// screen each, a long older one would otherwise blank most of the panel.
//
// `metaLine` is whether who-and-when may take a line of its own when the text
// fills its last line; without it they are simply left off such an item.
static void drawFeedScreen(const UiState &s, const char *heading,
                           const FeedItem *items, int n, int maxLines,
                           bool metaLine, const char *footer, const char *why,
                           const char *next) {
  drawFeedBar(s, heading);

  if (n <= 0) {
    lcd.setFont(u8g2_font_9x15_t_cyrillic);
    drawCentered(why, W / 2, 150);
    drawFeedFooter(footer, next);
    return;
  }

  const int textW = W - FEED_PAD - FEED_TEXT_X;
  // Static: fifteen lines of 96 bytes is too much to put on the loop task's
  // stack for a frame, and only the loop task ever draws.
  static char lines[FEED_AREA_LINES][FEED_LINE_CAP];
  int y = FEED_TOP;

  for (int i = 0; i < n; i++) {
    const int space = FEED_BOTTOM - y;
    const int room = space / FEED_LEAD;
    if (room <= 0) break;

    // Who it is from and when. Measured first, because whether it fits
    // decides how the text is wrapped at the bottom of the list.
    char meta[FEED_FROM_CAP + 8];
    if (items[i].timed) {
      snprintf(meta, sizeof(meta), "%s %02u:%02u", items[i].from,
               items[i].hour, items[i].minute);
    } else {
      snprintf(meta, sizeof(meta), "%s", items[i].from);
    }
    lcd.setFont(u8g2_font_6x12_t_cyrillic);
    const int metaW = lcd.getUTF8Width(meta);

    lcd.setFont(u8g2_font_9x15_t_cyrillic);
    const int limit = maxLines < room ? maxLines : room;
    int count = wrapText(items[i].text, textW, limit, lines);
    if (count <= 0) continue;

    // Tucked onto the end of the last line where the text left room, which
    // costs no height at all. Where it did not — every long page — it goes on
    // a short line of its own underneath, if this list allows that.
    const bool hasMeta = meta[0] != '\0';
    bool metaInline =
        hasMeta && FEED_TEXT_X + lcd.getUTF8Width(lines[count - 1]) + 14 +
                           metaW <= W - FEED_PAD;
    bool metaOwn = hasMeta && !metaInline && metaLine;
    if (metaOwn && count * FEED_LEAD + FEED_META_LEAD > space) {
      // The bottom of the list, with no height left for that line. The text
      // gives up its last line to it rather than the page losing its sender:
      // it ends in "..." either way.
      if (count > 1) {
        count = wrapText(items[i].text, textW, count - 1, lines);
        metaInline = FEED_TEXT_X + lcd.getUTF8Width(lines[count - 1]) + 14 +
                         metaW <= W - FEED_PAD;
        metaOwn = !metaInline;
      } else {
        metaOwn = false;
      }
    }

    const int h = count * FEED_LEAD + (metaOwn ? FEED_META_LEAD : 0);
    const int lastBase = y + 12 + (count - 1) * FEED_LEAD;

    // A rule down the left groups the wrapped lines into one item without
    // spending a line of height on a bullet.
    lcd.drawBox(FEED_PAD, y + 3, FEED_MARK_W, h - 6);
    for (int l = 0; l < count; l++) {
      lcd.drawUTF8(FEED_TEXT_X, y + 12 + l * FEED_LEAD, lines[l]);
    }

    lcd.setFont(u8g2_font_6x12_t_cyrillic);
    if (metaInline) {
      drawRight(meta, W - FEED_PAD, lastBase);
    } else if (metaOwn) {
      drawRight(meta, W - FEED_PAD, lastBase + FEED_META_LEAD);
    }

    y += h + FEED_ITEM_GAP;
  }

  drawFeedFooter(footer, next);
}

static void drawNewsScreen(const UiState &s) {
  char footer[48];
  const char *at = newsStamp();

  if (s.busy) {
    // This frame is drawn before the radio comes up, and the panel then holds
    // it for the several seconds that takes, so it has to say what the wait
    // is for. Nothing else on the board can.
    snprintf(footer, sizeof(footer), "обновляю...");
  } else {
    snprintf(footer, sizeof(footer),
             at[0] != '\0' ? "обновлено %s" : "нет данных", at);
  }

  // Reaching this screen is what triggers a fetch, so once the radio has been
  // and gone, empty means it failed — there is nothing left to wait for.
  const char *why =
      s.busy ? "загружаю новости..." : "не удалось загрузить новости";
#ifndef WIFI_SSID
  why = "нет учётных данных Wi-Fi в .env";
#elif !defined(NEWS_API_KEY)
  why = "нет ключа NEWS_API в .env";
#endif
  drawFeedScreen(s, kNewsHeading, newsItems(), newsCount(), NEWS_MAX_LINES,
                 NEWS_META_LINE, footer, why, "ПЕЙДЖЕР");
}

static void drawPagerScreen(const UiState &s) {
  char footer[48];
  const char *at = pagerStamp();
  const char *state = pagerStatus();

  // The footer is the session, not the age of the list: this screen is one
  // you stand in front of waiting, and "на связи" is the thing worth knowing.
  if (s.busy) {
    snprintf(footer, sizeof(footer), "подключаюсь...");
  } else if (state[0] != '\0' && at[0] != '\0') {
    snprintf(footer, sizeof(footer), "%s · %s", state, at);
  } else if (state[0] != '\0') {
    snprintf(footer, sizeof(footer), "%s", state);
  } else {
    snprintf(footer, sizeof(footer), at[0] != '\0' ? "обновлено %s" : "нет данных",
             at);
  }

  // An empty pager is three different situations and they need three
  // different answers: still connecting, connected with an empty inbox, or
  // something in the chain is broken. pagerError() carries the last one — an
  // unreachable proxy and an empty inbox look identical otherwise, and they
  // are very different things to go and fix.
  const char *why;
  if (s.busy) {
    why = "подключаюсь к телеграму...";
  } else if (pagerLive()) {
    why = "жду сообщений";
  } else if (pagerError() != nullptr) {
    why = pagerError();
  } else {
    why = "нет связи с телеграмом";
  }
#ifndef WIFI_SSID
  why = "нет учётных данных Wi-Fi в .env";
#elif !defined(TELEGRAM_BOT_TOKEN)
  why = "нет TOKEN_BOT в .env";
#elif !defined(SOCKS5_HOST)
  why = "нет SOCKS5_HOST в .env";
#endif
  drawFeedScreen(s, kPagerHeading, pagerItems(), pagerCount(), PAGER_MAX_LINES,
                 PAGER_META_LINE, footer, why, "ЧАСЫ");
}

void uiDraw(const UiState &s) {
  lcd.clearBuffer();

  switch (s.screen) {
    case UI_SCREEN_NEWS:
      drawNewsScreen(s);
      break;
    case UI_SCREEN_PAGER:
      drawPagerScreen(s);
      break;
    default:
      drawStatusBar(s);
      drawClock(s);
      drawOutdoorRow(s);
      drawForecast(s);
      drawFooter();
      break;
  }

  lcd.sendBuffer();
}
