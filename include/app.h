#pragma once

#include <Arduino.h>
#include <time.h>

// Local time zone of the clock. The RTC holds *local* time, so this offset is
// applied only when a UTC source (NTP) is converted before being written.
#define TZ_OFFSET_SECONDS (3 * 3600)  // UTC+3, no daylight saving
#define TZ_LABEL "UTC+3"

// --- which modes this build carries ---------------------------------------
// The firmware is four screens and the [modes] section of platformio.ini says
// which of them to build. 1 builds a mode in, 0 leaves it out completely: no
// code, no fetch, no session, and no place in the cycle KEY steps through.
// This is what one board is for and another is not — somebody wants only the
// PC monitor, somebody else the clock and the pager.
//
// Defaulted here rather than required, so sources built without the ini —
// another project, a bare compiler check — are the full firmware.
#ifndef MODE_CLOCK
#define MODE_CLOCK 1  // time, outdoors, the 12-hour forecast, sunset, battery
#endif
#ifndef MODE_NEWS
#define MODE_NEWS 1  // world headlines, fetched on the way into the screen
#endif
#ifndef MODE_PAGER
#define MODE_PAGER 1  // the live Telegram pager, through the SOCKS5 tunnel
#endif
#ifndef MODE_PC
#define MODE_PC 1  // the gaming PC's GPU/CPU/VRAM/FPS, off rtss_api
#endif

// How many of them are on. Zero is a legal build: it still boots, and the
// panel says there is nothing switched on rather than sitting blank — a blank
// reflective panel and a board that never started look identical.
#define MODE_COUNT (MODE_CLOCK + MODE_NEWS + MODE_PAGER + MODE_PC)

// --- what this build can reach --------------------------------------------
// scripts/env_flags.py turns .env into these; each one gates a different
// amount of the firmware, and every branch must still build.
//
//   WIFI_SSID           the whole networking half
//   NEWS_API_KEY        the world-news screen
//   TELEGRAM_BOT_TOKEN  }  the pager screen, which is proxy-only on purpose:
//   SOCKS5_HOST         }  api.telegram.org is not reachable directly here.
//   RTSS_HOST           the PC screen: rtss_api on the gaming PC, on the LAN
//
// A mode and its credentials are two different questions, and the four
// switches below are where they meet: the mode says whether the screen is in
// this firmware at all, the .env macros say whether it has anything to talk
// to. A screen that is built but starved says on the panel which value is
// missing; a mode that is off says nothing, because it is not there.

// The radio itself. Nothing brings Wi-Fi up for a mode that is switched off,
// so a build with no modes at all never opens a sync window either.
#if defined(WIFI_SSID) && MODE_COUNT > 0
#define NET_ENABLED 1
#endif

// Open-Meteo is fetched for the clock face and nowhere else: it is the only
// screen that shows a forecast.
#if MODE_CLOCK && defined(NET_ENABLED)
#define WEATHER_ENABLED 1
#endif

#if MODE_NEWS && defined(NET_ENABLED) && defined(NEWS_API_KEY)
#define NEWS_ENABLED 1
#endif

#if MODE_PAGER && defined(NET_ENABLED) && defined(TELEGRAM_BOT_TOKEN) && \
    defined(SOCKS5_HOST)
#define PAGER_ENABLED 1
// The ports are the only .env values that arrive as numbers rather than
// string literals, and each falls back on its protocol's default.
#ifndef SOCKS5_PORT
#define SOCKS5_PORT 1080
#endif
#endif

#if MODE_PC && defined(NET_ENABLED) && defined(RTSS_HOST)
#define PC_ENABLED 1
#ifndef RTSS_PORT
#define RTSS_PORT 8099  // rtss_api's own default, its -addr flag
#endif
#endif

// --- the place the forecast is for ----------------------------------------
// Brateevo, in the south-east of Moscow. Open-Meteo snaps this to the nearest
// node of its 1-2 km grid, so the third decimal is decoration.
#define WEATHER_LAT   "55.6386"
#define WEATHER_LON   "37.7561"
#define WEATHER_TZ    "Europe%2FMoscow"  // %2F: it goes into a query string

// --- rtc.cpp --------------------------------------------------------------

// Brings up the shared I2C bus (SDA 13 / SCL 14) and probes the PCF85063A.
bool rtcBegin();

// True if the RTC answered on the bus at startup.
bool rtcPresent();

// Reads the calendar. Returns false when the oscillator-stop flag is set,
// i.e. the RTC lost power and the value in *out is not trustworthy. A board
// with an empty backup-cell holder does this on every power cycle.
bool rtcReadTime(struct tm *out);

// Writes the calendar and clears the oscillator-stop flag.
bool rtcSetTime(const struct tm *t);

// --- battery.cpp ----------------------------------------------------------

void batteryBegin();

