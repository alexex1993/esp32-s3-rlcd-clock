// World headlines, behind the KEY button.
//
// NewsAPI, narrowed to the Russian-language services of the BBC and DW, so the
// news screen speaks the same language as the rest of the face.
//
// The obvious query is not the one used, and it is worth saying why before
// somebody "fixes" it. Checked against the live service:
//
//   * `top-headlines?country=ru` answers `"totalResults":0`. NewsAPI's own
//     documentation now lists `us` as the only country it supports.
//   * Their two Russian *sources*, `lenta` and `rbc`, still answer but are
//     frozen — `lenta` last published in 2022. The only Russian source of
//     theirs still moving is RT.
//
// What is left is `/v2/everything` filtered by domain, which is current and
// carries the outlets below. Re-verify before changing the query, not after.
//
// The answer is never held whole: it is walked once through a 256-byte window
// looking for three keys. That keeps the size of the response from mattering
// and means this file allocates nothing. The key search is deliberately dumb,
// in the same spirit as weather.cpp's: every token carries its own opening
// quote, which is what keeps `"name":` from matching inside something longer,
// and stops at the colon so that whitespace after it does not matter. It
// relies on the shape of an article — source{id,name}, author, title, ...,
// publishedAt — so "name" arrives before the title it belongs to and
// "publishedAt" after it.
//
// TLS, where weather.cpp deliberately uses plain HTTP: this request carries a
// credential. The certificate is still not checked — see newsFetch().
//
// Every headline is folded into the alphabet the panel can draw on the way
// in, by textSanitise() in text.cpp — which the pager screen shares.

#include <Arduino.h>

#include "app.h"

// NEWS_ENABLED is decided in app.h, together with the other three modes: the
// news mode has to be built in *and* the key has to be there.
#ifdef NEWS_ENABLED
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

// Two outlets that publish in Russian and are current. Anything on NewsAPI's
// source list works here; it is one string.
#define NEWS_API_DOMAINS "bbc.com,dw.com"

static FeedItem s_items[NEWS_MAX_ITEMS];
static int s_count = 0;
static char s_stamp[8] = "";

int newsCount() { return s_count; }
const FeedItem *newsItems() { return s_items; }
const char *newsStamp() { return s_stamp; }

#ifndef NEWS_ENABLED

// The news mode switched off, or built without the Wi-Fi credentials and the
// API key, so there is nothing to ask. A screen that is built but starved
// says which value is missing rather than sitting empty; one whose mode is
// off is not in the cycle at all.
bool newsFetch() { return false; }

#else

static const uint32_t HTTP_TIMEOUT_MS = 10000;
// A ceiling on a stalled transfer, not a budget for a healthy one: the answer
// is a few kilobytes and arrives at Wi-Fi speed.
static const uint32_t BODY_TIMEOUT_MS = 15000;

// Parsed into before anything is published, so a half-read answer cannot
// replace a good screenful. Static rather than automatic because the loop
// task's stack is 8 KB and this is 1.7 KB of it.
static FeedItem s_scratch[NEWS_MAX_ITEMS];

// --- reading the body -----------------------------------------------------

// In an anonymous namespace, and this is load-bearing rather than tidiness.
// telegram.cpp has a `Reader` of its own, and a struct defined at file scope
// in a .cpp still has *external* linkage — its in-class member functions are
// implicitly inline, so both translation units emit `Reader::next()` under
// the same mangled name and the linker keeps whichever it saw first. The
// result is telegram.cpp calling this one, dereferencing a `stream` pointer
// that was never set, and a LoadProhibited panic. Anonymous namespace makes
// each type local to its file, which is what was meant all along.
namespace {

// A pull reader over the HTTP body, so nothing is ever buffered whole.
struct Reader {
  WiFiClient *stream = nullptr;
  uint8_t buf[256];
  int len = 0;
  int pos = 0;
  uint32_t deadline = 0;

  // The next body byte, or -1 once the answer is finished or has stalled.
  int next() {
    if (pos < len) return buf[pos++];

    for (;;) {
      if ((int32_t)(millis() - deadline) >= 0) return -1;

      const int avail = stream->available();
      if (avail > 0) {
        const int got =
            stream->read(buf, (size_t)min(avail, (int)sizeof(buf)));
        if (got > 0) {
          len = got;
          pos = 0;
          return buf[pos++];
        }
      } else if (!stream->connected()) {
        return -1;
      }
      delay(2);
    }
  }
};

}  // namespace

// jsonReadString() in text.cpp pulls through this.
static int readerByte(void *ctx) { return ((Reader *)ctx)->next(); }

// Steps over any whitespace between a colon and its value and hands back the
// first byte of the value itself. NewsAPI sends compact JSON today; this is
// what stops a reformatting on their side from emptying the screen.
static int skipSpace(Reader &r) {
  for (;;) {
    const int c = r.next();
    if (c < 0) return -1;
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return c;
  }
}

// A string value, opening quote and all. A value that is not a string — null,
// which is what NewsAPI puts in an absent author — reads as empty rather than
// as a failure, and the one byte consumed of it matches no token.
static bool readValueString(Reader &r, char *out, size_t cap) {
  const int c = skipSpace(r);
  if (c < 0) return false;
  if (c != '"') {
    out[0] = '\0';
    return true;
  }
  return jsonReadString(readerByte, &r, out, cap);
}

