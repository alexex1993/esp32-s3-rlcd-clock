// Clock, outdoor conditions, 12-hour forecast and an 18650 gauge, for the
// Waveshare ESP32-S3-RLCD-4.2.
//
// Time comes from the on-board PCF85063A RTC, which holds *local* time
// (UTC+3, see TZ_OFFSET_SECONDS in app.h). The board ships with an empty
// backup-cell holder, so on a cold start the RTC's oscillator-stop flag is
// set and its calendar is meaningless. Three ways to fix that, in the order
// they are tried:
//
//   1. Wi-Fi + NTP, if credentials were compiled in (see below).
//   2. The build timestamp, as a fallback so the face is never blank.
//   3. Typed at the USB console: "T2026-09-09 21:30:00" then Enter.
//
// With credentials present the sync is not a one-off. Every ONLINE_PERIOD_MS
// (30 min) the radio comes up for one short window that does two things and
// then shuts down again:
//
//   * re-disciplines the RTC from NTP, whose crystal drifts a few seconds a
//     day on its own, and
//   * pulls the current conditions, the air quality and UV index, the next
//     twelve hours of weather and today's sunset for Brateevo (see
//     weather.cpp).
//
// Both ride the same window on purpose: the radio is the expensive part, and
// the panel is unreadable-stale for as long as it is up.
//
// The temperature, humidity and pressure on the face are the outdoor ones from
// that fetch, not the on-board SHTC3: this is a clock you read to know what to
// wear. The SHTC3 is still on the bus and still readable with "?" at the
// console — it is simply not on the panel.
//
// KEY (GPIO18) steps through four screens — the clock, the world headlines,
// the Telegram pager and the gaming PC's telemetry — and the clock comes back
// on its own from the headlines after SCREEN_HOLD_MS.
//
// The clock face is also the only screen that is built to be *cheap*, because
// it is the one the thing spends its life on. It carries no seconds, so it is
// rewritten once a minute rather than sixty times; the panel rests in its own
// low-power mode between those writes; the CPU drops to 80 MHz; and the SoC
// spends the minute in light sleep, waking just past the boundary with the new
// value already in the RTC. KEY pulls GPIO18 low, which is the second wake
// source, so the button is as live asleep as awake. Overnight — NIGHT_START_
// HOUR to NIGHT_END_HOUR — the scheduled sync window is held off as well: a
// forecast fetched between 1 and 7 is one nobody reads.
//
// None of that applies to the other three screens. They are what somebody is
// standing in front of, and the pager and the PC screen are holding a socket
// open besides, so they run at full rate.
//
// Both lists come up *only* on the way into their screen: there is no timer
// behind either and they take no part in the sync window above. So the radio
// comes up for a few seconds while you stand there waiting, which is why both
// draw the screen once before they start. The upside is that a clock nobody is
// looking at spends nothing on either.
//
// The two differ in what happens next. The headlines are one fetch and the
// radio goes straight back down. The pager is a *session*: the radio and the
// TLS connection stay up for as long as its screen does, getUpdates is
// long-polled over that one connection, and a message typed into the bot is
// on the panel about a second later — with a chirp out of the speaker. That
// is also why the pager is the one screen that does not fold itself back to
// the clock: it is there to be watched. It costs perhaps 80 mA of the 18650
// while it is open, so nothing opens it but a deliberate press of KEY.
//
// The pager reaches Telegram over a SOCKS5 tunnel — api.telegram.org is not
// reachable from here directly, so the proxy is not optional and the screen
// says so when .env does not configure one. See telegram.cpp and socks5.cpp.
//
// The PC screen is the second session. It holds Wi-Fi and one keep-alive HTTP
// connection to rtss_api on the gaming PC (RTSS_HOST in .env) and puts a fresh
// sample of the GPU, CPU, memory and frame rate on the panel twice a second —
// see rtss.cpp. Like the pager it stays up until KEY moves on, and while it is
// up it keeps asking for the network back if Wi-Fi drops. Stepping from the
// pager straight to it keeps the radio up across the switch.
//
// Credentials come from .env in the project root (SSID= / PWD= / NEWS_API= /
// TOKEN_BOT= / SOCKS5_* / RTSS_HOST=), which scripts/env_flags.py turns into
// -D macros at build time.
//
// Console: the single Type-C socket is the S3's native USB and there is no
// UART bridge chip, so -DARDUINO_USB_MODE=1 and -DARDUINO_USB_CDC_ON_BOOT=1
// in platformio.ini are what make Serial reach it at all.

#include <Arduino.h>

#include "app.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

#ifdef WIFI_SSID
#include <WiFi.h>
#include "esp_sntp.h"
#endif

