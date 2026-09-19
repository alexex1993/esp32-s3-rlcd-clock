// The 12-hour forecast for Brateevo, from Open-Meteo.
//
// Open-Meteo needs no API key and no account, which is the whole reason it is
// used here: there is nothing to put in .env beyond the Wi-Fi credentials.
//
//   http://api.open-meteo.com/v1/forecast
//       ?latitude=..&longitude=..
//       &current=temperature_2m,relative_humidity_2m,surface_pressure
//       &hourly=temperature_2m,weather_code,precipitation_probability,is_day
//       &daily=sunset
//       &forecast_hours=12&timezone=Europe/Moscow
//
// The one request carries all three things the face needs: the conditions
// right now (which replaced the on-board SHTC3 on the panel), the twelve-hour
// strip, and today's sunset. `daily` is left at its default seven days rather
// than pinned with forecast_days=1, which would also clip the hourly range and
// leave the strip short of twelve columns late in the evening; the extra six
// sunsets cost about a hundred bytes and are ignored.
//
// forecast_hours=12 makes the server truncate the series to the twelve hours
// starting with the one in progress, so the first column is always "now" and
// the firmware never has to line the array up against its own clock.
//
// Two decisions worth stating:
//
//   * Plain HTTP, not HTTPS. The answer is a public weather forecast with no
//     credentials in the request, and skipping TLS saves the ~40 KB of heap
//     and the second or so of handshake that the radio would otherwise be up
//     for. Nothing here is worth authenticating.
//
//   * No JSON library. The response is a handful of flat objects of parallel
//     arrays and is picked apart below with strstr()/strtof(), which keeps
//     lib_deps at a single entry (U8g2). The parsers are deliberately dumb:
//     they scan for `"<key>":[` and read numbers until the closing bracket, or
//     for `"<key>":` inside one named object and read a single number.
//
// The same row on the face also carries the air quality and the UV index,
// which Open-Meteo serves from a different host, so they cannot ride in the
// request above:
//
//   http://air-quality-api.open-meteo.com/v1/air-quality
//       ?latitude=..&longitude=..&current=european_aqi,uv_index
//
// Same terms — no key, plain HTTP, one flat `current` object — but its own
// call, airQualityFetch(), so one service being down never costs the face the
// other's readings.

#include <Arduino.h>
#include <math.h>

#include "app.h"

// WEATHER_ENABLED, from app.h: the credentials, and the clock mode, which is
// the only screen a forecast is ever drawn on.
#ifdef WEATHER_ENABLED
#include <HTTPClient.h>
#include <WiFi.h>
#endif

static WeatherHour s_hours[WEATHER_HOURS];
static int s_count = 0;
static char s_stamp[8] = "";
static WeatherNow s_now = {NAN, NAN, NAN, NAN, NAN};
static bool s_nowValid = false;
static char s_sunset[8] = "";

bool weatherValid() { return s_count > 0; }
int weatherCount() { return s_count; }
const WeatherHour *weatherHours() { return s_hours; }
const char *weatherStamp() { return s_stamp; }
const char *weatherSunset() { return s_sunset; }

bool weatherNow(WeatherNow *out) {
  if (!s_nowValid) return false;
  if (out != nullptr) *out = s_now;
  return true;
}

#ifndef WEATHER_ENABLED

// No credentials compiled in, or no clock face to draw a forecast on, so
// there is nothing to ask for. The panel leaves the strip empty and says so.
bool weatherFetch() { return false; }
bool airQualityFetch() { return false; }

#else

static const uint32_t HTTP_TIMEOUT_MS = 8000;

// One GET, the whole body into *body. False, having said why, on anything but
// a 200. `what` names the caller in the log.
static bool httpGet(const char *what, const String &url, String *body) {
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(url)) {
    Serial.printf("%s: could not open the connection\n", what);
    return false;
  }

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("%s: HTTP %d\n", what, status);
    http.end();
    return false;
  }

  *body = http.getString();
  http.end();
  return true;
}

