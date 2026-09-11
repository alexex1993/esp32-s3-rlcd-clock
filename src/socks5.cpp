// A SOCKS5 client (RFC 1928, with the username/password auth of RFC 1929),
// for the one thing on this board that cannot be reached directly.
//
// Open-Meteo and NewsAPI are fetched straight out; api.telegram.org is not
// reachable from the network this clock sits on, which is why the pager is
// deliberately proxy-only: without SOCKS5_HOST in .env there is no pager
// screen at all rather than a screen that quietly times out.
//
// The destination is handed to the proxy as a *domain name* (ATYP 0x03)
// rather than an address, so the name is resolved on the proxy's side. That
// is the point: the local resolver either cannot reach api.telegram.org or
// cannot answer honestly about it.
//
// What comes back is an ordinary blocking socket that behaves like a direct
// connection to the destination, with send and receive timeouts already set,
// ready for the TLS handshake in telegram.cpp. The tunnel carries the
// ciphertext and nothing else: the proxy never sees the bot token, because
// the certificate is verified against the ESP-IDF root bundle on the far side
// of it.
//
// Written against the RFCs rather than pulled in as a library, in the same
// spirit as the PCF85063A and SHTC3 drivers: the whole protocol is four short
// messages.

#include <Arduino.h>

#include "app.h"

#ifdef PAGER_ENABLED

#include <errno.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#define SOCKS5_VERSION 0x05
#define SOCKS5_AUTH_NONE 0x00
#define SOCKS5_AUTH_USERPASS 0x02
#define SOCKS5_AUTH_NONE_ACCEPTABLE 0xFF
#define SOCKS5_CMD_CONNECT 0x01
#define SOCKS5_ATYP_IPV4 0x01
#define SOCKS5_ATYP_DOMAIN 0x03
#define SOCKS5_ATYP_IPV6 0x04
#define SOCKS5_USERPASS_VERSION 0x01

// The greeting is 4 bytes and the CONNECT request at most 262; the auth
// message, with a 255-byte name and a 255-byte password, is the largest of
// the three, so one buffer sized for it covers every step.
#define SOCKS5_MAX_MSG 520

// Empty unless .env carried them; an empty user means the RFC 1929 exchange
// is not offered at all.
#ifndef SOCKS5_USER
#define SOCKS5_USER ""
#endif
#ifndef SOCKS5_PASS
#define SOCKS5_PASS ""
#endif

static const char *replyError(uint8_t rep) {
  switch (rep) {
    case 0x01: return "general SOCKS server failure";
    case 0x02: return "connection not allowed by ruleset";
    case 0x03: return "network unreachable";
    case 0x04: return "host unreachable";
    case 0x05: return "connection refused";
    case 0x06: return "TTL expired";
    case 0x07: return "command not supported";
    case 0x08: return "address type not supported";
    default:   return "unknown error";
  }
}

static bool writeAll(int fd, const uint8_t *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int n = send(fd, buf + sent, len - sent, 0);
    if (n <= 0) {
      Serial.printf("socks5: send failed, errno %d\n", errno);
      return false;
    }
    sent += (size_t)n;
  }
  return true;
}

static bool readAll(int fd, uint8_t *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    const int n = recv(fd, buf + got, len - got, 0);
    if (n == 0) {
      Serial.println("socks5: the proxy closed the connection mid-handshake");
      return false;
    }
    if (n < 0) {
      Serial.printf("socks5: recv failed, errno %d\n", errno);
      return false;
    }
    got += (size_t)n;
  }
  return true;
}