// Read once a minute rather than every ten seconds: the cell does not move
// fast, and on the clock face every reading is a wake that would otherwise
// not have happened.
static const uint32_t BATTERY_PERIOD_MS = 60000;

#ifdef WIFI_SSID
static const uint32_t ONLINE_PERIOD_MS = 30UL * 60 * 1000;  // NTP + weather
static const uint32_t ONLINE_RETRY_MS = 60UL * 1000;        // sooner after a failure
static uint32_t s_nextOnlineMs = 0;
#endif

// How long a list screen stays up with nobody pressing anything. The panel
// holds its image with the radio and the CPU idle, so this costs nothing to
// wait out — it is about the clock coming back, not about power.
static const uint32_t SCREEN_HOLD_MS = 45000;

// Two presses closer together than this are one press bouncing.
static const uint32_t KEY_DEBOUNCE_MS = 200;

// --- what the clock face costs --------------------------------------------
// Nearly all of it was the SoC being awake for a panel that changes once a
// minute. The sleep at the foot of loop() is the answer to that; the rest of
// these are the smaller levers around it.

// The face is read to the minute, so the RTC is sampled to the second. Off
// the clock face the sample is finer (250 ms in loop()), because there an
// HH:MM in the bar sits over a screen being polled every 5 ms and stale reads
// worse than cheap.
static const uint32_t CLOCK_SAMPLE_MS = 1000;

// The sleep aims just *past* the minute boundary rather than just before it:
// one wake a minute that already has the new value, instead of two around it.
static const uint32_t WAKE_OVERSHOOT_MS = 80;

// Shorter than this and the entry and exit are not worth it, so the pass just
// paces itself the ordinary way.
static const uint32_t SLEEP_FLOOR_MS = 250;

// 240 MHz is what the sessions want: the pager walks a certificate chain and
// the PC screen parses an answer twice a second. The clock face needs none of
// it. 80 is a floor rather than a tuning knob — below it the USB-Serial-JTAG
// console stops working, and on this board that is the only console there is.
static const uint32_t CPU_MHZ_IDLE = 80;
static const uint32_t CPU_MHZ_BUSY = 240;

#ifdef WIFI_SSID
// The hours the sync window opens half as often. Not a window that stops: the
// RTC wants disciplining whether or not anybody is awake for it, and a clock
// that drifts overnight is the one fault this thing cannot have. What is not
// worth the radio between 1 and 7 is the second forecast of each hour, which
// nobody reads. Local time, which is what the RTC holds.
static const int NIGHT_START_HOUR = 1;
static const int NIGHT_END_HOUR = 7;
static const uint32_t NIGHT_ONLINE_PERIOD_MS = 60UL * 60 * 1000;
#endif

static UiState s_ui;
// Whether a trustworthy *source* has ever set the clock. s_ui.timeValid is the
// narrower question the face asks — that, and the RTC still answering now.
static bool s_timeValid = false;
// Every screen ticks on the minute now: the clock face joined the other three
// when it gave up its seconds, and nothing on any of them moves faster.
static int s_lastMinute = -1;
static UiScreen s_screen = UI_SCREEN_CLOCK;
static uint32_t s_screenMs = 0;
static uint32_t s_lastBatteryMs = 0;
static char s_note[32] = "";

// Makes the next pass through loop() rewrite the panel, whatever the minute
// on it says.
static void forceRedraw() { s_lastMinute = -1; }

// --- time sources ---------------------------------------------------------

// Parses the compiler's __DATE__ ("Sep  9 2026") and __TIME__ ("21:30:00").
// Not accurate — it is the moment the firmware was built, not the moment it
// started running — but it puts a plausible date on the face and keeps the
// weekday and month-length arithmetic honest until a real source arrives.
static bool buildTimestamp(struct tm *out) {
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int day, year, hh, mm, ss;

  if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return false;
  if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) return false;

  const char *p = strstr(months, mon);
  if (p == nullptr) return false;

  memset(out, 0, sizeof(*out));
  out->tm_mon = (int)(p - months) / 3;
  out->tm_mday = day;
  out->tm_year = year - 1900;
  out->tm_hour = hh;
  out->tm_min = mm;
  out->tm_sec = ss;
  out->tm_isdst = 0;
  mktime(out);  // fills in tm_wday
  return true;
}

// Applies a calendar to both the RTC and the ESP's own clock, so time keeps
// running even if the RTC drops off the bus later.
static bool applyTime(struct tm *t, const char *source) {
  mktime(t);  // normalise, and derive tm_wday

  struct tm copy = *t;
  time_t local = mktime(&copy);
  struct timeval tv = {.tv_sec = local, .tv_usec = 0};
  settimeofday(&tv, nullptr);

  bool ok = rtcSetTime(t);
  Serial.printf("time set from %s: %04d-%02d-%02d %02d:%02d:%02d (RTC write %s)\n",
                source, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour,
                t->tm_min, t->tm_sec, ok ? "ok" : "FAILED");
  return ok;
}

