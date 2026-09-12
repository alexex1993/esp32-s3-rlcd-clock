// The PC screen, behind the fourth press of KEY.
//
// Live numbers from the gaming PC across the room: what RivaTuner Statistics
// Server and MSI Afterburner already publish for their own on-screen display,
// republished on the LAN by rtss_api (README_RTSS.md describes the server).
// The screen shows the GPU's temperature, load, core clock and power; the
// CPU's temperature, load across all cores and power; the video memory in use
// and its clock; and the frame rate, with a graph of the last 95 seconds of it, whose samples
// this file keeps.
//
// Decisions worth stating before somebody "simplifies" one of them:
//
//   * The binary endpoint, /api/v1/metrics.bin, not the JSON one. It is a
//     fixed 72-byte little-endian struct in a fixed order, so a sample is a
//     memcpy and a magic number to check: nothing to scan, nothing to
//     allocate. The ESP32-S3 is little-endian and its float is IEEE 754, like
//     the Go server's, so the bytes go straight in.
//
//   * Exactly 72 bytes, nothing shorter or longer. The layout has changed
//     without its magic number changing: the 1% and 0.1% lows came out of the
//     middle, video memory in use turned into the video memory clock at 68
//     bytes, and came back on the end at 72. The length is the only thing that
//     tells those apart, so an answer of any other length is refused rather
//     than read into the wrong fields.
//
//   * A session, like the pager, not a fetch like the headlines. For as long
//     as the screen is up, the radio stays up and one HTTP/1.1 keep-alive
//     connection is held to the PC, with a request on it every POLL_MS.
//     Reconnecting per sample would be a TCP handshake twice a second for
//     nothing.
//
//   * Nothing here blocks. The connect is non-blocking and watched from loop()
//     with a zero-timeout select(), and the answer is collected the same way.
//     A PC that is switched off costs a reason on the panel, never a press of
//     KEY that goes unnoticed.
//
//   * Twice a second, not the 4-6 Hz rtss_api's README suggests, graph or no
//     graph. At 2 px a sample the graph already spans 95 seconds of the
//     panel's width, which is the span a stutter or a loading screen shows up
//     in. Five a second would cut that to 38 seconds and repaint the whole
//     panel five times a second to do it, and the numbers under the graph come
//     from Afterburner, which refreshes its sensors once a second by default.
//
//   * A sample goes stale after STALE_MS. Numbers from a PC that has stopped
//     answering are not shown as if they were current: the screen says why
//     instead.
//
//   * Plain HTTP. It never leaves the LAN, carries no credentials either way,
//     and the numbers are not worth a TLS session's heap.
//
// rtss_api is a Go net/http server, which frames a 72-byte answer with
// Content-Length. That is its choice to make rather than a promise, so a
// chunked answer and one that ends when the connection does are read too.

#include <Arduino.h>
#include <math.h>

#include "app.h"

#ifndef PC_ENABLED

// Without the Wi-Fi credentials or the PC's address there is nothing to ask.
// The screen says which of the two is missing.
void pcOpen() {}
bool pcPoll() { return false; }
void pcClose() {}
bool pcLive() { return false; }
bool pcMetrics(PcMetrics *out) {
  (void)out;
  return false;
}
int pcFpsHistory(float *out, int cap) {
  (void)out;
  (void)cap;
  return 0;
}
uint32_t pcResponses() { return 0; }
const char *pcStatus() { return ""; }
const char *pcError() { return nullptr; }

#else

#include <WiFi.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <stdarg.h>
#include <strings.h>

static const char kPath[] = "/api/v1/metrics.bin";

