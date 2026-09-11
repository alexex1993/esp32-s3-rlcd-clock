// The pager, behind the second press of KEY.
//
// One call to Telegram's getUpdates, over TLS, inside a SOCKS5 tunnel. What
// comes back is the last few messages anybody sent the bot, newest first.
//
// Three decisions worth stating before somebody "simplifies" one of them:
//
//   * SOCKS5 is not optional. api.telegram.org is not reachable from the
//     network this clock sits on, and the destination is handed to the proxy
//     as a *name* so that it is resolved on the far side too — the local
//     resolver cannot answer honestly about it either. Without SOCKS5_HOST in
//     .env there is no pager screen at all rather than one that times out.
//     See socks5.cpp.
//
//   * The certificate *is* checked here, against the ESP-IDF root bundle,
//     where weather.cpp uses plain HTTP and news.cpp deliberately does not
//     check. The difference is the proxy: it is a third party sitting in the
//     middle of this connection by design, and the thing riding through it is
//     a bot token — a credential that is the whole bot. The bundle costs
//     ~64 KB of flash on a 16 MB part, which is the cheapest part of this
//     file.
//
//   * The screen is a live session, not a fetch. Opening it brings the
//     connection up and keeps it up: one TLS handshake, then getUpdates
//     long-polled back to back over the same connection with HTTP/1.1
//     keep-alive, so a message typed into the bot is on the panel about a
//     second later and the radio is never cycled. The handshake through this
//     proxy costs seconds and is the only part worth paying twice.
//
//     Nothing blocks the loop for long. The waiting happens in PG_WAIT,
//     where each pass through loop() only asks the socket whether anything
//     has arrived; the parse itself runs when it has, and takes a few
//     hundred milliseconds. So KEY stays live and the bar's clock keeps
//     ticking through a 25-second long poll.
//
//   * The very first request after boot asks for `offset=-N`, which returns
//     the last N updates *without* confirming them: open the pager and the
//     recent backlog is there. Every request after that carries a real
//     advancing offset, which is what makes long polling block instead of
//     replaying — and which does confirm. So the backlog survives reboots
//     only until the first live poll consumes it; after that the messages
//     live in this file's ring and nowhere else. That is the price of a
//     pager that is actually live, and it is the way every Telegram bot
//     works.
//
// The answer is never held whole. It is walked once through a 256-byte window
// by a small recursive-descent reader that knows how to skip a value it does
// not want, so nesting is handled exactly — which matters here in a way it
// does not for a flat news article: a reply carries a whole second message
// inside `reply_to_message`, and a token scanner would read the quoted text
// instead of the reply. Nothing in this file allocates.
//
// HTTP/1.0 with Connection: close, like the ESP32TelegramAlert project this
// is ported from: it rules out chunked transfer-encoding, so "read until the
// peer closes" is an exact read of the whole body and there is no chunk
// parser here.

#include <Arduino.h>

#include "app.h"

static FeedItem s_items[PAGER_MAX_ITEMS];
static int s_count = 0;
static char s_stamp[8] = "";
static const char *s_error = nullptr;

int pagerCount() { return s_count; }
const FeedItem *pagerItems() { return s_items; }
const char *pagerStamp() { return s_stamp; }
const char *pagerError() { return s_error; }

#ifndef PAGER_ENABLED

// Without the credentials, the bot token and a proxy there is nothing to ask.
// The screen says which of them is missing rather than sitting empty.
void pagerOpen() {}
bool pagerPoll() { return false; }
void pagerClose() {}
bool pagerLive() { return false; }
bool pagerTakeArrival() { return false; }
uint32_t pagerResponses() { return 0; }
const char *pagerStatus() { return ""; }

#else

#include <WiFi.h>
#include <errno.h>
#include <esp_crt_bundle.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

static const char kHost[] = "api.telegram.org";
static const uint16_t kPort = 443;

// Reaching the proxy and getting a SOCKS5 CONNECT back.
static const uint32_t CONNECT_TIMEOUT_MS = 10000;
// SO_RCVTIMEO/SO_SNDTIMEO on the socket, which govern the SOCKS5 handshake in
// socks5.cpp. They stop mattering the moment setNonBlocking() runs below: from
// there on it is select() slices against TOTAL_TIMEOUT_MS.
static const uint32_t IO_SLICE_MS = 3000;
// Bringing the connection up: tunnel plus handshake. The panel is frozen for
// this one, with somebody standing in front of it, so it is kept tight — and
// it happens once per visit to the screen, not once per poll.
static const uint32_t CONNECT_BUDGET_MS = 25000;
// Sending a request, and reading an answer that has already started to
// arrive. Both are short; neither waits for Telegram to have something to say.
static const uint32_t REQUEST_BUDGET_MS = 8000;
static const uint32_t READ_BUDGET_MS = 12000;