#ifdef WIFI_SSID
// The face is frozen for as long as an attempt lasts, so every window is kept
// as short as the network allows: the NTP one has to cover a DNS lookup for the
// pool plus the first poll, which is what the earlier 8 s was losing to.
static const uint32_t WIFI_TIMEOUT_MS = 12000;
static const uint32_t NTP_TIMEOUT_MS = 15000;

// Set from the SNTP task when an answer has actually been applied. Waiting on
// this rather than on getLocalTime() matters for every sync after the first:
// once the ESP's clock is plausible, getLocalTime() returns straight away with
// the *old* time, and a resync that never heard from a server would look like
// a success and write the drifting clock back into the RTC.
static volatile bool s_ntpUpdated = false;

static void onNtpUpdate(struct timeval *tv) { s_ntpUpdated = true; }

// Brings Wi-Fi up briefly, takes the time from NTP and the forecast from
// Open-Meteo, then shuts the radio down again — the panel needs no network to
// keep running.
//
// The two errands are reported separately because they mean different things:
// *timeSynced decides whether the face is trustworthy, and the return value —
// true only when both succeeded — decides whether the next window is the long
// one or the short retry.
static bool syncOnline(bool *timeSynced) {
  Serial.printf("wifi: connecting to \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (timeSynced != nullptr) *timeSynced = false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi: no connection, keeping the RTC and forecast as they are");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  // configTime() takes the offset, so localtime() below already returns UTC+3
  // and the RTC gets local time, which is what it is expected to hold.
  s_ntpUpdated = false;
  sntp_set_time_sync_notification_cb(onNtpUpdate);
  configTime(TZ_OFFSET_SECONDS, 0, "pool.ntp.org", "time.google.com",
             "time.cloudflare.com");

  t0 = millis();
  while (!s_ntpUpdated && millis() - t0 < NTP_TIMEOUT_MS) delay(50);

  struct tm t;
  bool timeOk = s_ntpUpdated && getLocalTime(&t, 0);
  if (timeOk) {
    timeOk = applyTime(&t, "NTP");
  } else {
    Serial.println("ntp: no answer");
  }

  // The forecast is fetched second: it is stamped with the local time, and by
  // now that is the time NTP just handed over rather than yesterday's drift.
  const bool weatherOk = weatherFetch();
  // Left out of the verdict on purpose: two numbers from a second service are
  // not worth reopening the window every sixty seconds while it is down. The
  // next scheduled window simply asks again.
  airQualityFetch();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  if (timeSynced != nullptr) *timeSynced = timeOk;
  return timeOk && weatherOk;
}
#endif  // WIFI_SSID

// --- the radio, and what it is brought up for -----------------------------

#ifdef WIFI_SSID
// Brings Wi-Fi up and waits for it, or gives up after WIFI_TIMEOUT_MS.
//
// Deliberately not syncOnline(): that window belongs to the clock and the
// forecast and opens on a timer, while these open because somebody pressed a
// button and is standing in front of the panel. They leave the sync schedule
// alone, so a visit to a list screen neither brings the next NTP window
// forward nor pushes it back.
static bool radioUp(const char *what) {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("%s: radio up", what);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("%s: no connection\n", what);
  WiFi.mode(WIFI_OFF);
  return false;
}

static void radioDown() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// One errand with the radio up, then down again: the headlines' shape.
__attribute__((unused)) static void withRadio(const char *what,
                                              bool (*errand)()) {
  if (radioUp(what)) {
    errand();
  } else {
    Serial.printf("%s: keeping what is on the screen\n", what);
  }
  radioDown();
}
#endif

#if defined(WIFI_SSID) && defined(NEWS_API_KEY)
static void refreshNews() { withRadio("news", newsFetch); }
#else
// Without both the credentials and the key there is nothing to ask, and no
// reason to spend twelve seconds of radio finding that out. The screen says
// which of the two is missing.
static void refreshNews() {}
#endif

// --- the pager session ----------------------------------------------------

#ifdef PAGER_ENABLED
// The Arduino core declares this weak so a sketch can widen its own task.
// The pager is the only thing on the board that verifies a certificate, and
// walking a chain against the ESP-IDF root bundle wants a few kilobytes of
// stack that the default 8 KB does not comfortably have left once this file's
// own frames are on it. The cost is 4 KB of internal RAM out of ~250 KB free.
size_t getArduinoLoopTaskStackSize() { return 12 * 1024; }