// Connects with an explicit deadline. A plain blocking connect() would follow
// lwIP's SYN retry schedule instead, which takes tens of seconds to give up
// on an unreachable proxy — and the panel is frozen for every one of them.
static int tcpConnect(const char *host, uint16_t port, uint32_t timeoutMs) {
  char portStr[6];
  snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = nullptr;
  const int rc = getaddrinfo(host, portStr, &hints, &res);
  if (rc != 0 || res == nullptr) {
    Serial.printf("socks5: cannot resolve %s (getaddrinfo %d)\n", host, rc);
    return -1;
  }

  int out = -1;
  for (struct addrinfo *ai = res; ai != nullptr && out < 0; ai = ai->ai_next) {
    const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int err = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (err != 0 && errno == EINPROGRESS) {
      fd_set wset;
      FD_ZERO(&wset);
      FD_SET(fd, &wset);
      struct timeval tv = {
          .tv_sec = (time_t)(timeoutMs / 1000),
          .tv_usec = (suseconds_t)((timeoutMs % 1000) * 1000),
      };
      const int sel = select(fd + 1, nullptr, &wset, nullptr, &tv);
      if (sel > 0) {
        int soError = 0;
        socklen_t len = sizeof(soError);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
        err = (soError == 0) ? 0 : -1;
        errno = soError;
      } else {
        err = -1;
        if (sel == 0) errno = ETIMEDOUT;
      }
    }

    if (err == 0) {
      fcntl(fd, F_SETFL, flags);  // back to blocking for the handshake
      out = fd;
      break;
    }

    Serial.printf("socks5: connect to %s:%u failed, errno %d\n", host,
                  (unsigned)port, errno);
    close(fd);
  }

  freeaddrinfo(res);
  return out;
}

// Offers the methods we can actually do and hands back the one the proxy
// picked.
static bool negotiateMethod(int fd, bool wantAuth, uint8_t *outMethod) {
  uint8_t greeting[4];
  size_t len = 0;
  greeting[len++] = SOCKS5_VERSION;
  if (wantAuth) {
    greeting[len++] = 2;
    greeting[len++] = SOCKS5_AUTH_NONE;
    greeting[len++] = SOCKS5_AUTH_USERPASS;
  } else {
    greeting[len++] = 1;
    greeting[len++] = SOCKS5_AUTH_NONE;
  }
  if (!writeAll(fd, greeting, len)) return false;

  uint8_t reply[2];
  if (!readAll(fd, reply, sizeof(reply))) return false;

  if (reply[0] != SOCKS5_VERSION) {
    Serial.printf("socks5: proxy answered version 0x%02x, expected 5\n", reply[0]);
    return false;
  }
  if (reply[1] == SOCKS5_AUTH_NONE_ACCEPTABLE) {
    Serial.printf("socks5: proxy rejected every offered auth method%s\n",
                  wantAuth ? "" : " (no SOCKS5_USER in .env)");
    return false;
  }
  if (reply[1] != SOCKS5_AUTH_NONE && reply[1] != SOCKS5_AUTH_USERPASS) {
    Serial.printf("socks5: proxy chose unsupported method 0x%02x\n", reply[1]);
    return false;
  }

  *outMethod = reply[1];
  return true;
}

static bool authenticate(int fd, const char *user, const char *pass) {
  const size_t userLen = strlen(user);
  const size_t passLen = strlen(pass);
  if (userLen == 0 || userLen > 255 || passLen == 0 || passLen > 255) {
    Serial.println("socks5: proxy wants credentials but .env has none usable");
    return false;
  }

  uint8_t msg[SOCKS5_MAX_MSG];
  size_t len = 0;
  msg[len++] = SOCKS5_USERPASS_VERSION;
  msg[len++] = (uint8_t)userLen;
  memcpy(msg + len, user, userLen);
  len += userLen;
  msg[len++] = (uint8_t)passLen;
  memcpy(msg + len, pass, passLen);
  len += passLen;

  if (!writeAll(fd, msg, len)) return false;

  uint8_t reply[2];
  if (!readAll(fd, reply, sizeof(reply))) return false;
  // RFC 1929: any non-zero status is a refusal, and the server then closes.
  if (reply[1] != 0x00) {
    Serial.printf("socks5: proxy rejected the credentials (status 0x%02x)\n",
                  reply[1]);
    return false;
  }
  return true;
}