// One sample this often. See the header for why not faster.
static const uint32_t POLL_MS = 500;
// A connect across the LAN takes milliseconds; a PC that has not answered in
// this long is not going to.
static const uint32_t CONNECT_TIMEOUT_MS = 3000;
// rtss_api answers out of memory it already has mapped, in microseconds, so
// this is all Wi-Fi.
static const uint32_t ANSWER_TIMEOUT_MS = 2000;
// After a failure, before connecting again. Short, because a failed connect on
// the LAN is cheap to repeat, and what is usually being waited for is somebody
// switching the PC on.
static const uint32_t RETRY_MS = 2000;
// Older than this, a sample is not shown as a reading: several missed polls,
// not one.
static const uint32_t STALE_MS = 3000;

// Big enough for the head Go writes (well under 200 bytes) and the body, with
// room for a proxy or a newer server to add a few headers.
static const size_t ANSWER_CAP = 768;
// A 72-byte blob is expected. Anything that does not fit in this is not one,
// and a short error page from something else still does.
static const size_t BODY_CAP = 128;

// What the panel is told, as named strings so that "the same failure again"
// is a pointer comparison and is logged once rather than every RETRY_MS.
static const char kErrWifi[] = "нет Wi-Fi";
static const char kErrResolve[] = "адрес ПК не найден";
static const char kErrRefused[] = "rtss_api не запущен";
static const char kErrUnreachable[] = "ПК не отвечает";
static const char kErrSilent[] = "ПК не ответил";
static const char kErrDropped[] = "ПК оборвал связь";
static const char kErrStatus[] = "rtss_api ответил ошибкой";
static const char kErrForeign[] = "это не rtss_api";

static const char kStateConnecting[] = "подключаюсь...";
static const char kStateReconnecting[] = "переподключаюсь...";
static const char kStateLive[] = "на связи";

namespace {

// The wire format, field for field as rtss_api's README lays it out: 72 bytes,
// little-endian, no padding. A float the PC does not have arrives as -1.0 and
// an integer as 0.
#pragma pack(push, 1)
struct Wire {
  uint32_t magic;
  uint32_t seq;
  float gpuTemp;
  float gpuLoad;
  float gpuClock;
  float gpuPower;
  float cpuTemp;
  float cpuLoad;
  float cpuPower;
  uint32_t memUsedMb;
  uint32_t memTotalMb;
  uint32_t memFreqMhz;
  float fpsCurrent;
  float fpsAvg;
  uint32_t pid;
  float vramClockMhz;
  uint32_t vramTotalMb;
  uint32_t vramUsedMb;
};
#pragma pack(pop)

// One HTTP answer, taken apart.
struct Answer {
  int status;
  bool keepAlive;
  uint8_t body[BODY_CAP];
  size_t bodyLen;
};

enum Parsed {
  PARSE_MORE = 0,  // incomplete so far; wait for more bytes
  PARSE_DONE,      // a whole answer is in hand
  PARSE_BAD,       // not an HTTP answer this can read
};

enum PcState {
  PC_OFF = 0,     // the screen is not up
  PC_CONNECT,     // open a socket and start the handshake
  PC_CONNECTING,  // the handshake is under way; ask the socket, do not block
  PC_IDLE,        // connected, and the next request is not due yet
  PC_WAIT,        // a request is out; collect the answer as it arrives
  PC_RETRY,       // something failed; wait before connecting again
};

}  // namespace

static_assert(sizeof(Wire) == 72, "rtss_api's metrics.bin is 72 bytes");
static_assert(offsetof(Wire, pid) == 56 && offsetof(Wire, vramClockMhz) == 60 &&
                  offsetof(Wire, vramUsedMb) == 68,
              "the fields after fps_avg, where the layout keeps changing");
static const uint32_t WIRE_MAGIC = 0x42415452u;  // "RTAB" as it sits in memory

static PcState s_state = PC_OFF;
static int s_fd = -1;
static uint32_t s_deadline = 0;  // for the step under way: connect or answer
static uint32_t s_nextAt = 0;    // when the next request is due
static uint32_t s_retryAt = 0;
static uint32_t s_responses = 0;
static uint32_t s_served = 0;  // answers on the connection that is up now
static bool s_announced = false;
static const char *s_error = nullptr;