// The pager holds the radio for as long as its screen is up — that is the
// whole point of it, and the reason it does not time out back to the clock.
// It costs perhaps 80 mA of the 18650 while it is open, which is why nothing
// opens it but a deliberate press of KEY.
static void pagerStart() {
  if (!radioUp("pager")) return;
  pagerOpen();
}
#else
static void pagerStart() {}
#endif

// --- the PC session -------------------------------------------------------

#ifdef PC_ENABLED
// How long Wi-Fi may stay gone with the PC screen up before it is asked for
// again. Longer than a join takes, so a rejoin already under way is not
// interrupted by the next one.
static const uint32_t WIFI_REJOIN_MS = 20000;

// Unlike the pager, the session opens whether or not Wi-Fi came up: this
// screen is meant to stay connected, and pcKeepRadio() goes on asking for the
// network for as long as it is up.
static void pcStart() {
  radioUp("pc");
  pcOpen();
}

// The core rejoins by itself after most drops — a lost beacon, a roaming AP —
// but not after every one, and not at all once radioUp() has given up and
// switched the radio off. So while the screen is up, a network that has been
// gone for WIFI_REJOIN_MS is asked for again, without waiting on it: the
// session in rtss.cpp notices when it is back.
static void pcKeepRadio() {
  static uint32_t s_wifiSeenMs = 0;
  if (WiFi.status() == WL_CONNECTED) {
    s_wifiSeenMs = millis();
    return;
  }
  if (millis() - s_wifiSeenMs < WIFI_REJOIN_MS) return;
  s_wifiSeenMs = millis();
  Serial.println("pc: wifi is down, rejoining");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}
#else
static void pcStart() {}
static void pcKeepRadio() {}
#endif

#ifdef WIFI_SSID
// The screens that keep Wi-Fi up for as long as they are showing: the two
// sessions, where this build has them.
static bool holdsRadio(UiScreen screen) {
#ifdef PAGER_ENABLED
  if (screen == UI_SCREEN_PAGER) return true;
#endif
#ifdef PC_ENABLED
  if (screen == UI_SCREEN_PC) return true;
#endif
  (void)screen;
  return false;
}
#endif

// --- sensors --------------------------------------------------------------

static void readBattery() {
  s_ui.volts = batteryVolts();
  s_ui.percent = batteryPercent(s_ui.volts);
  snprintf(s_note, sizeof(s_note), "%s", s_ui.percent <= 10 ? "НИЗКИЙ ЗАРЯД" : "");
}

// --- the clock the face is drawn from -------------------------------------

// The RTC sits on a 100 kHz bus and a read is about a millisecond. That is
// nothing once a second, but the pager runs the loop at 5 ms so it can watch
// its socket, and reading the RTC every pass there would spend a fifth of the
// CPU re-fetching a value that only feeds an HH:MM. So it is sampled on a
// schedule of its own and in between the last reading stands. Pass 0 to force
// one — after the sync window, which blocks for seconds.
static struct tm s_clock;
static bool s_clockRead = false;
static bool s_clockOk = false;
static uint32_t s_clockMs = 0;

static void clockSample(uint32_t period) {
  if (s_clockRead && (millis() - s_clockMs) < period) return;
  s_clockMs = millis();
  s_clockRead = true;
  if (rtcPresent()) {
    // A false here means the RTC lost power while running; s_timeValid stays
    // the authority on whether the *source* was ever trustworthy.
    s_clockOk = rtcReadTime(&s_clock);
  } else {
    // No RTC on the bus — fall back to the ESP's own clock, which applyTime()
    // keeps in step whenever a real source turns up.
    const time_t t = time(nullptr);
    localtime_r(&t, &s_clock);
    s_clockOk = true;
  }
}

#ifdef WIFI_SSID
// A clock that was never set has no night: the hour in it is whatever the
// RTC's registers happened to hold, and stretching the sync window on that
// would hide the one thing that could still fix it.
static bool isNight(const struct tm &t, bool valid) {
  return valid && t.tm_hour >= NIGHT_START_HOUR && t.tm_hour < NIGHT_END_HOUR;
}
#endif

// --- how hard the SoC works -----------------------------------------------

// The clock face is the screen this thing spends its life on, so it is the one
// built to be cheap; the other three are what somebody is standing in front of.
static void cpuForScreen(UiScreen screen) {
  const uint32_t want =
      (screen == UI_SCREEN_CLOCK) ? CPU_MHZ_IDLE : CPU_MHZ_BUSY;
  if ((uint32_t)getCpuFrequencyMhz() != want) setCpuFrequencyMhz(want);
}