// How long Telegram holds a poll open with nothing to report. Its own ceiling
// is 50 s; 25 keeps a dead connection from going unnoticed for a minute.
static const int LONGPOLL_S = 25;
// The wait is watched from loop(), so this only has to be longer than the
// long poll by enough to cover the round trip.
static const uint32_t WAIT_BUDGET_MS = (uint32_t)LONGPOLL_S * 1000 + 15000;
// After a failure, before trying the whole tunnel again.
static const uint32_t RETRY_MS = 8000;

// Twice the slots, so that a handful of updates the pager does not show —
// an edit, a callback, a member joining — cannot crowd the messages out of
// the tail of the queue.
static const int FETCH_UPDATES = PAGER_MAX_ITEMS * 2;

// --- the TLS session ------------------------------------------------------
// One at a time, on the loop task, so a single set of contexts is enough and
// there is nothing to lock. Torn down after every fetch: the record buffers
// are 16 KB each way and there is no reason to hold them between presses.

static int s_fd = -1;
static bool s_open = false;
static uint32_t s_deadline = 0;
static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_conf;
static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_entropy_context s_entropy;
static mbedtls_net_context s_net;

static bool pastDeadline() { return (int32_t)(millis() - s_deadline) >= 0; }

// mbedTLS only reports WANT_READ/WANT_WRITE on a *non-blocking* socket: its
// net_would_block() checks O_NONBLOCK before anything else and, on a blocking
// one, hands back a read that merely hit SO_RCVTIMEO as
// MBEDTLS_ERR_NET_RECV_FAILED — indistinguishable from the peer going away.
// The SOCKS5 handshake in socks5.cpp wants a blocking socket; TLS wants a
// non-blocking one. The switch happens here, between the two.
static bool setNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    Serial.printf("pager: cannot set O_NONBLOCK, errno %d\n", errno);
    return false;
  }
  return true;
}

// Waits for the socket to be able to make progress again, in slices short
// enough that the overall deadline is honoured to within one of them.
// Returns false only when that deadline has passed.
static bool waitSocket(bool forWrite) {
  if (pastDeadline()) return false;

  fd_set set;
  FD_ZERO(&set);
  FD_SET(s_fd, &set);
  struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
  select(s_fd + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr,
         nullptr, &tv);
  return true;
}

// True for the three things mbedTLS returns that are not failures, however
// negative they look.
//
// The last one is the trap. api.telegram.org speaks TLS 1.3, and a TLS 1.3
// server sends its NewSessionTicket *after* the handshake completes — so the
// very first mbedtls_ssl_read() of the response comes back as
// MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET rather than as data. It has to
// be read past, not treated as the end of the body: doing the latter is a
// connection that handshakes perfectly and then reports "no answer".
static bool tlsRetryable(int ret) {
  return ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
         ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET;
}

// A ticket needs no wait — the bytes for it have already arrived.
static bool tlsWouldBlock(int ret) {
  return ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE;
}

// What to do about a retryable return, in one place so every loop below
// treats it identically. False means the deadline has passed and the caller
// must give up.
//
// The deadline is checked on *every* retry, ticket or not. A peer that keeps
// handing back NewSessionTicket and never any data is not waiting on
// anything, so a loop that only checks the clock on the would-block path
// spins on it forever with the panel frozen and nothing on the console.
static bool tlsRetryWait(int ret) {
  if (pastDeadline()) return false;
  if (tlsWouldBlock(ret)) return waitSocket(ret == MBEDTLS_ERR_SSL_WANT_WRITE);
  return true;
}

static void logMbedtls(const char *what, int ret) {
  char buf[112];
  mbedtls_strerror(ret, buf, sizeof(buf));
  Serial.printf("pager: %s failed, -0x%04x (%s)\n", what, -ret, buf);
}

static void tlsClose() {
  if (s_open) {
    mbedtls_ssl_close_notify(&s_ssl);
    mbedtls_ssl_free(&s_ssl);
    mbedtls_ssl_config_free(&s_conf);
    mbedtls_ctr_drbg_free(&s_drbg);
    mbedtls_entropy_free(&s_entropy);
    s_open = false;
  }
  if (s_fd >= 0) {
    socks5Close(s_fd);
    s_fd = -1;
  }
}