static PcMetrics s_metrics;
static bool s_have = false;
static uint32_t s_sampleMs = 0;
static uint32_t s_samples = 0;

static char s_buf[ANSWER_CAP];
static size_t s_len = 0;

// The graph's samples: a ring, s_fpsHead being where the next one goes.
static float s_fps[PC_FPS_HISTORY];
static int s_fpsHead = 0;
static int s_fpsCount = 0;

// What pcPoll() last reported to the panel, so it can say when that changed.
static const char *s_shownStatus = nullptr;
static const char *s_shownError = nullptr;
static bool s_shownFresh = false;
static uint32_t s_shownSamples = 0;

static bool due(uint32_t at) { return (int32_t)(millis() - at) >= 0; }

static bool fresh() { return s_have && millis() - s_sampleMs < STALE_MS; }

uint32_t pcResponses() { return s_responses; }
const char *pcError() { return s_error; }
const char *pcHost() { return RTSS_HOST; }
uint16_t pcPort() { return (uint16_t)RTSS_PORT; }
bool pcLive() { return s_state == PC_IDLE || s_state == PC_WAIT; }

// Chosen so the footer does not flicker. Once something has failed, every
// attempt after it is a reconnect until a sample says otherwise, rather than
// flipping between two phrases every few seconds while the PC is off. And a
// connection being replaced under a live sample — a server that closes after
// every answer does that twice a second — is still "на связи".
const char *pcStatus() {
  if (s_state == PC_OFF) return "";
  if (s_error != nullptr) return kStateReconnecting;
  if (s_state == PC_IDLE || s_state == PC_WAIT || fresh()) return kStateLive;
  return kStateConnecting;
}

bool pcMetrics(PcMetrics *out) {
  if (!fresh()) return false;
  if (out != nullptr) *out = s_metrics;
  return true;
}

static void pushFps(float v) {
  s_fps[s_fpsHead] = v;
  s_fpsHead = (s_fpsHead + 1) % PC_FPS_HISTORY;
  if (s_fpsCount < PC_FPS_HISTORY) s_fpsCount++;
}

int pcFpsHistory(float *out, int cap) {
  const int n = s_fpsCount < cap ? s_fpsCount : cap;
  for (int i = 0; i < n; i++) {
    out[i] = s_fps[(s_fpsHead - n + i + PC_FPS_HISTORY) % PC_FPS_HISTORY];
  }
  return n;
}

// --- HTTP framing -----------------------------------------------------------

static const char *findCrlf(const char *p, const char *end) {
  for (; p + 1 < end; p++) {
    if (p[0] == '\r' && p[1] == '\n') return p;
  }
  return nullptr;
}

static bool nameIs(const char *name, const char *colon, const char *want) {
  const size_t len = (size_t)(colon - name);
  return len == strlen(want) && strncasecmp(name, want, len) == 0;
}

static bool containsCI(const char *p, const char *end, const char *needle) {
  const size_t n = strlen(needle);
  for (; p + n <= end; p++) {
    if (strncasecmp(p, needle, n) == 0) return true;
  }
  return false;
}