// The clock face's idle. Both cores stop, RAM and every peripheral keep their
// state, the panel keeps its image, and two things bring it back: the timer,
// and KEY pulling GPIO18 low. GPIO18 idles high through the board's own 10K
// pull-up, so a level-triggered wake on low needs no extra hardware, and the
// button is as live asleep as it is awake.
//
// Coming back is a *return from this function*, not a reset — the forecast,
// the headlines, the Wi-Fi configuration and millis() all survive it, which is
// the reason this is light sleep and not deep sleep. Deep sleep would cost a
// full boot and a fresh sync window on every press of KEY, for a difference
// this board's LDO and its four powered I2C slaves would mostly swallow.
// esp_timer is resynchronised from the RTC across the sleep, so the sync
// schedule is still true on the other side of it.
static void clockSleep(uint32_t ms) {
  // A level-triggered wake source that is already asserted returns from the
  // sleep immediately, so a held-down KEY would spin here. Let loop() see the
  // press instead.
  if (digitalRead(PIN_BTN_KEY) == LOW) {
    delay(10);
    return;
  }

  gpio_wakeup_enable((gpio_num_t)PIN_BTN_KEY, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);

  esp_light_sleep_start();

  // Nothing is left armed: the session screens run at full rate with the radio
  // up, and a wake source still attached to a button there is one nobody asked
  // for.
  gpio_wakeup_disable((gpio_num_t)PIN_BTN_KEY);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
}

// "T2026-09-09 21:30:00" (the space may also be a 'T'), typed at the console.
static void handleConsole() {
  static char line[64];
  static size_t len = 0;

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;
    }
    line[len] = '\0';
    len = 0;

    if (line[0] == 'T' || line[0] == 't') {
      struct tm t;
      memset(&t, 0, sizeof(t));
      int y, mo, d, hh, mm, ss;
      if (sscanf(line + 1, "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &hh, &mm, &ss) == 6) {
        t.tm_year = y - 1900;
        t.tm_mon = mo - 1;
        t.tm_mday = d;
        t.tm_hour = hh;
        t.tm_min = mm;
        t.tm_sec = ss;
        applyTime(&t, "console");
        s_timeValid = true;
        forceRedraw();
      } else {
        Serial.println("usage: T2026-09-09 21:30:00");
      }
#ifdef WIFI_SSID
    } else if (line[0] == 'N' || line[0] == 'n') {
      // Forces the scheduled window to open on the next pass through loop().
      // Time and weather only: the headlines are KEY's business and nothing
      // else's.
      Serial.println("online: syncing now");
      s_nextOnlineMs = millis();
#endif
#ifdef PAGER_ENABLED
    } else if (line[0] == 'P' || line[0] == 'p') {
      // The headlines have no console command on purpose — KEY is the only
      // way in. The pager gets one because the path behind it has three
      // separate things that can be wrong in .env (the proxy, its
      // credentials, the bot token), and pressing a button on the desk is a
      // poor way to read the reason off the console.
      //
      // One answer, not a session: open, spin until something comes back —
      // and the 👀 for whatever it brought have gone out — or the patience
      // runs out, then close. Skipped while the screen is up, where a session
      // is already running and a second one would fight it.
      if (s_screen == UI_SCREEN_PAGER) {
        Serial.println("pager: the screen is up, the session is already live");
      } else if (radioUp("pager")) {
        const uint32_t before = pagerResponses();
        pagerOpen();
        const uint32_t t0 = millis();
        while ((pagerResponses() == before || pagerReacting()) &&
               millis() - t0 < 40000) {
          pagerPoll();
          delay(10);
        }
        if (pagerResponses() == before) Serial.println("pager: gave up waiting");
        // Typed on the PC screen, the radio stays: that screen's own session
        // is still riding the link a diagnostic borrowed.
        pagerClose();
        if (!holdsRadio(s_screen)) radioDown();
        forceRedraw();
      } else {
        radioDown();
      }
#endif
    } else if (line[0] == 'B' || line[0] == 'b') {
      // The speaker is the one thing here with no other way to test it: it
      // is silent when it works and silent when it does not.
      Serial.printf("audio: ES8311 %s, chirping\n",
                    audioPresent() ? "present" : "MISSING");
      audioNotify();
    } else if (line[0] == '?') {
      Serial.printf("RTC %s, time %s, battery %.2f V (%d%%)",
                    rtcPresent() ? "present" : "MISSING",
                    s_timeValid ? "valid" : "not set", s_ui.volts,
                    s_ui.percent);
      // The SHTC3 is off the panel now, so it is read here on demand rather
      // than kept fresh in the background: it is a diagnostic, not a display.
      float inT, inRh;
      if (shtc3Read(&inT, &inRh)) {
        Serial.printf(", indoor %.1f C / %.0f%% RH", inT, inRh);
      } else {
        Serial.print(", SHTC3 not reading");
      }
      WeatherNow now;
      if (weatherNow(&now)) {
        Serial.printf(", outdoor %.1f C / %.0f%% RH / %.0f hPa, AQI %.0f, UV %.1f",
                      now.temp, now.humidity, now.pressure, now.aqi, now.uv);
      }
      Serial.printf(", forecast %s, sunset %s",
                    weatherValid() ? weatherStamp() : "none",
                    weatherSunset()[0] != '\0' ? weatherSunset() : "unknown");
      Serial.printf(", news %d headlines %s", newsCount(),
                    newsStamp()[0] != '\0' ? newsStamp() : "(never)");
      Serial.printf(", pager %d messages %s", pagerCount(),
                    pagerStamp()[0] != '\0' ? pagerStamp() : "(never)");
      if (pagerError() != nullptr) Serial.printf(" [%s]", pagerError());
#ifdef PAGER_ENABLED
      Serial.printf(", session %s after %lu answers",
                    pagerLive() ? "live" : "down",
                    (unsigned long)pagerResponses());
      Serial.printf(", proxy %s:%u", socks5Host(), (unsigned)socks5Port());
#endif
#ifdef PC_ENABLED
      Serial.printf(", pc %s after %lu answers from %s:%u",
                    pcLive() ? "live" : "down", (unsigned long)pcResponses(),
                    pcHost(), (unsigned)pcPort());
      if (pcError() != nullptr) Serial.printf(" [%s]", pcError());
#endif
      Serial.printf(", speaker %s", audioPresent() ? "ready" : "MISSING");
#ifdef WIFI_SSID
      Serial.printf(", next sync in %ld s",
                    (long)((int32_t)(s_nextOnlineMs - millis()) / 1000));
#endif
      Serial.println();
    } else if (line[0] != '\0') {
      Serial.println("commands: T<YYYY-MM-DD HH:MM:SS> to set the clock,"
#ifdef WIFI_SSID
                     " N to sync time and weather now,"
#endif
#ifdef PAGER_ENABLED
                     " P to read telegram now,"
#endif
                     " B to test the speaker,"
                     " ? for status; KEY steps clock -> news -> pager -> pc,"
                     " and the pager and pc screens stay up until KEY says"
                     " otherwise");
    }
  }
}