// 18650 terminal voltage, in volts, averaged over a few ADC samples.
float batteryVolts();

// State of charge, 0..100, from a resting li-ion discharge curve.
int batteryPercent(float volts);

// --- shtc3.cpp ------------------------------------------------------------

// Probes the on-board SHTC3 at 0x70. rtcBegin() must have run first: that is
// what brings the shared I2C bus up.
bool shtc3Begin();

bool shtc3Present();

// One shot: wake, measure, sleep. ~13 ms. Either pointer may be null.
bool shtc3Read(float *tempC, float *humidity);

// --- weather.cpp ----------------------------------------------------------

#define WEATHER_HOURS 12

struct WeatherHour {
  uint8_t hour;  // 0..23, local time at the forecast location
  float temp;    // degrees C, NAN when the source had no value
  uint8_t pop;   // probability of precipitation, 0..100
  uint8_t code;  // WMO weather code, see wmo→icon mapping in ui.cpp
  bool day;      // daylight at that hour, for the sun/moon glyph
};

// The conditions right now at the same place, as Open-Meteo's `current` block
// reports them. Separate from the hourly strip on purpose: the strip's first
// column is the hour *in progress* (09:00 at 09:50), while this is a reading
// no more than fifteen minutes old.
struct WeatherNow {
  float temp;      // degrees C
  float humidity;  // % RH
  float pressure;  // hPa at the station's altitude, not reduced to sea level
  // These two come from Open-Meteo's air-quality service, by a request of
  // their own (airQualityFetch), and stay NAN until it has answered once.
  float aqi;       // European AQI: 0-20 good, 20-40 fair ... over 100 extreme
  float uv;        // UV index
};

// Fetches the next WEATHER_HOURS hours from Open-Meteo. Requires a live Wi-Fi
// connection; the caller owns the radio. On failure the previous forecast is
// kept, so the panel shows stale data rather than nothing.
bool weatherFetch();

// Fetches the European air quality index and the UV index for the same place
// into weatherNow()'s aqi and uv. Same terms as weatherFetch(): live Wi-Fi,
// the caller owns the radio, and a failure keeps the previous reading.
bool airQualityFetch();

// True once a fetch has succeeded at least since boot.
bool weatherValid();

// The current conditions. False until a fetch has filled them in; individual
// fields may still be NAN when the source had no value for one.
bool weatherNow(WeatherNow *out);

// Today's sunset as local "HH:MM", or "" if it has not been fetched yet.
const char *weatherSunset();

// Number of usable entries in weatherHours(), 0..WEATHER_HOURS.
int weatherCount();

const WeatherHour *weatherHours();

// Local "HH:MM" of the last successful fetch, or "" if there has not been one.
const char *weatherStamp();

// --- text.cpp -------------------------------------------------------------

// One line item on either of the two list screens: a headline or a page.
// Both screens are drawn by the same code in ui.cpp, so both fill the same
// struct — a news item's `from` is its outlet, a page's is whoever sent it.
//
// The caps are what the panel can show: a page may take the whole list area,
// fifteen lines of 41 cells in the 9x15 face, and Cyrillic is two bytes a
// cell — 1230 bytes at the very most. 1280 is past that, so a text cut here
// always overflows the panel too and is drawn ending in "..."; a headline is
// held to three lines when it is drawn and simply never fills it.
#define FEED_TEXT_CAP 1280
// 40 rather than the 24 a news outlet needs: a page is signed with whoever
// sent it, and "Анна Петрова" is already 23 bytes of Cyrillic.
#define FEED_FROM_CAP 40

struct FeedItem {
  // Already sanitised: every code point in here is one the panel's fonts can
  // actually draw. See textSanitise() below.
  char text[FEED_TEXT_CAP];
  char from[FEED_FROM_CAP];
  uint8_t hour, minute;  // local time it was published or sent
  bool timed;            // false when the source gave no usable timestamp
};

// Decodes one UTF-8 sequence and advances *p. An invalid byte is consumed and
// reported as U+FFFD.
uint32_t textUtf8Next(const char **p);

// Appends one code point to out as UTF-8, advancing *len. Nothing above the
// BMP ever gets here.
void textPutCp(char *out, size_t cap, size_t *len, uint32_t cp);

// Folds arbitrary UTF-8 into the ~0x20..0x7E plus Cyrillic alphabet the
// panel's fonts actually carry, mapping the punctuation rather than dropping
// it. Everything that comes off the network and reaches the panel goes
// through this — see the header of text.cpp for why.
void textSanitise(const char *in, char *out, size_t cap);

// The next byte of a JSON body, or -1 once it has ended. Both network screens
// walk their answer through one of these rather than buffering it whole.
typedef int (*TextByteSource)(void *ctx);