static bool tlsOpen() {
  s_fd = socks5Connect(kHost, kPort, CONNECT_TIMEOUT_MS, IO_SLICE_MS);
  if (s_fd < 0) {
    s_error = "прокси недоступен";
    return false;
  }
  if (!setNonBlocking(s_fd)) {
    socks5Close(s_fd);
    s_fd = -1;
    s_error = "прокси недоступен";
    return false;
  }

  mbedtls_ssl_init(&s_ssl);
  mbedtls_ssl_config_init(&s_conf);
  mbedtls_ctr_drbg_init(&s_drbg);
  mbedtls_entropy_init(&s_entropy);
  mbedtls_net_init(&s_net);
  s_net.fd = s_fd;
  s_open = true;
  s_error = "нет защищённого канала";

  int ret = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                                  (const unsigned char *)"rlcd-pager", 10);
  if (ret != 0) {
    logMbedtls("ctr_drbg_seed", ret);
    return false;
  }

  ret = mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    logMbedtls("ssl_config_defaults", ret);
    return false;
  }

  // The bundled root store rather than a pinned certificate: Telegram rotates
  // issuers, and putting a proxy in front of them does not change who has to
  // be authenticated at the other end.
  mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  ret = esp_crt_bundle_attach(&s_conf);
  if (ret != 0) {
    Serial.printf("pager: esp_crt_bundle_attach failed (%d)\n", ret);
    return false;
  }
  mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);

  ret = mbedtls_ssl_setup(&s_ssl, &s_conf);
  if (ret != 0) {
    logMbedtls("ssl_setup", ret);
    return false;
  }
  // Both the SNI sent and the name checked against the certificate.
  ret = mbedtls_ssl_set_hostname(&s_ssl, kHost);
  if (ret != 0) {
    logMbedtls("ssl_set_hostname", ret);
    return false;
  }
  mbedtls_ssl_set_bio(&s_ssl, &s_net, mbedtls_net_send, mbedtls_net_recv,
                      nullptr);

  const uint32_t t0 = millis();
  while ((ret = mbedtls_ssl_handshake(&s_ssl)) != 0) {
    if (tlsRetryable(ret)) {
      if (!tlsRetryWait(ret)) {
        Serial.println("pager: TLS handshake timed out");
        return false;
      }
      continue;
    }
    logMbedtls("TLS handshake", ret);
    return false;
  }

  const uint32_t verify = mbedtls_ssl_get_verify_result(&s_ssl);
  if (verify != 0) {
    Serial.printf("pager: certificate rejected (0x%08lx)\n",
                  (unsigned long)verify);
    s_error = "сертификат отклонён";
    return false;
  }

  Serial.printf("pager: TLS to %s in %lu ms\n", kHost,
                (unsigned long)(millis() - t0));
  s_error = nullptr;
  return true;
}

static bool tlsWrite(const char *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int ret =
        mbedtls_ssl_write(&s_ssl, (const unsigned char *)data + sent, len - sent);
    if (tlsRetryable(ret)) {
      if (!tlsRetryWait(ret)) {
        Serial.println("pager: the request stalled on the way out");
        return false;
      }
      continue;
    }
    if (ret <= 0) {
      logMbedtls("ssl_write", ret);
      return false;
    }
    sent += (size_t)ret;
  }
  return true;
}

// --- reading the body -----------------------------------------------------

// A pull reader over the decrypted stream, with one byte of pushback so the
// value skipper can stop *before* the comma that ends a bare number.
//
// It also owns the HTTP framing, which keep-alive makes necessary: with
// Connection: close, "the body ends when the socket does" was exact. Now the
// socket outlives the answer, so the body's end has to come from
// Content-Length or from the chunk headers, or the JSON parser would run on
// into the next response's status line. `raw()` is the byte stream and
// `pull()` is the body; only the body is ever handed to the parser.
//
// In an anonymous namespace: news.cpp has a `Reader` too, and a file-scope
// struct in a .cpp has external linkage, so without this the two definitions
// collide and the linker silently gives one file the other's member
// functions. See the same note in news.cpp.
namespace {

struct Reader {
  uint8_t buf[512];
  int len = 0;
  int pos = 0;
  int peeked = -1;
  bool dead = false;  // the connection itself is gone

  // Reset per response by beginBody().
  bool bodyDone = true;
  bool chunked = false;
  long long remaining = -1;  // Content-Length countdown; -1 = until close
  long long chunkLeft = 0;
  bool chunkFirst = true;

  // Whether a byte is already in hand, which decides whether the socket has
  // to be asked at all.
  bool buffered() const { return peeked >= 0 || pos < len; }

  // One byte off the TLS stream, framing ignored: headers and chunk sizes.
  int raw() {
    if (pos < len) return buf[pos++];
    if (dead) return -1;
    for (;;) {
      const int ret = mbedtls_ssl_read(&s_ssl, buf, sizeof(buf));

      if (tlsRetryable(ret)) {
        if (!tlsRetryWait(ret)) {
          Serial.println("pager: the answer stalled");
          dead = true;
          return -1;
        }
        continue;
      }

      if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        dead = true;  // the orderly end of the connection
        return -1;
      }
      if (ret < 0) {
        // A peer that dropped the connection without a close_notify is
        // untidy rather than interesting; anything else is worth a line,
        // because this is the only place it would ever be printed.
        if (ret != MBEDTLS_ERR_NET_CONN_RESET) logMbedtls("ssl_read", ret);
        dead = true;
        return -1;
      }

      len = ret;
      pos = 0;
      return buf[pos++];
    }
  }

  // Starts a response body. `contentLength` < 0 with `isChunked` false means
  // the old read-until-close framing, which is still what a 1.0 answer or a
  // Connection: close answer uses.
  void beginBody(long long contentLength, bool isChunked) {
    peeked = -1;
    chunked = isChunked;
    remaining = isChunked ? -1 : contentLength;
    chunkLeft = 0;
    chunkFirst = true;
    bodyDone = (!isChunked && contentLength == 0);
  }