// --- setup / loop ---------------------------------------------------------

void setup() {
  Serial.begin(115200);
  // The CDC device does not exist until the host enumerates it; without this
  // wait the first lines are printed into a void.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);

  Serial.println("\nESP32-S3-RLCD-4.2 clock + weather, " TZ_LABEL);

  memset(&s_ui, 0, sizeof(s_ui));
  s_ui.note = s_note;

  pinMode(PIN_BTN_KEY, INPUT);  // external 10K pull-up, active low
  batteryBegin();
  displayBegin();

  if (!rtcBegin()) {
    Serial.println("rtc: PCF85063A did not answer at 0x51");
  }
  // rtcBegin() owns Wire.begin(), so everything else on the bus is probed
  // after it.
  if (!shtc3Begin()) {
    Serial.println("shtc3: no answer at 0x70, \"?\" will not report indoors");
  }
  if (!audioBegin()) {
    Serial.println("audio: no ES8311 at 0x18, the pager will page silently");
  }

  s_timeValid = rtcReadTime(&s_ui.time);
  if (!s_timeValid) {
    // Expected on a board whose backup-cell holder is empty: the RTC loses
    // time on every power cycle, and says so through the OS flag.
    Serial.println("rtc: oscillator-stop flag set, the calendar is not trustworthy");
  }

#ifdef WIFI_SSID
  // One window at boot either way. Even a clock that survived needs it: the
  // forecast lives in RAM and does not survive anything.
  //
  // The result only ever *adds* trust: a failed window must not invalidate an
  // RTC that kept the time across the power cycle.
  bool ntpOk = false;
  const bool allOk = syncOnline(&ntpOk);
  if (ntpOk) s_timeValid = true;
  if (s_timeValid) rtcReadTime(&s_ui.time);
#else
  const bool allOk = false;
#endif

  if (!s_timeValid && buildTimestamp(&s_ui.time)) {
    // Seeded, but deliberately not marked valid: the RTC now runs and its OS
    // flag is clear, so nothing downstream would ever admit that this is the
    // moment of compilation rather than the moment of reading. The face keeps
    // saying "clock not set" until a real source replaces it.
    applyTime(&s_ui.time, "the build timestamp");
  }

  if (!s_timeValid) {
    Serial.println("set the clock with: T2026-09-09 21:30:00");
  }

#ifdef WIFI_SSID
  const uint32_t firstGap = allOk ? ONLINE_PERIOD_MS : ONLINE_RETRY_MS;
  s_nextOnlineMs = millis() + firstGap;
  Serial.printf("online: next sync in %lu min\n",
                (unsigned long)(firstGap / 60000));
#else
  (void)allOk;
#endif

  readBattery();
  s_lastBatteryMs = millis();
  Serial.printf("battery: %.2f V (%d%%)\n", s_ui.volts, s_ui.percent);

  s_ui.timeValid = s_timeValid;
  s_ui.screen = s_screen;
  uiDraw(s_ui);
  s_lastMinute = s_ui.time.tm_min;
  s_screenMs = millis();

  // The board comes up on the clock face, so it comes up in that face's two
  // cheap states. Both were left alone until here on purpose: the boot sync
  // window above wanted the radio, and the first frame wanted the panel at
  // full rate.
  displayLowPower(true);
  cpuForScreen(s_screen);
}