// Takes apart what has arrived so far. `closed` is whether the peer has shut
// its side, which is what ends an answer with neither a length nor chunks and
// what turns "incomplete" into "never going to be complete".
//
// buf must be NUL-terminated at buf[len]: the status line is read with sscanf.
static Parsed parseAnswer(const char *buf, size_t len, bool closed, Answer *a) {
  const char *const end = buf + len;
  a->status = 0;
  a->keepAlive = false;
  a->bodyLen = 0;

  const char *headEnd = nullptr;
  for (const char *p = buf; p + 3 < end; p++) {
    if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
      headEnd = p;
      break;
    }
  }
  if (headEnd == nullptr) return closed ? PARSE_BAD : PARSE_MORE;

  int major = 0, minor = 0;
  if (sscanf(buf, "HTTP/%d.%d %d", &major, &minor, &a->status) != 3) {
    return PARSE_BAD;
  }
  // HTTP/1.1 keeps the connection unless told otherwise; 1.0 closes it.
  a->keepAlive = major > 1 || (major == 1 && minor >= 1);

  long contentLength = -1;
  bool chunked = false;
  const char *const headStop = headEnd + 2;  // past the last header's CRLF
  const char *line = findCrlf(buf, headStop) + 2;
  while (line < headStop) {
    const char *const eol = findCrlf(line, headStop);
    if (eol == nullptr) break;
    const char *const colon =
        (const char *)memchr(line, ':', (size_t)(eol - line));
    if (colon != nullptr) {
      const char *value = colon + 1;
      while (value < eol && *value == ' ') value++;
      if (nameIs(line, colon, "Content-Length")) {
        contentLength = strtol(value, nullptr, 10);
      } else if (nameIs(line, colon, "Transfer-Encoding")) {
        chunked = containsCI(value, eol, "chunked");
      } else if (nameIs(line, colon, "Connection")) {
        if (containsCI(value, eol, "close")) {
          a->keepAlive = false;
        } else if (containsCI(value, eol, "keep-alive")) {
          a->keepAlive = true;
        }
      }
    }
    line = eol + 2;
  }

  const char *p = headEnd + 4;

  if (chunked) {
    for (;;) {
      const char *const eol = findCrlf(p, end);
      if (eol == nullptr) return closed ? PARSE_BAD : PARSE_MORE;
      char *stop = nullptr;
      const unsigned long size = strtoul(p, &stop, 16);
      if (stop == p || stop > eol) return PARSE_BAD;
      const char *data = eol + 2;
      if (size == 0) {
        // The last chunk; the answer ends at the blank line after whatever
        // trailers there are, which is normally none.
        for (;;) {
          const char *const trailer = findCrlf(data, end);
          if (trailer == nullptr) return closed ? PARSE_BAD : PARSE_MORE;
          if (trailer == data) return PARSE_DONE;
          data = trailer + 2;
        }
      }
      // Checked before any arithmetic on it: a size near ULONG_MAX would wrap.
      if (size > BODY_CAP - a->bodyLen) return PARSE_BAD;
      if ((size_t)(end - data) < size + 2) {
        return closed ? PARSE_BAD : PARSE_MORE;
      }
      memcpy(a->body + a->bodyLen, data, size);
      a->bodyLen += size;
      p = data + size + 2;
    }
  }

  if (contentLength >= 0) {
    if ((size_t)contentLength > BODY_CAP) return PARSE_BAD;
    if ((size_t)(end - p) < (size_t)contentLength) {
      return closed ? PARSE_BAD : PARSE_MORE;
    }
    memcpy(a->body, p, (size_t)contentLength);
    a->bodyLen = (size_t)contentLength;
    return PARSE_DONE;
  }

  // Neither a length nor chunks: the body is everything until the peer closes.
  a->keepAlive = false;
  if ((size_t)(end - p) > BODY_CAP) return PARSE_BAD;
  if (!closed) return PARSE_MORE;
  a->bodyLen = (size_t)(end - p);
  memcpy(a->body, p, a->bodyLen);
  return PARSE_DONE;
}

// --- the connection ---------------------------------------------------------

static void closeSocket() {
  if (s_fd >= 0) {
    close(s_fd);
    s_fd = -1;
  }
  s_len = 0;
  s_buf[0] = '\0';
}