// Consumes a JSON string — the caller has already read the opening quote —
// undoing the escapes and copying at most cap-1 bytes into out. The rest is
// still consumed, so the scan stays in step however long the string is.
// Returns false only if the body ended first.
bool jsonReadString(TextByteSource src, void *ctx, char *out, size_t cap);

// --- news.cpp -------------------------------------------------------------

// World headlines, from NewsAPI narrowed to the Russian-language services of
// the BBC and DW, so the news screen speaks the same language as the rest of
// the face. See the header of news.cpp for why NewsAPI's own `country=ru` and
// its Russian sources are not what this uses.

// Six is what the panel can show at the shortest plausible headline length,
// so fetching more would only be thrown away.
#define NEWS_MAX_ITEMS 6

// Fetches the headlines. Requires a live Wi-Fi connection; the caller owns the
// radio. There is no timer behind this — it runs when, and only when, KEY
// brings the news screen up. On failure the previous headlines are kept, so
// the screen shows something stale rather than nothing.
bool newsFetch();

// Number of usable headlines, 0..NEWS_MAX_ITEMS.
int newsCount();

const FeedItem *newsItems();

// Local "HH:MM" of the last successful fetch, or "" if there has not been one.
const char *newsStamp();

// --- socks5.cpp -----------------------------------------------------------
// Only built when PAGER_ENABLED: it exists for api.telegram.org and nothing
// else on this board goes through a proxy.

#ifdef PAGER_ENABLED

// Opens a TCP connection to host:port through the SOCKS5 proxy from .env and
// hands back a blocking socket with both timeouts already set, or -1. The
// destination is resolved by the proxy, not here.
int socks5Connect(const char *host, uint16_t port, uint32_t connectTimeoutMs,
                  uint32_t ioTimeoutMs);
void socks5Close(int fd);

// For the console's status line.
const char *socks5Host();
uint16_t socks5Port();

#endif  // PAGER_ENABLED

// --- audio.cpp ------------------------------------------------------------

// Probes the ES8311 speaker codec at 0x18 and parks the amplifier off.
// rtcBegin() must have run first: that is what brings the I2C bus up.
bool audioBegin();

bool audioPresent();

// The pager's chirp: two rising notes, about a third of a second, blocking.
// Brings the amplifier and the codec up for the sound and puts both back —
// nothing is left powered between beeps. False if the codec is not there.
bool audioNotify();

// --- telegram.cpp ---------------------------------------------------------

// The pager: messages sent to the bot named by TOKEN_BOT in .env, read over
// a SOCKS5 tunnel. How many of them are on the panel is decided by how many
// lines they take, not by a slot count; ten is the most that can ever be
// there at once — ten one-line pages fill the list area in ui.cpp exactly —
// so holding more would only be thrown away.
#define PAGER_MAX_ITEMS 10

// Unlike the headlines, the pager is a *session* rather than a fetch: the
// connection is held open for as long as the screen is up and getUpdates is
// long-polled over it, so a message appears about a second after it is sent.
// The caller owns the radio and must keep Wi-Fi up for the whole session.

// Marks the session as wanting to be up. The connection itself is built on
// the first pagerPoll(), so the caller can put a frame on the panel first.
void pagerOpen();

// Drives the session. Call once per pass through loop() while the pager
// screen is up. Returns true when something the panel shows has changed.
// Only the connect and the parse block, and neither waits on Telegram having
// anything to say.
bool pagerPoll();

// Tears the connection down. The messages already read are kept.
void pagerClose();

// True while the connection is up with a poll outstanding.
bool pagerLive();

// True once per batch of newly *arrived* messages — the backlog the screen
// opens with does not count. Reading it clears it. This is what makes the
// noise.
bool pagerTakeArrival();

// Answers processed since boot, for the console's one-shot "P".
uint32_t pagerResponses();

// True while a 👀 reaction is still waiting to go out, or its answer is still
// on the way back. Every message that reaches the pager gets one; "P" waits on
// this so a one-shot read does not close the connection under them.
bool pagerReacting();

// A short Russian phrase for the footer: what the session is doing.
const char *pagerStatus();

// Number of usable pages, 0..PAGER_MAX_ITEMS, newest first.
int pagerCount();

const FeedItem *pagerItems();

// Local "HH:MM" of the last successful fetch, or "" if there has not been one.
const char *pagerStamp();

// Why the last fetch failed, as a short Russian phrase for the panel, or null
// if the last one succeeded. A pager that shows nothing has to say whether
// that is "no messages" or "could not reach the proxy" — they are different
// things to go and fix.
const char *pagerError();

// --- rtss.cpp -------------------------------------------------------------