  // Reads the `<hex>[;ext]CRLF` in front of the next chunk. False at the
  // terminating zero-length chunk, whose trailers are consumed here too.
  bool nextChunk() {
    if (!chunkFirst) {  // the CRLF that closed the previous chunk
      if (raw() < 0 || raw() < 0) return false;
    }
    chunkFirst = false;

    long long n = 0;
    bool any = false;
    for (;;) {
      const int c = raw();
      if (c < 0) return false;
      if (c == '\r') {
        if (raw() < 0) return false;  // the \n
        break;
      }
      if (c == ';') {  // a chunk extension, which nothing here needs
        for (;;) {
          const int d = raw();
          if (d < 0) return false;
          if (d == '\r') {
            raw();
            break;
          }
        }
        break;
      }
      int v;
      if (c >= '0' && c <= '9') {
        v = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        v = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        v = c - 'A' + 10;
      } else {
        continue;
      }
      n = n * 16 + v;
      any = true;
    }
    if (!any) return false;

    if (n == 0) {
      // The last chunk. Trailers, then a blank line, and the body is over.
      for (;;) {
        int count = 0;
        for (;;) {
          const int c = raw();
          if (c < 0) return false;
          if (c == '\n') break;
          if (c != '\r') count++;
        }
        if (count == 0) break;
      }
      return false;
    }

    chunkLeft = n;
    return true;
  }

  // One byte of the response body, or -1 at its end.
  int pull() {
    if (bodyDone) return -1;
    if (chunked) {
      if (chunkLeft == 0 && !nextChunk()) {
        bodyDone = true;
        return -1;
      }
    } else if (remaining == 0) {
      bodyDone = true;
      return -1;
    }

    const int c = raw();
    if (c < 0) {
      bodyDone = true;
      return -1;
    }
    if (chunked) {
      chunkLeft--;
    } else if (remaining > 0) {
      remaining--;
    }
    return c;
  }

  int next() {
    if (peeked >= 0) {
      const int c = peeked;
      peeked = -1;
      return c;
    }
    return pull();
  }

  int peek() {
    if (peeked < 0) peeked = pull();
    return peeked;
  }

  // Everything the JSON parser did not want — trailing whitespace, the
  // chunked terminator. Without this the stream is left mid-body and the
  // next response's headers are read as rubbish.
  void drain() {
    while (pull() >= 0) {
    }
  }
};

}  // namespace

// jsonReadString() in text.cpp pulls through this.
static int readerByte(void *ctx) { return ((Reader *)ctx)->next(); }

// Parser scratch, shared by everything below: one fetch at a time, on one
// task. On the stack this would be most of the loop task's 8 KB.
static char s_raw[512];