// Drops the connection and says why: `why` for the panel, the rest for the
// console. The same failure repeating every RETRY_MS while the PC is off is
// one line in the log, not one every two seconds.
__attribute__((format(printf, 2, 3)))
static void fail(const char *why, const char *fmt, ...) {
  if (why != s_error) {
    char detail[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    Serial.printf("pc: %s\n", detail);
  }
  s_error = why;
  s_announced = false;
  closeSocket();
  s_state = PC_RETRY;
  s_retryAt = millis() + RETRY_MS;
}

// A refusal means the PC is up and nothing listens on the port, so the server
// is not running. Anything else, a timeout above all, means nothing answered
// at that address: the PC is off, asleep, or its firewall drops the port.
// They are different things to go and fix. lwIP reports a refused connect as
// ECONNRESET, a BSD stack as ECONNREFUSED.
static void connectFailed(int err) {
  if (err == ECONNREFUSED || err == ECONNRESET) {
    fail(kErrRefused, "%s:%u refused the connection, is rtss_api running?",
         RTSS_HOST, (unsigned)RTSS_PORT);
  } else {
    fail(kErrUnreachable, "connect to %s:%u failed, errno %d", RTSS_HOST,
         (unsigned)RTSS_PORT, err);
  }
}

// Starts a non-blocking connect; PC_CONNECTING watches it finish.
static void startConnect() {
  char port[6];
  snprintf(port, sizeof(port), "%u", (unsigned)RTSS_PORT);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  // Immediate for an address, which is what RTSS_HOST normally is; a name
  // costs one DNS lookup per connection, and connections are kept.
  struct addrinfo *res = nullptr;
  const int rc = getaddrinfo(RTSS_HOST, port, &hints, &res);
  if (rc != 0 || res == nullptr) {
    fail(kErrResolve, "cannot resolve %s (getaddrinfo %d)", RTSS_HOST, rc);
    return;
  }

  s_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s_fd < 0) {
    const int err = errno;
    freeaddrinfo(res);
    fail(kErrUnreachable, "socket() failed, errno %d", err);
    return;
  }

  const int flags = fcntl(s_fd, F_GETFL, 0);
  fcntl(s_fd, F_SETFL, flags | O_NONBLOCK);
  // The requests are tiny and one at a time; there is nothing for Nagle to
  // coalesce, only a delayed ACK to wait on.
  const int one = 1;
  setsockopt(s_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  const int cr = connect(s_fd, res->ai_addr, res->ai_addrlen);
  const int err = errno;
  freeaddrinfo(res);
  if (cr != 0 && err != EINPROGRESS) {
    connectFailed(err);
    return;
  }

  s_served = 0;
  s_deadline = millis() + CONNECT_TIMEOUT_MS;
  s_state = PC_CONNECTING;
}

// 1 once the handshake has finished, 0 while it is still going, -1 when it
// failed, with the reason in *err.
static int connectProgress(int *err) {
  fd_set wr, ex;
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  FD_SET(s_fd, &wr);
  FD_SET(s_fd, &ex);
  struct timeval tv = {.tv_sec = 0, .tv_usec = 0};
  const int sel = select(s_fd + 1, nullptr, &wr, &ex, &tv);
  if (sel == 0) return 0;
  if (sel < 0) {
    *err = errno;
    return -1;
  }
  int soError = 0;
  socklen_t len = sizeof(soError);
  getsockopt(s_fd, SOL_SOCKET, SO_ERROR, &soError, &len);
  if (soError != 0) {
    *err = soError;
    return -1;
  }
  return 1;
}

static bool sendRequest() {
  char req[384];
  const int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\n"
                         "Host: %s:%u\r\n"
                         "User-Agent: rlcd-esp32s3-pc\r\n"
                         "Connection: keep-alive\r\n"
                         "\r\n",
                         kPath, RTSS_HOST, (unsigned)RTSS_PORT);
  if (n <= 0 || n >= (int)sizeof(req)) return false;
  // One small request on a connection with nothing else queued: the send
  // buffer takes it whole or the connection is gone, so a short send is a
  // failure rather than something to come back and finish.
  return send(s_fd, req, (size_t)n, 0) == n;
}

// Whatever has arrived, off the socket without waiting for more. 1 when the
// peer has closed its side, 0 while it is open, -1 when the socket failed.
static int pump() {
  while (s_len < sizeof(s_buf) - 1) {
    const int n = recv(s_fd, s_buf + s_len, sizeof(s_buf) - 1 - s_len, 0);
    if (n > 0) {
      s_len += (size_t)n;
      s_buf[s_len] = '\0';
      continue;
    }
    if (n == 0) return 1;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }
  return 0;  // full; the parser decides what that means
}