// Feeds one byte to a token matcher. Returns true when the token completes.
// On a mismatch the byte is re-tested against the first character, which
// matters because every token here starts with the quote that also ends the
// value in front of it.
static bool matchAdvance(const char *tok, int *idx, int c) {
  if (c == (int)(uint8_t)tok[*idx]) {
    (*idx)++;
  } else {
    *idx = (c == (int)(uint8_t)tok[0]) ? 1 : 0;
  }
  if (*idx > 0 && tok[*idx] == '\0') {
    *idx = 0;
    return true;
  }
  return false;
}

// "2026-09-09T09:16:12Z", UTC. Only a clock face is ever shown, so the date
// rolling over does not matter and the offset lands on the hour alone.
static void setIsoClock(FeedItem *it, const char *iso) {
  int y, mo, d, hh, mm;
  if (sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mm) != 5) return;
  it->hour = (uint8_t)(((hh + TZ_OFFSET_SECONDS / 3600) % 24 + 24) % 24);
  it->minute = (uint8_t)mm;
  it->timed = true;
}

static bool parseNewsApi(Reader &r, FeedItem *out, int max, int *count) {
  int iName = 0, iTitle = 0, iAt = 0;
  char raw[320];
  char source[FEED_FROM_CAP] = "";
  *count = 0;

  for (;;) {
    const int c = r.next();
    if (c < 0) break;

    // All three matchers see every byte; they share a leading quote and are
    // otherwise independent.
    const bool nameHit = matchAdvance("\"name\":", &iName, c);
    const bool titleHit = matchAdvance("\"title\":", &iTitle, c);
    const bool atHit = matchAdvance("\"publishedAt\":", &iAt, c);

    if (nameHit) {
      // source{id,name} is the only "name" in an article, and it arrives
      // ahead of the title it belongs to.
      if (!readValueString(r, raw, sizeof(raw))) break;
      textSanitise(raw, source, sizeof(source));
      // NewsAPI labels DW's Russian service "DW (English)". The parenthetical
      // is wrong as well as too wide for the row.
      char *paren = strstr(source, " (");
      if (paren != nullptr) *paren = '\0';
      iName = iTitle = iAt = 0;
      continue;
    }

    if (titleHit) {
      if (!readValueString(r, raw, sizeof(raw))) break;
      iName = iTitle = iAt = 0;
      // The count is only bumped here, while the return below waits for the
      // matching publishedAt, so an article missing one must not slide the
      // next title past the end of the array.
      if (*count >= max) return true;

      FeedItem &it = out[*count];
      memset(&it, 0, sizeof(it));
      textSanitise(raw, it.text, sizeof(it.text));
      if (it.text[0] == '\0') continue;
      snprintf(it.from, sizeof(it.from), "%s", source);
      (*count)++;
      continue;
    }

    if (atHit) {
      if (!readValueString(r, raw, sizeof(raw))) break;
      iName = iTitle = iAt = 0;
      if (*count > 0) setIsoClock(&out[*count - 1], raw);
      if (*count >= max) return true;
    }
  }
  return *count > 0;
}

// --- the fetch ------------------------------------------------------------

bool newsFetch() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("news: no Wi-Fi, keeping the previous headlines");
    return false;
  }

  WiFiClientSecure client;
  // The certificate is not checked. The answer is public news text with
  // nothing authenticated in it, and carrying a root bundle would cost more
  // flash and heap than the check is worth on a desk clock. TLS is here for
  // the other direction: the key rides inside the session instead of in the
  // clear, which is why this file does not follow weather.cpp's deliberate
  // plain HTTP.
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  const String url =
      String("https://newsapi.org/v2/everything?domains=") + NEWS_API_DOMAINS +
      "&language=ru&sortBy=publishedAt&pageSize=" + String(NEWS_MAX_ITEMS);

  if (!http.begin(client, url)) {
    Serial.println("news: could not open the connection");
    return false;
  }
  // HTTPClient cannot undo an encoding, and the answer is walked byte by byte.
  http.addHeader("Accept-Encoding", "identity");
  // In the header rather than the query string, so the key stays out of any
  // proxy or server log that records URLs.
  http.addHeader("X-Api-Key", NEWS_API_KEY);

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    // 429 here is NewsAPI's daily cap of 100 requests; one press of KEY into
    // the news screen is one request, so that is 100 visits in a day.
    Serial.printf("news: HTTP %d\n", status);
    http.end();
    return false;
  }

  Reader r;
  r.stream = http.getStreamPtr();
  r.deadline = millis() + BODY_TIMEOUT_MS;

  int n = 0;
  const bool ok = parseNewsApi(r, s_scratch, NEWS_MAX_ITEMS, &n);
  http.end();

  if (!ok || n <= 0) {
    Serial.println("news: unparsable answer, keeping the previous headlines");
    return false;
  }

  memcpy(s_items, s_scratch, sizeof(s_scratch));
  s_count = n;

  struct tm now;
  const time_t t = time(nullptr);
  localtime_r(&t, &now);
  snprintf(s_stamp, sizeof(s_stamp), "%02d:%02d", now.tm_hour, now.tm_min);

  Serial.printf("news: %d headlines, top \"%s\"\n", n, s_items[0].text);
  return true;
}

#endif  // NEWS_ENABLED