void loop() {
  handleConsole();

  const uint32_t nowMs = millis();

  // Sampled before anything else needs it: the window below asks what hour it
  // is, and the sleep at the foot asks how much of the minute is left.
  clockSample(s_screen == UI_SCREEN_CLOCK ? CLOCK_SAMPLE_MS : 250);

#ifdef WIFI_SSID
  // Signed difference, so the schedule survives the millis() rollover. Held
  // off while a session screen is up: that screen owns the radio and a sync
  // window would tear its connection down under it, for a clock nobody is
  // looking at just then.
  //
  // Which period follows a window is decided from the hour that window landed
  // in, not the one it opened in — it blocks for seconds and can straddle
  // 01:00 or 07:00 — so the clock is re-sampled first.
  if (holdsRadio(s_screen)) {
    s_nextOnlineMs = nowMs + ONLINE_RETRY_MS;
  } else if ((int32_t)(nowMs - s_nextOnlineMs) >= 0) {
    bool timeSynced = false;
    const bool ok = syncOnline(&timeSynced);
    clockSample(0);
    const uint32_t period =
        isNight(s_clock, s_clockOk && (s_timeValid || timeSynced))
            ? NIGHT_ONLINE_PERIOD_MS
            : ONLINE_PERIOD_MS;
    s_nextOnlineMs = millis() + (ok ? period : ONLINE_RETRY_MS);
    // A window that reached NTP but not Open-Meteo still fixed the clock.
    if (timeSynced) s_timeValid = true;
    forceRedraw();  // the face is stale after the attempt either way
  }
#endif

  if (nowMs - s_lastBatteryMs >= BATTERY_PERIOD_MS) {
    s_lastBatteryMs = nowMs;
    readBattery();
  }

  // KEY (GPIO18) steps through the screens on the falling edge. The foot of
  // loop() no longer paces this on its own — the pager screen runs at 5 ms so
  // it can watch its socket — so the debounce is explicit: a press inside
  // KEY_DEBOUNCE_MS of the last one is the same press bouncing.
  static bool keyWasDown = false;
  static uint32_t keyLastMs = 0;
  const bool keyDown = digitalRead(PIN_BTN_KEY) == LOW;
  if (keyDown && !keyWasDown && (nowMs - keyLastMs) >= KEY_DEBOUNCE_MS) {
    keyLastMs = nowMs;
    const UiScreen was = s_screen;
    s_screen = (UiScreen)((s_screen + 1) % UI_SCREEN_COUNT);
    readBattery();

    // Leaving a session screen always ends its session. The radio goes down
    // with it unless the next screen is a session too and would only have to
    // bring it straight back up — which is how the pager hands it to the PC
    // screen without a second wait for Wi-Fi.
    if (was == UI_SCREEN_PAGER) pagerClose();
    if (was == UI_SCREEN_PC) pcClose();
#ifdef WIFI_SSID
    if (holdsRadio(was) && !holdsRadio(s_screen)) radioDown();
#endif

    // The clock face rests cheap and the other three do not: the panel goes to
    // LPM and the CPU to 80 MHz only where a frame a minute is the most that
    // can happen. Done before the frame below, so the busy frame is already
    // written at the rate the new screen will keep.
    displayLowPower(s_screen == UI_SCREEN_CLOCK);
    cpuForScreen(s_screen);

    if (s_screen != UI_SCREEN_CLOCK) {
      // Bringing the radio up takes seconds and the panel is frozen for every
      // one of them, so the press gets a frame of its own before the wait
      // rather than after it. s_ui still carries the time from the last draw,
      // which is a second old at worst and only feeds the HH:MM in the bar.
      s_ui.screen = s_screen;
      s_ui.busy = true;
      uiDraw(s_ui);
      s_ui.busy = false;
      if (s_screen == UI_SCREEN_NEWS) {
        refreshNews();
      } else if (s_screen == UI_SCREEN_PAGER) {
        pagerStart();
      } else {
        pcStart();
      }
    }
    // Started after the radio came up, not before it: the hold is time to
    // read what came back, and the seconds the radio took are not that.
    s_screenMs = millis();
    forceRedraw();
  }
  keyWasDown = keyDown;

  // The news screen never stays up for good; the pager and the PC screen do.
  // Standing there watching for a message, or glancing at a frame rate, is
  // the whole reason either exists, and a screen that folded itself away
  // after 45 seconds would be useless for it — so both stay until KEY says
  // otherwise. millis() rather than nowMs, which a fetch just above may have
  // left seconds behind: stale, it would read as a huge elapsed time and
  // bounce straight back to the clock.
  if (s_screen == UI_SCREEN_NEWS && millis() - s_screenMs >= SCREEN_HOLD_MS) {
    s_screen = UI_SCREEN_CLOCK;
    forceRedraw();
  }

  // The live session. Only PG_CONNECT and the parse block, so this is cheap
  // on the passes where nothing has arrived.
  bool chirp = false;
  if (s_screen == UI_SCREEN_PAGER) {
    if (pagerPoll()) forceRedraw();
    // Held until after the redraw at the foot of this pass: the panel is what
    // somebody looks at when they hear it, and audioNotify() blocks for a
    // third of a second.
    chirp = pagerTakeArrival();
    if (chirp) forceRedraw();
  }

  // The PC screen: a sample twice a second, and the network kept up under it.
  // Nothing in here waits on the PC, so a switched-off one does not cost KEY.
  if (s_screen == UI_SCREEN_PC) {
    pcKeepRadio();
    if (pcPoll()) forceRedraw();
  }

  // Sampled again, and captured, only here: three things above this line can
  // block for seconds — the sync window, a headline fetch, opening a session —
  // and a frame drawn from a reading taken before one of them would carry a
  // minute that has since turned. The call is throttled, so on an ordinary
  // pass this is the reading the top of the loop already took.
  clockSample(s_screen == UI_SCREEN_CLOCK ? CLOCK_SAMPLE_MS : 250);
  const struct tm now = s_clock;
  const bool valid = s_clockOk && s_timeValid;

  // The panel holds its image on its own, so it is only rewritten when what it
  // shows has actually changed — which, on all four screens now, is once a
  // minute. The clock face joined the other three when it gave up its seconds.
  if (now.tm_min != s_lastMinute) {
    s_lastMinute = now.tm_min;
    s_ui.time = now;
    s_ui.timeValid = valid;
    s_ui.screen = s_screen;
    uiDraw(s_ui);
  }

  // The page is on the panel by now, so the noise and the thing it is about
  // arrive together.
  if (chirp) audioNotify();

  // The pace at the foot of the pass. The two session screens watch a socket
  // rather than a clock, so they are polled fast and never sleep; the PC's
  // answers are 72 bytes twice a second, which 10 ms keeps up with.
  //
  // The clock face is the opposite, and is where nearly all of the power goes:
  // nothing on it changes until the minute does, so it sleeps to the boundary
  // and wakes just past it with the new value already in the RTC. Three things
  // keep it awake even there:
  //
  //   * a USB console, which light sleep disconnects. A board on a desk with
  //     the monitor open stays exactly as responsive as it was, and a board on
  //     the 18650 sleeps. Nothing to configure — this is the difference
  //     between a host that has enumerated the port and a charger that has not.
  //   * Wi-Fi up, which off the session screens only happens inside the sync
  //     window, and that window blocks anyway.
  //   * a clock that is not set, which has no minute boundary to aim at and
  //     probably has somebody at the console about to give it one.
  uint32_t pace = 50;
  if (s_screen == UI_SCREEN_PAGER) pace = 5;
  if (s_screen == UI_SCREEN_PC) pace = 10;

  bool slept = false;
  if (s_screen == UI_SCREEN_CLOCK && valid && !Serial
#ifdef WIFI_SSID
      && WiFi.status() != WL_CONNECTED
#endif
  ) {
    // What is left of the minute, less the age of the reading that was worked
    // out from, plus the overshoot that lands the wake after the turn.
    const uint32_t age = millis() - s_clockMs;
    int32_t sleepMs = (int32_t)((60 - now.tm_sec) * 1000) - (int32_t)age +
                      (int32_t)WAKE_OVERSHOOT_MS;
#ifdef WIFI_SSID
    // Never sleep past a sync window that is already due.
    const int32_t toOnline = (int32_t)(s_nextOnlineMs - millis());
    if (toOnline > 0 && toOnline < sleepMs) sleepMs = toOnline;
#endif
    if (sleepMs >= (int32_t)SLEEP_FLOOR_MS) {
      clockSleep((uint32_t)sleepMs);
      slept = true;
    }
  }
  if (!slept) delay(pace);
}