// `"hourly"` is the only object in the response whose members are arrays, so
// `"<key>":[` cannot collide with the same key inside "hourly_units", where it
// is followed by a string. Returns the first character after the bracket.
static const char *arrayStart(const char *json, const char *key) {
  char pattern[40];
  snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
  const char *p = strstr(json, pattern);
  return (p != nullptr) ? p + strlen(pattern) : nullptr;
}

// ["2026-09-10T09:00", ...] -> the hour of each entry. Only the two digits
// after the 'T' are of interest: the server already applied the time zone.
static int parseHours(const char *p, uint8_t *out, int max) {
  int n = 0;
  while (p != nullptr && *p != '\0' && *p != ']' && n < max) {
    if (*p != '"') {
      p++;
      continue;
    }
    const char *quote = strchr(p + 1, '"');
    const char *tee = strchr(p + 1, 'T');
    if (quote == nullptr || tee == nullptr || tee + 2 >= quote) break;
    out[n++] = (uint8_t)((tee[1] - '0') * 10 + (tee[2] - '0'));
    p = quote + 1;
  }
  return n;
}

// `"current":{` -> the first character inside the braces. `"current_units":{`
// does not match: the pattern carries the closing quote.
static const char *objectStart(const char *json, const char *key) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":{", key);
  const char *p = strstr(json, pattern);
  return (p != nullptr) ? p + strlen(pattern) : nullptr;
}

// One number out of a flat object. The search is bounded by the object's own
// closing brace — none of the objects read here nest — so a key missing from
// `current` reads as absent rather than picking up the same name out of
// `hourly_units` further down the response.
static float scalarField(const char *obj, const char *key) {
  if (obj == nullptr) return NAN;
  const char *end = strchr(obj, '}');
  char pattern[40];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char *p = strstr(obj, pattern);
  if (p == nullptr || (end != nullptr && p > end)) return NAN;
  p += strlen(pattern);
  char *stop = nullptr;
  float v = strtof(p, &stop);
  return (stop == p) ? NAN : v;  // a string value ("°C") is not a reading
}

// ["2026-09-10T19:42", ...] -> "19:42" of the first entry, which is today.
static void parseFirstClock(const char *p, char *out, size_t outSize) {
  if (p == nullptr) return;
  const char *quote = strchr(p, '"');
  if (quote == nullptr) return;
  const char *tee = strchr(quote + 1, 'T');
  if (tee == nullptr || strlen(tee) < 6) return;
  snprintf(out, outSize, "%.5s", tee + 1);
}

// [16.2, 18.7, null, ...] -> floats, with JSON null becoming NAN.
static int parseNumbers(const char *p, float *out, int max) {
  int n = 0;
  while (p != nullptr && *p != '\0' && *p != ']' && n < max) {
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
      char *end = nullptr;
      out[n++] = strtof(p, &end);
      if (end == p) break;  // not a number after all, do not spin
      p = end;
    } else if (*p == 'n') {  // null
      out[n++] = NAN;
      p += 4;
    } else {
      p++;
    }
  }
  return n;
}