static bool requestConnect(int fd, const char *host, uint16_t port) {
  const size_t hostLen = strlen(host);
  if (hostLen == 0 || hostLen > 255) return false;

  uint8_t msg[SOCKS5_MAX_MSG];
  size_t len = 0;
  msg[len++] = SOCKS5_VERSION;
  msg[len++] = SOCKS5_CMD_CONNECT;
  msg[len++] = 0x00;  // reserved
  msg[len++] = SOCKS5_ATYP_DOMAIN;
  msg[len++] = (uint8_t)hostLen;
  memcpy(msg + len, host, hostLen);
  len += hostLen;
  msg[len++] = (uint8_t)(port >> 8);
  msg[len++] = (uint8_t)(port & 0xFF);

  if (!writeAll(fd, msg, len)) return false;

  uint8_t head[4];
  if (!readAll(fd, head, sizeof(head))) return false;
  if (head[0] != SOCKS5_VERSION) {
    Serial.printf("socks5: bad reply version 0x%02x\n", head[0]);
    return false;
  }
  if (head[1] != 0x00) {
    Serial.printf("socks5: proxy refused CONNECT to %s:%u: %s (0x%02x)\n", host,
                  (unsigned)port, replyError(head[1]), head[1]);
    return false;
  }

  // The bound address is of no use here, but it has to come off the socket
  // before the tunnel starts carrying application bytes.
  size_t boundLen;
  switch (head[3]) {
    case SOCKS5_ATYP_IPV4:
      boundLen = 4 + 2;
      break;
    case SOCKS5_ATYP_IPV6:
      boundLen = 16 + 2;
      break;
    case SOCKS5_ATYP_DOMAIN: {
      uint8_t nameLen;
      if (!readAll(fd, &nameLen, 1)) return false;
      boundLen = (size_t)nameLen + 2;
      break;
    }
    default:
      Serial.printf("socks5: unknown reply address type 0x%02x\n", head[3]);
      return false;
  }

  uint8_t discard[257];
  return readAll(fd, discard, boundLen);
}

const char *socks5Host() { return SOCKS5_HOST; }
uint16_t socks5Port() { return (uint16_t)SOCKS5_PORT; }

void socks5Close(int fd) {
  if (fd >= 0) close(fd);
}

int socks5Connect(const char *host, uint16_t port, uint32_t connectTimeoutMs,
                  uint32_t ioTimeoutMs) {
  const int fd = tcpConnect(SOCKS5_HOST, (uint16_t)SOCKS5_PORT, connectTimeoutMs);
  if (fd < 0) return -1;

  // Applied before the handshake, so a proxy that accepts the TCP connection
  // and then says nothing cannot hang the face either. The same timeouts stay
  // on the socket afterwards and are what turns a stalled TLS read into
  // MBEDTLS_ERR_SSL_WANT_READ rather than a wait with no end.
  struct timeval tv = {
      .tv_sec = (time_t)(ioTimeoutMs / 1000),
      .tv_usec = (suseconds_t)((ioTimeoutMs % 1000) * 1000),
  };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  const char *user = SOCKS5_USER;
  const char *pass = SOCKS5_PASS;
  const bool wantAuth = (user[0] != '\0');

  uint8_t method = SOCKS5_AUTH_NONE;
  if (!negotiateMethod(fd, wantAuth, &method)) {
    close(fd);
    return -1;
  }
  if (method == SOCKS5_AUTH_USERPASS && !authenticate(fd, user, pass)) {
    close(fd);
    return -1;
  }
  if (!requestConnect(fd, host, port)) {
    close(fd);
    return -1;
  }

  Serial.printf("socks5: tunnel to %s:%u open through %s:%u%s\n", host,
                (unsigned)port, SOCKS5_HOST, (unsigned)SOCKS5_PORT,
                method == SOCKS5_AUTH_USERPASS ? " (authenticated)" : "");
  return fd;
}

#endif  // PAGER_ENABLED