static bool isJsonSpace(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// The next byte that is not whitespace, consumed.
static int skipWs(Reader &r) {
  for (;;) {
    const int c = r.next();
    if (c < 0) return -1;
    if (!isJsonSpace(c)) return c;
  }
}

// Consumes the rest of a bare number, true, false or null, stopping *before*
// the delimiter that ends it — the caller's member loop has to see that.
static bool skipScalarTail(Reader &r) {
  for (;;) {
    const int p = r.peek();
    if (p < 0) return false;
    if (p == ',' || p == '}' || p == ']' || isJsonSpace(p)) return true;
    r.next();
  }
}

// Consumes a string whose opening quote has been read, keeping none of it.
static bool skipStringBody(Reader &r) {
  for (;;) {
    const int c = r.next();
    if (c < 0) return false;
    if (c == '"') return true;
    // \uXXXX needs no special case: none of the four hex digits is a quote or
    // a backslash, so stepping over the one escaped character is enough.
    if (c == '\\' && r.next() < 0) return false;
  }
}

// Consumes one whole value whose first character `c` has already been read.
static bool skipValueFrom(Reader &r, int c) {
  if (c < 0) return false;
  if (c == '"') return skipStringBody(r);

  if (c == '{' || c == '[') {
    int depth = 1;
    while (depth > 0) {
      const int d = r.next();
      if (d < 0) return false;
      if (d == '"') {
        if (!skipStringBody(r)) return false;
      } else if (d == '{' || d == '[') {
        depth++;
      } else if (d == '}' || d == ']') {
        depth--;
      }
    }
    return true;
  }

  return skipScalarTail(r);
}

static bool skipValue(Reader &r) { return skipValueFrom(r, skipWs(r)); }

// A string value. Anything that is not a string — null, which is what
// Telegram puts in an absent optional field — reads as empty rather than as a
// failure.
static bool readStringValue(Reader &r, char *out, size_t cap) {
  const int c = skipWs(r);
  if (c < 0) return false;
  if (c != '"') {
    out[0] = '\0';
    return skipValueFrom(r, c);
  }
  return jsonReadString(readerByte, &r, out, cap);
}

// A number whose first character has already been read. Telegram's ids and
// dates stay well inside a long long.
static long long readNumberFrom(Reader &r, int first) {
  char buf[24];
  size_t n = 0;
  if (first == '-' || (first >= '0' && first <= '9')) buf[n++] = (char)first;
  for (;;) {
    const int p = r.peek();
    if (p < 0) break;
    const bool part = (p >= '0' && p <= '9') || p == '-' || p == '+' ||
                      p == '.' || p == 'e' || p == 'E';
    if (!part) break;
    if (n + 1 < sizeof(buf)) buf[n++] = (char)p;
    r.next();
  }
  buf[n] = '\0';
  return atoll(buf);
}

// Steps to the next member of an object whose '{' has been consumed, leaving
// the reader on the first character of that member's value.
// Returns 1 for a member read, 0 when the object ended, -1 when the body did.
static int nextMember(Reader &r, char *key, size_t cap) {
  int c = skipWs(r);
  if (c == ',') c = skipWs(r);
  if (c == '}') return 0;
  if (c != '"') return -1;
  if (!jsonReadString(readerByte, &r, key, cap)) return -1;
  if (skipWs(r) != ':') return -1;
  return 1;
}

// --- one message ----------------------------------------------------------

// Whoever sent it: "first last", or the username when Telegram gave no name
// at all — a bot, or an account that hides one.
static bool parseFrom(Reader &r, char *out, size_t cap) {
  char key[20];
  char first[48] = "", last[48] = "", user[48] = "";

  for (;;) {
    const int m = nextMember(r, key, sizeof(key));
    if (m < 0) return false;
    if (m == 0) break;

    if (strcmp(key, "first_name") == 0) {
      if (!readStringValue(r, first, sizeof(first))) return false;
    } else if (strcmp(key, "last_name") == 0) {
      if (!readStringValue(r, last, sizeof(last))) return false;
    } else if (strcmp(key, "username") == 0) {
      if (!readStringValue(r, user, sizeof(user))) return false;
    } else if (!skipValue(r)) {
      return false;
    }
  }

  if (first[0] != '\0' && last[0] != '\0') {
    snprintf(out, cap, "%s %s", first, last);
  } else if (first[0] != '\0') {
    snprintf(out, cap, "%s", first);
  } else {
    snprintf(out, cap, "%s", user);
  }
  return true;
}

// A group's or channel's name, for the messages that carry no sender of their
// own — a channel post has a `chat` and no `from`.
static bool parseChat(Reader &r, char *out, size_t cap) {
  char key[20];
  char title[48] = "", first[48] = "";

  for (;;) {
    const int m = nextMember(r, key, sizeof(key));
    if (m < 0) return false;
    if (m == 0) break;

    if (strcmp(key, "title") == 0) {
      if (!readStringValue(r, title, sizeof(title))) return false;
    } else if (strcmp(key, "first_name") == 0) {
      if (!readStringValue(r, first, sizeof(first))) return false;
    } else if (!skipValue(r)) {
      return false;
    }
  }

  snprintf(out, cap, "%s", title[0] != '\0' ? title : first);
  return true;
}

// The '{' of the message object has been consumed. Returns false only when
// the body ran out: a message with nothing in it still fills a slot, saying
// so, because a page that arrived is news even when it is a sticker.
static bool parseMessage(Reader &r, FeedItem *out) {
  char key[24];
  char from[96] = "", chat[96] = "";
  long long date = 0;
  bool haveText = false;

  s_raw[0] = '\0';

  for (;;) {
    const int m = nextMember(r, key, sizeof(key));
    if (m < 0) return false;
    if (m == 0) break;

    if (strcmp(key, "text") == 0) {
      if (!readStringValue(r, s_raw, sizeof(s_raw))) return false;
      haveText = true;
    } else if (strcmp(key, "caption") == 0 && !haveText) {
      // A photo or a document with something typed under it. Only used when
      // there is no text of its own; the two never both appear.
      if (!readStringValue(r, s_raw, sizeof(s_raw))) return false;
    } else if (strcmp(key, "date") == 0) {
      const int c = skipWs(r);
      if (c < 0) return false;
      date = readNumberFrom(r, c);
    } else if (strcmp(key, "from") == 0) {
      const int c = skipWs(r);
      if (c == '{') {
        if (!parseFrom(r, from, sizeof(from))) return false;
      } else if (!skipValueFrom(r, c)) {
        return false;
      }
    } else if (strcmp(key, "chat") == 0) {
      const int c = skipWs(r);
      if (c == '{') {
        if (!parseChat(r, chat, sizeof(chat))) return false;
      } else if (!skipValueFrom(r, c)) {
        return false;
      }
    } else if (!skipValue(r)) {
      // Everything else, `reply_to_message` above all: skipped whole, nesting
      // and all, so the quoted message's own text and sender never leak into
      // this one.
      return false;
    }
  }

  memset(out, 0, sizeof(*out));
  textSanitise(from[0] != '\0' ? from : chat, out->from, sizeof(out->from));
  textSanitise(s_raw, out->text, sizeof(out->text));
  if (out->text[0] == '\0') {
    // A sticker, a photo with no caption, somebody joining. The slot still
    // says something arrived and from whom, which is what a pager is for.
    snprintf(out->text, sizeof(out->text), "[без текста]");
  }

  if (date > 0) {
    // Unix seconds, UTC. gmtime_r on the shifted value is what turns it into
    // local time without depending on the ESP's own TZ being set.
    const time_t local = (time_t)date + TZ_OFFSET_SECONDS;
    struct tm t;
    gmtime_r(&local, &t);
    out->hour = (uint8_t)t.tm_hour;
    out->minute = (uint8_t)t.tm_min;
    out->timed = true;
  }
  return true;
}

// --- the last few, newest first -------------------------------------------
// Telegram hands updates back oldest first, and only the tail of them fits on
// the panel, so they go into a ring as they are read and come out backwards.
//
// The ring is *not* cleared between polls, which is the difference a live
// session makes: after the first one, each answer carries only what is new,
// and what it carries is added to what is already on the screen.

static FeedItem s_ring[PAGER_MAX_ITEMS];
static int s_ringHead = 0;
static int s_ringCount = 0;

// The read cursor. `primed` means a real update_id has been seen, so polls
// can carry a real offset and Telegram will hold them open instead of
// replaying the same batch.
static long long s_offset = 0;
static bool s_primed = false;
static bool s_firstEver = true;

// Messages added by the poll just parsed, and whether any of them should make
// a noise. The backlog the screen opens with deliberately does not.
static int s_added = 0;
static bool s_arrived = false;

static void ringPush(const FeedItem &it) {
  s_ring[s_ringHead] = it;
  s_ringHead = (s_ringHead + 1) % PAGER_MAX_ITEMS;
  if (s_ringCount < PAGER_MAX_ITEMS) s_ringCount++;
  s_added++;
}

// The '{' of the update object has been consumed.
static bool parseUpdate(Reader &r) {
  char key[24];
  FeedItem item;
  bool have = false;

  for (;;) {
    const int m = nextMember(r, key, sizeof(key));
    if (m < 0) return false;
    if (m == 0) break;

    if (strcmp(key, "update_id") == 0) {
      const int c = skipWs(r);
      if (c < 0) return false;
      const long long id = readNumberFrom(r, c);
      // Advanced past every update, including the ones dropped below: an
      // offset that stalls makes Telegram replay the same batch forever.
      if (!s_primed || id >= s_offset) {
        s_offset = id + 1;
        s_primed = true;
      }
      continue;
    }

    const bool isMessage = strcmp(key, "message") == 0 ||
                           strcmp(key, "channel_post") == 0 ||
                           strcmp(key, "edited_message") == 0;
    const int c = skipWs(r);
    if (isMessage && c == '{' && !have) {
      if (!parseMessage(r, &item)) return false;
      have = true;
    } else if (!skipValueFrom(r, c)) {
      return false;
    }
  }

  if (have) ringPush(item);
  return true;
}

// True when Telegram answered `"ok":true`. Whatever it put in `description`
// is printed on the way past, because that is the only place a wrong token or
// a bot nobody has ever messaged says so.
static bool parseResponse(Reader &r) {
  if (skipWs(r) != '{') return false;

  char key[24];
  bool ok = false;

  for (;;) {
    const int m = nextMember(r, key, sizeof(key));
    if (m < 0) break;
    if (m == 0) break;

    if (strcmp(key, "ok") == 0) {
      const int c = skipWs(r);
      if (c < 0) break;
      ok = (c == 't');
      if (!skipScalarTail(r)) break;
    } else if (strcmp(key, "description") == 0) {
      if (!readStringValue(r, s_raw, sizeof(s_raw))) break;
      Serial.printf("pager: telegram says \"%s\"\n", s_raw);
    } else if (strcmp(key, "result") == 0) {
      const int c = skipWs(r);
      if (c != '[') {
        if (!skipValueFrom(r, c)) break;
        continue;
      }
      bool bad = false;
      for (;;) {
        int d = skipWs(r);
        if (d == ',') d = skipWs(r);
        if (d == ']') break;
        if (d != '{') {
          bad = true;
          break;
        }
        if (!parseUpdate(r)) {
          bad = true;
          break;
        }
      }
      if (bad) break;
    } else if (!skipValue(r)) {
      break;
    }
  }

  return ok;
}

// --- the request ----------------------------------------------------------

// Case-insensitive "does this header line start with `name:`", and where its
// value begins. Header names are not case-sensitive and nothing guarantees
// which case a server picks.
static bool headerIs(const char *line, const char *name, const char **value) {
  size_t i = 0;
  for (; name[i] != '\0'; i++) {
    const char c = line[i];
    if (c == '\0') return false;
    const char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    if (lower != name[i]) return false;
  }
  if (line[i] != ':') return false;
  i++;
  while (line[i] == ' ' || line[i] == '\t') i++;
  *value = line + i;
  return true;
}

// True if `hay` contains `needle`, ignoring case. Both Transfer-Encoding and
// Connection can carry a list, so a prefix test is not enough.
static bool containsCI(const char *hay, const char *needle) {
  for (; *hay != '\0'; hay++) {
    size_t i = 0;
    while (needle[i] != '\0') {
      const char c = hay[i];
      const char lower = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
      if (lower != needle[i]) break;
      i++;
    }
    if (needle[i] == '\0') return true;
  }
  return false;
}

// Reads the status line and headers off the front of the answer, leaving the
// reader on the first byte of the body and telling the caller how that body
// is framed.
static bool readHttpHead(Reader &r, int *status, long long *contentLength,
                         bool *chunked, bool *keepAlive) {
  char line[200];

  *contentLength = -1;
  *chunked = false;
  *keepAlive = true;

  for (int lineNo = 0;; lineNo++) {
    size_t n = 0;
    for (;;) {
      const int c = r.raw();
      if (c < 0) return false;
      if (c == '\n') break;
      if (c != '\r' && n + 1 < sizeof(line)) line[n++] = (char)c;
    }
    line[n] = '\0';

    if (lineNo == 0) {
      int major = 0, minor = 0;
      if (sscanf(line, "HTTP/%d.%d %d", &major, &minor, status) != 3) {
        Serial.printf("pager: malformed status line \"%s\"\n", line);
        return false;
      }
      // A 1.0 answer holds the connection open only if it says so.
      if (major == 1 && minor == 0) *keepAlive = false;
      continue;
    }

    if (n == 0) return true;  // the blank line: the body starts here

    const char *value;
    if (headerIs(line, "content-length", &value)) {
      *contentLength = atoll(value);
    } else if (headerIs(line, "transfer-encoding", &value)) {
      if (containsCI(value, "chunked")) *chunked = true;
    } else if (headerIs(line, "connection", &value)) {
      if (containsCI(value, "close")) *keepAlive = false;
      if (containsCI(value, "keep-alive")) *keepAlive = true;
    }
  }
}

// --- the live session -----------------------------------------------------
//
// One connection, held open, with a long poll always outstanding on it. Every
// state but PG_CONNECT and PG_READ returns to loop() promptly; those two are
// the only ones that block, and neither waits on Telegram having something to
// say.

enum PagerState {
  PG_OFF = 0,   // the screen is not up
  PG_CONNECT,   // (re)build the tunnel and the TLS session
  PG_REQUEST,   // send one getUpdates
  PG_WAIT,      // a poll is outstanding; ask the socket, do not block
  PG_READ,      // an answer has started to arrive
  PG_RETRY,     // something failed; wait before trying the tunnel again
};

static PagerState s_state = PG_OFF;
static uint32_t s_waitUntil = 0;
static uint32_t s_retryAt = 0;
static uint32_t s_responses = 0;
static bool s_keepAlive = true;
static Reader s_reader;

uint32_t pagerResponses() { return s_responses; }
bool pagerLive() { return s_state == PG_REQUEST || s_state == PG_WAIT; }

bool pagerTakeArrival() {
  const bool was = s_arrived;
  s_arrived = false;
  return was;
}

const char *pagerStatus() {
  switch (s_state) {
    case PG_CONNECT: return "подключаюсь...";
    case PG_REQUEST:
    case PG_READ:    return "на связи";
    case PG_WAIT:    return "на связи";
    case PG_RETRY:   return "переподключаюсь...";
    default:         return "";
  }
}

// Copies the ring out newest-first and stamps the clock.
static void publish() {
  s_count = s_ringCount;
  for (int i = 0; i < s_ringCount; i++) {
    const int slot = (s_ringHead - 1 - i + PAGER_MAX_ITEMS) % PAGER_MAX_ITEMS;
    s_items[i] = s_ring[slot];
  }

  struct tm now;
  const time_t t = time(nullptr);
  localtime_r(&t, &now);
  snprintf(s_stamp, sizeof(s_stamp), "%02d:%02d", now.tm_hour, now.tm_min);
}

static bool sendRequest() {
  char body[176];
  int bodyLen;

  if (s_primed) {
    // A real offset, so Telegram holds the poll open until there is
    // something new rather than handing the same batch back at once.
    bodyLen = snprintf(body, sizeof(body),
                       "{\"offset\":%lld,\"limit\":%d,\"timeout\":%d,"
                       "\"allowed_updates\":[\"message\",\"channel_post\"]}",
                       s_offset, FETCH_UPDATES, LONGPOLL_S);
  } else {
    // Nothing seen yet. The very first request of all returns immediately
    // with the backlog; after that an empty queue is worth waiting on.
    bodyLen = snprintf(body, sizeof(body),
                       "{\"offset\":-%d,\"limit\":%d,\"timeout\":%d,"
                       "\"allowed_updates\":[\"message\",\"channel_post\"]}",
                       FETCH_UPDATES, FETCH_UPDATES,
                       s_firstEver ? 0 : LONGPOLL_S);
  }

  // The token is in the path, which is where Telegram puts it and why this
  // request is worth a verified certificate.
  char head[320];
  const int headLen =
      snprintf(head, sizeof(head),
               "POST /bot%s/getUpdates HTTP/1.1\r\n"
               "Host: %s\r\n"
               "User-Agent: rlcd-esp32s3-pager\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: %d\r\n"
               "Connection: keep-alive\r\n"
               "\r\n",
               TELEGRAM_BOT_TOKEN, kHost, bodyLen);
  if (headLen <= 0 || headLen >= (int)sizeof(head)) {
    Serial.println("pager: the request headers do not fit");
    s_error = "неверный токен бота";
    return false;
  }

  return tlsWrite(head, (size_t)headLen) && tlsWrite(body, (size_t)bodyLen);
}

// One whole answer. Returns false when the session has to be rebuilt.
static bool readResponse(bool *changed) {
  int status = 0;
  long long contentLength = -1;
  bool chunked = false;
  bool keepAlive = true;

  if (!readHttpHead(s_reader, &status, &contentLength, &chunked, &keepAlive)) {
    Serial.println("pager: no answer from telegram");
    s_error = "телеграм не ответил";
    return false;
  }
  s_reader.beginBody(contentLength, chunked);

  s_added = 0;
  const bool ok = parseResponse(s_reader);
  s_reader.drain();
  s_keepAlive = keepAlive && !s_reader.dead;
  s_responses++;

  if (!ok) {
    // A 401 is a bad token, a 404 a token that is not a bot; both arrive
    // with a description, which parseResponse has already printed.
    Serial.printf("pager: telegram refused the request (HTTP %d)\n", status);
    s_error = "телеграм отклонил запрос";
    return false;
  }

  if (s_added > 0) {
    publish();
    s_error = nullptr;
    // The backlog the screen opens with is history somebody asked to see,
    // not a page arriving — so it does not make a noise.
    if (!s_firstEver) s_arrived = true;
    Serial.printf("pager: %d new, newest from %s: \"%s\"\n", s_added,
                  s_items[0].from, s_items[0].text);
    *changed = true;
  } else if (s_firstEver) {
    publish();
    if (s_count == 0) {
      Serial.println("pager: nothing in the queue");
      s_error = "нет сообщений";
    }
    *changed = true;
  }

  s_firstEver = false;
  return true;
}

// True when a byte is already in hand, decrypted and waiting, or the socket
// says one has arrived. This is what keeps a 25-second long poll from
// costing 25 seconds of frozen panel.
static bool answerStarted() {
  if (s_reader.buffered()) return true;
  if (mbedtls_ssl_get_bytes_avail(&s_ssl) > 0) return true;

  fd_set set;
  FD_ZERO(&set);
  FD_SET(s_fd, &set);
  struct timeval tv = {.tv_sec = 0, .tv_usec = 0};
  return select(s_fd + 1, &set, nullptr, nullptr, &tv) > 0;
}

static void dropSession(const char *why) {
  if (why != nullptr) Serial.printf("pager: %s, reconnecting\n", why);
  tlsClose();
  s_reader = Reader();
  s_state = PG_RETRY;
  s_retryAt = millis() + RETRY_MS;
}

void pagerOpen() {
  // The connect itself happens on the first pagerPoll(), so main.cpp gets to
  // put a frame on the panel before the several seconds it takes.
  s_state = PG_CONNECT;
  s_arrived = false;
}

void pagerClose() {
  if (s_state != PG_OFF) {
    tlsClose();
    s_reader = Reader();
    s_state = PG_OFF;
  }
}

bool pagerPoll() {
  switch (s_state) {
    case PG_OFF:
      return false;

    case PG_RETRY:
      if ((int32_t)(millis() - s_retryAt) < 0) return false;
      s_state = PG_CONNECT;
      return true;

    case PG_CONNECT: {
      tlsClose();
      s_reader = Reader();
      s_deadline = millis() + CONNECT_BUDGET_MS;
      s_error = nullptr;
      if (!tlsOpen()) {
        dropSession(nullptr);
        return true;
      }
      s_keepAlive = true;
      s_state = PG_REQUEST;
      return true;
    }

    case PG_REQUEST: {
      s_deadline = millis() + REQUEST_BUDGET_MS;
      if (!sendRequest()) {
        dropSession("the request would not go out");
        return true;
      }
      s_waitUntil = millis() + WAIT_BUDGET_MS;
      s_state = PG_WAIT;
      return false;
    }

    case PG_WAIT: {
      if (answerStarted()) {
        s_state = PG_READ;
        return false;
      }
      if ((int32_t)(millis() - s_waitUntil) >= 0) {
        // Telegram closes a long poll on its own well before this, so a
        // silence this long is the connection, not the bot.
        dropSession("the long poll went quiet");
        return true;
      }
      return false;
    }

    case PG_READ: {
      s_deadline = millis() + READ_BUDGET_MS;
      bool changed = false;
      const bool ok = readResponse(&changed);
      if (!ok || !s_keepAlive) {
        // A refused request is not worth hammering; a closed keep-alive just
        // needs the connection built again, which PG_RETRY also does.
        dropSession(ok ? "the peer closed the connection" : nullptr);
        return true;
      }
      s_state = PG_REQUEST;
      return changed;
    }
  }
  return false;
}

#endif  // PAGER_ENABLED