// Live telemetry from the gaming PC: what RTSS and MSI Afterburner publish for
// their own overlay, read off rtss_api's /api/v1/metrics.bin on the LAN (see
// README_RTSS.md for the server).
//
// A reading the PC does not have is NAN, whatever sentinel the wire used for
// it: Afterburner not running, a sensor not ticked on its Monitoring tab, no
// 3D application in the foreground for the frame rate. So the panel can put
// "--" in exactly the place that is missing.
struct PcMetrics {
  float gpuTemp;    // degrees C
  float gpuLoad;    // %
  float gpuClock;   // MHz, the core clock
  float gpuPower;   // W
  float cpuTemp;    // degrees C
  float cpuLoad;    // %, all cores together
  float cpuPower;   // W
  float vramClock;  // MHz, the video memory clock
  float vramUsed;   // MB, the video memory in use
  float fps;       // frames per second, now
  bool game;        // RTSS has a 3D application to report on
};

// How many frame-rate samples the graph keeps: one per answer, so twice a
// second. Sized from ui.cpp's layout, which checks it — the graph is 380 px
// wide at 2 px a sample, so this is the last 95 seconds.
#define PC_FPS_HISTORY 190

// Like the pager, a *session* rather than a fetch: the connection to the PC
// is held for as long as the screen is up and a fresh sample is asked for
// twice a second. The caller owns the radio and keeps Wi-Fi up meanwhile.

// Marks the session as wanting to be up. The connection is started on the
// first pcPoll(), so the caller can put a frame on the panel first.
void pcOpen();

// Drives the session. Call once per pass through loop() while the PC screen
// is up. Returns true when something the panel shows has changed. Never
// blocks: the connect and the answer are both watched, not waited on.
bool pcPoll();

// Tears the connection down.
void pcClose();

// True while connected, whether or not an answer is outstanding.
bool pcLive();

// The latest sample, if there is one recent enough to show as current.
// False otherwise, and then pcError() usually says why.
bool pcMetrics(PcMetrics *out);

// The newest frame-rate samples, up to `cap` of them, oldest first, and how
// many there were. NAN is a sample with no frame rate — no game — or one the
// PC did not answer for, so the graph leaves a gap there rather than joining
// across it. Kept for the visit only: the screen opens on an empty graph.
int pcFpsHistory(float *out, int cap);

// Answers read since boot, for the console.
uint32_t pcResponses();

// A short Russian phrase for the footer: what the session is doing.
const char *pcStatus();

// Why the last attempt failed, as a short Russian phrase for the panel, or
// null if it did not. A PC that is switched off and a PC with rtss_api not
// running look the same otherwise, and are different things to go and fix.
const char *pcError();

#ifdef PC_ENABLED
// Where the samples come from, for the footer and the console.
const char *pcHost();
uint16_t pcPort();
#endif

// --- ui.cpp ---------------------------------------------------------------

// What the panel is currently showing. KEY steps through the modes this build
// carries, in this order, and wraps round to the first of them — which is not
// always the clock, since the clock is a mode like the other three.
enum UiScreen {
  UI_SCREEN_CLOCK = 0,
  UI_SCREEN_NEWS,
  UI_SCREEN_PAGER,
  UI_SCREEN_PC,
  // No mode is switched on. Not a screen anybody steps to: it is the whole
  // of an empty build, and it says so on the panel.
  UI_SCREEN_NONE,
  UI_SCREEN_COUNT,
};

// The cycle KEY walks, built in ui.cpp from the MODE_ switches above. Nothing
// else decides what a press does, so a mode that is off is simply not there.

// How many modes this build carries, 0..4.
int uiScreenCount();

// The screen the board comes up on and the news screen folds back to: the
// first mode in the cycle, which is the clock where that mode is built and
// whatever comes first where it is not. UI_SCREEN_NONE in an empty build.
UiScreen uiScreenHome();

// The screen after `cur`, wrapping. Returns `cur` itself when this build has
// only one mode — there is nowhere for KEY to go, and the footer says nothing
// about it rather than pointing at the screen you are already on.
UiScreen uiScreenNext(UiScreen cur);

// A screen's short Russian name, as the "KEY: ..." footer says it.
const char *uiScreenName(UiScreen screen);

struct UiState {
  struct tm time;
  bool timeValid;

  float volts;
  int percent;

  const char *note;  // may be null

  UiScreen screen;
  // Set for the one frame drawn to acknowledge the button before the radio
  // comes up, since the fetch behind it blocks for seconds.
  bool busy;
};

void displayBegin();

// Parks the ST7305 in its low-power mode (0x39, ~1 Hz self-refresh) or brings
// it back to the high-power one (0x38, ~32 Hz). The panel holds its image in
// both — this is not setPowerSave(), which sends 0x28 and turns the display
// off. uiDraw() lifts the panel out of LPM for the write on its own, so a
// caller only has to say which mode the screen it is showing should rest in.
void displayLowPower(bool on);

// Redraws the whole panel from the state above plus the module-owned forecast,
// headlines and pages, choosing the layout from s.screen.
void uiDraw(const UiState &s);