// Anything the PC does not have is NAN here, whatever the wire used for it,
// so the panel can put "--" in exactly that one place.
static float reading(float v) { return (isnan(v) || v < 0.0f) ? NAN : v; }

static void take(const Wire &w) {
  PcMetrics m;
  m.gpuTemp = reading(w.gpuTemp);
  m.gpuLoad = reading(w.gpuLoad);
  m.gpuClock = reading(w.gpuClock);
  m.gpuPower = reading(w.gpuPower);
  m.cpuTemp = reading(w.cpuTemp);
  m.cpuLoad = reading(w.cpuLoad);
  m.cpuPower = reading(w.cpuPower);
  m.vramClock = reading(w.vramClockMhz);
  m.vramUsed = w.vramUsedMb != 0 ? (float)w.vramUsedMb : NAN;
  m.fps = reading(w.fpsCurrent);
  m.game = w.pid != 0 || !isnan(m.fps);

  // A stretch the PC did not answer for is a gap in the graph, one empty
  // sample for every poll it missed, so the time along the graph stays true
  // rather than the two sides being butted together. A reply a little late is
  // not a missed poll; one a whole poll late is.
  if (s_fpsCount > 0) {
    const uint32_t polls = (millis() - s_sampleMs) / POLL_MS;
    for (uint32_t k = 1; k < polls && k <= PC_FPS_HISTORY; k++) {
      pushFps(NAN);
    }
  }
  pushFps(m.fps);

  s_metrics = m;
  s_have = true;
  s_sampleMs = millis();
  s_samples++;
  s_error = nullptr;

  // Once per session, not twice a second. System RAM and the card's total
  // video memory are not on the panel, but they are on the wire, and it costs
  // nothing to log them once.
  if (!s_announced) {
    s_announced = true;
    Serial.printf("pc: live at %s:%u, GPU %.0f C %.0f%% %.0f MHz %.0f W,"
                  " CPU %.0f C %.0f%% %.0f W, VRAM %.0f of %u MB at %.0f MHz,"
                  " RAM %u MB at %u MHz, FPS %.0f, pid %u\n",
                  RTSS_HOST, (unsigned)RTSS_PORT, m.gpuTemp, m.gpuLoad,
                  m.gpuClock, m.gpuPower, m.cpuTemp, m.cpuLoad, m.cpuPower,
                  m.vramUsed, (unsigned)w.vramTotalMb, m.vramClock, (unsigned)w.memUsedMb,
                  (unsigned)w.memFreqMhz, m.fps, (unsigned)w.pid);
  }
}

// --- the live session -------------------------------------------------------

void pcOpen() {
  // The connect happens on the first pcPoll(), so main.cpp gets a frame on
  // the panel first. A reason left over from the last visit is not this one's.
  closeSocket();
  s_error = nullptr;
  s_announced = false;
  s_nextAt = millis();
  s_state = PC_CONNECT;
  // A graph carried over from the last visit would join two sessions minutes
  // apart as if they were one.
  s_fpsHead = 0;
  s_fpsCount = 0;
}

void pcClose() {
  closeSocket();
  s_state = PC_OFF;
}

// Reconnects straight away, without a reason on the panel or a wait: a
// keep-alive connection the server let go of between two requests is normal
// HTTP, not a failure.
static void reconnect() {
  closeSocket();
  s_state = PC_CONNECT;
}