bool weatherFetch() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("weather: no Wi-Fi, keeping the previous forecast");
    return false;
  }

  const String url =
      String("http://api.open-meteo.com/v1/forecast?latitude=") + WEATHER_LAT +
      "&longitude=" + WEATHER_LON +
      "&current=temperature_2m,relative_humidity_2m,surface_pressure"
      "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
      "&daily=sunset"
      "&forecast_hours=" + String(WEATHER_HOURS) + "&timezone=" + WEATHER_TZ;

  String body;
  if (!httpGet("weather", url, &body)) return false;

  const char *json = body.c_str();

  uint8_t hours[WEATHER_HOURS];
  float temp[WEATHER_HOURS], code[WEATHER_HOURS];
  float pop[WEATHER_HOURS], day[WEATHER_HOURS];

  const int nHour = parseHours(arrayStart(json, "time"), hours, WEATHER_HOURS);
  const int nTemp =
      parseNumbers(arrayStart(json, "temperature_2m"), temp, WEATHER_HOURS);
  const int nCode =
      parseNumbers(arrayStart(json, "weather_code"), code, WEATHER_HOURS);
  const int nPop = parseNumbers(arrayStart(json, "precipitation_probability"),
                                pop, WEATHER_HOURS);
  const int nDay = parseNumbers(arrayStart(json, "is_day"), day, WEATHER_HOURS);

  // The current conditions and today's sunset are read before the strip is
  // validated below: they come out of different objects and one being missing
  // is no reason to drop the other.
  const char *current = objectStart(json, "current");
  if (current != nullptr) {
    const float t = scalarField(current, "temperature_2m");
    const float rh = scalarField(current, "relative_humidity_2m");
    const float hpa = scalarField(current, "surface_pressure");
    // A block with no temperature in it is not worth overwriting the last good
    // reading with; a missing humidity or pressure only blanks its own field.
    if (!isnan(t)) {
      s_now.temp = t;
      s_now.humidity = rh;
      s_now.pressure = hpa;
      s_nowValid = true;
    }
  }
  // "sunset":[ is in "daily"; "daily_units" has it as a plain string.
  parseFirstClock(arrayStart(json, "sunset"), s_sunset, sizeof(s_sunset));

  // The hour labels and the codes are what the strip is built around; a
  // response missing either is not worth overwriting a good forecast with.
  int n = nHour;
  if (nCode < n) n = nCode;
  if (n <= 0) {
    Serial.printf("weather: unparsable answer (%u bytes)\n",
                  (unsigned)body.length());
    return false;
  }

  for (int i = 0; i < n; i++) {
    s_hours[i].hour = hours[i];
    s_hours[i].temp = (i < nTemp) ? temp[i] : NAN;
    // A null weather_code would be NAN here, and casting that to an integer is
    // undefined. 3 (overcast) is the honest stand-in for "the server did not
    // say", and is also what an unmapped code falls back to in ui.cpp.
    s_hours[i].code = isnan(code[i]) ? 3 : (uint8_t)code[i];
    // A missing probability reads as 0 rather than as rain.
    s_hours[i].pop = (i < nPop && !isnan(pop[i])) ? (uint8_t)pop[i] : 0;
    // Unknown daylight defaults to day, so an unlit sun beats a wrong moon.
    s_hours[i].day = (i < nDay) ? (day[i] != 0.0f) : true;
  }
  s_count = n;

  struct tm now;
  time_t t = time(nullptr);
  localtime_r(&t, &now);
  snprintf(s_stamp, sizeof(s_stamp), "%02d:%02d", now.tm_hour, now.tm_min);

  Serial.printf("weather: %d h from %02d:00, now %.1f C / %.0f%% RH / %.0f hPa,"
                " code %u, sunset %s\n",
                n, s_hours[0].hour, s_now.temp, s_now.humidity, s_now.pressure,
                s_hours[0].code, s_sunset[0] != '\0' ? s_sunset : "?");
  return true;
}

bool airQualityFetch() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("air: no Wi-Fi, keeping the previous reading");
    return false;
  }

  const String url =
      String("http://air-quality-api.open-meteo.com/v1/air-quality?latitude=") +
      WEATHER_LAT + "&longitude=" + WEATHER_LON +
      "&current=european_aqi,uv_index";

  String body;
  if (!httpGet("air", url, &body)) return false;

  // `"current":{` again; `"current_units":{` carries the same two keys as
  // strings, which scalarField() reads as absent.
  const char *current = objectStart(body.c_str(), "current");
  const float aqi = scalarField(current, "european_aqi");
  const float uv = scalarField(current, "uv_index");
  // An answer with neither is not worth overwriting the last good one with; a
  // null in one of them only blanks that one.
  if (isnan(aqi) && isnan(uv)) {
    Serial.printf("air: unparsable answer (%u bytes)\n",
                  (unsigned)body.length());
    return false;
  }
  s_now.aqi = aqi;
  s_now.uv = uv;

  Serial.printf("air: european AQI %.0f, UV index %.1f\n", aqi, uv);
  return true;
}

#endif  // WEATHER_ENABLED