static void step() {
  if (s_state == PC_RETRY && due(s_retryAt)) s_state = PC_CONNECT;

  switch (s_state) {
    case PC_OFF:
    case PC_RETRY:
      return;

    case PC_CONNECT:
      closeSocket();
      if (WiFi.status() != WL_CONNECTED) {
        fail(kErrWifi, "wifi is down, waiting for it");
        return;
      }
      startConnect();
      return;

    case PC_CONNECTING: {
      int err = 0;
      const int progress = connectProgress(&err);
      if (progress < 0) {
        connectFailed(err);
      } else if (progress > 0) {
        s_state = PC_IDLE;
      } else if (due(s_deadline)) {
        connectFailed(ETIMEDOUT);
      }
      return;
    }

    case PC_IDLE:
      if (!due(s_nextAt)) return;
      if (WiFi.status() != WL_CONNECTED) {
        fail(kErrWifi, "wifi dropped");
        return;
      }
      if (!sendRequest()) {
        if (s_served > 0) {
          reconnect();
        } else {
          fail(kErrDropped, "the request would not go out, errno %d", errno);
        }
        return;
      }
      // Paced from when a request goes out, not from when its answer is
      // back, so the rate stays POLL_MS however long the round trip.
      s_nextAt = millis() + POLL_MS;
      s_deadline = millis() + ANSWER_TIMEOUT_MS;
      s_len = 0;
      s_buf[0] = '\0';
      s_state = PC_WAIT;
      return;

    case PC_WAIT: {
      const int pumped = pump();
      if (s_len == 0 && pumped != 0 && s_served > 0) {
        // Closed, or reset, before a single byte of this answer: the server
        // retired the connection. Ask again on a new one.
        reconnect();
        return;
      }
      if (pumped < 0) {
        fail(kErrDropped, "recv failed, errno %d", errno);
        return;
      }

      Answer a;
      Parsed parsed = parseAnswer(s_buf, s_len, pumped > 0, &a);
      if (parsed == PARSE_MORE && s_len >= sizeof(s_buf) - 1) {
        parsed = PARSE_BAD;
      }
      if (parsed == PARSE_MORE) {
        if (due(s_deadline)) {
          fail(kErrSilent, "no answer from %s:%u in %u ms", RTSS_HOST,
               (unsigned)RTSS_PORT, (unsigned)ANSWER_TIMEOUT_MS);
        }
        return;
      }
      if (parsed == PARSE_BAD) {
        fail(kErrForeign, "unreadable answer, %u bytes, HTTP %d",
             (unsigned)s_len, a.status);
        return;
      }

      s_responses++;
      s_served++;
      if (a.status != 200) {
        fail(kErrStatus, "HTTP %d for %s", a.status, kPath);
        return;
      }
      // Every rtss_api layout so far has carried the same magic, so the
      // length is what says this is the one read here. See the header.
      if (a.bodyLen != sizeof(Wire)) {
        fail(kErrForeign, "the answer is %u bytes, expected %u%s",
             (unsigned)a.bodyLen, (unsigned)sizeof(Wire),
             a.bodyLen == 68   ? ": that is the older rtss_api without VRAM"
                                 " used, update it"
             : a.bodyLen == 76 ? ": that is the older rtss_api with the 1% lows,"
                                 " update it"
                               : "");
        return;
      }
      Wire w;
      memcpy(&w, a.body, sizeof(w));
      if (w.magic != WIRE_MAGIC) {
        fail(kErrForeign, "magic 0x%08x, expected 0x%08x", (unsigned)w.magic,
             (unsigned)WIRE_MAGIC);
        return;
      }
      take(w);

      if (pumped > 0 || !a.keepAlive) {
        reconnect();
      } else {
        s_state = PC_IDLE;
      }
      return;
    }
  }
}

bool pcPoll() {
  if (s_state == PC_OFF) return false;
  step();

  // Whether anything the panel shows is out of date: a new sample, the
  // footer's session state, the reason, or a sample going stale.
  const char *const status = pcStatus();
  const bool isFresh = fresh();
  const bool changed = status != s_shownStatus || s_error != s_shownError ||
                       isFresh != s_shownFresh || s_samples != s_shownSamples;
  s_shownStatus = status;
  s_shownError = s_error;
  s_shownFresh = isFresh;
  s_shownSamples = s_samples;
  return changed;
}

#endif  // PC_ENABLED
