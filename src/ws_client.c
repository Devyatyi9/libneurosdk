#include "ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

/* ================================================================== */
/*  Platform detection                                                 */
/* ================================================================== */
#if defined(_WIN32)
  #define WS_PLATFORM_WIN 1
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <bcrypt.h>
  /* MSVC deprecates POSIX names; map to ISO C equivalents */
  #define strncasecmp _strnicmp
  #define strdup _strdup
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
  #define WS_PLATFORM_POSIX 1
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #if defined(__linux__)
    #include <sys/random.h>
  #endif
  #define SOCKET_ERROR (-1)
  #define INVALID_SOCKET (-1)
  typedef int SOCKET;
#else
  #error "Unsupported platform"
#endif

/* Fixed timeout for blocking-until-writable retries inside "send all".
 * Not exposed via the public API yet -- for a local, low-latency
 * connection this is plenty; revisit if this ever talks to anything
 * that isn't on the same machine. */
#define WS_SEND_TIMEOUT_MS 5000

/* ================================================================== */
/*  Internal structures                                                 */
/* ================================================================== */
#define WS_RECV_BUF_SIZE 262144

struct ws_t {
  ws_state_e state;
  ws_callbacks_t callbacks;

  SOCKET fd;
  char recv_buf[WS_RECV_BUF_SIZE];
  size_t recv_len;

  /* Parsed URL parts */
  char *host;
  uint16_t port;
  char *path;

  /* Upgrade response state */
  int upgrade_done;
  char key[64];  /* Sec-WebSocket-Key sent during upgrade */

  /* Fragmented-message reassembly (FIN=0 ... continuation ... FIN=1) */
  int frag_active;
  int frag_binary;
  unsigned char *frag_buf;
  size_t frag_len;
  size_t frag_cap;

  /* Close handshake state */
  int closing_initiated;  /* non-zero if WE initiated the close */
};

/* ================================================================== */
/*  socket_io — cross-platform socket abstraction                      */
/* ================================================================== */

#if defined(WS_PLATFORM_WIN)
static int sock_refcount = 0;
static int sock_init(void) {
  if (sock_refcount++ == 0) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      sock_refcount--;
      return -1;
    }
  }
  return 0;
}
static void sock_cleanup(void) {
  if (--sock_refcount == 0) WSACleanup();
}
#else
static int sock_init(void) { return 0; }
static void sock_cleanup(void) {}
#endif

static SOCKET sock_create(void) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return INVALID_SOCKET;
  return s;
}

static void sock_close(SOCKET s) {
  if (s == INVALID_SOCKET) return;
#if defined(WS_PLATFORM_WIN)
  closesocket(s);
#else
  close(s);
#endif
}

static int sock_set_nonblock(SOCKET s) {
#if defined(WS_PLATFORM_WIN)
  unsigned long mode = 1;
  return ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : -1;
#else
  int flags = fcntl(s, F_GETFL, 0);
  if (flags == -1) return -1;
  return fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* Centralised "would this call have blocked" check -- used for both
 * recv() and send() paths so the platform #ifdef only lives in one place. */
static int sock_would_block(void) {
#if defined(WS_PLATFORM_WIN)
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static int sock_connect_nonblock(SOCKET s, const struct sockaddr *addr, socklen_t addrlen) {
  int rc = connect(s, addr, addrlen);
  if (rc == 0) return 0; /* immediate connect */
#if defined(WS_PLATFORM_WIN)
  if (WSAGetLastError() == WSAEWOULDBLOCK) return 0;
#else
  if (errno == EINPROGRESS) return 0;
#endif
  return -1;
}

#if defined(WS_PLATFORM_WIN)
static int sock_poll_writable(SOCKET s, int timeout_ms) {
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
  fd_set efds; FD_ZERO(&efds); FD_SET(s, &efds);
  int rc = select((int)(s + 1), NULL, &wfds, &efds, timeout_ms < 0 ? NULL : &tv);
  if (rc <= 0) return rc;
  if (FD_ISSET(s, &efds)) return -1;
  int err = 0; socklen_t errlen = sizeof(err);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) != 0 || err != 0) return -1;
  return 1;
}
#else
static int sock_poll_writable(SOCKET s, int timeout_ms) {
  struct pollfd pfd = {s, POLLOUT, 0};
  int rc = poll(&pfd, 1, timeout_ms);
  if (rc <= 0) return rc;
  if (pfd.revents & POLLERR) return -1;
  int err = 0; socklen_t errlen = sizeof(err);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0) return -1;
  return 1;
}
#endif
/* Connect-completion check is the same "wait writable + confirm no
 * pending SO_ERROR" test as a normal write-readiness wait, so it's the
 * same function under a name that reads well at each call site. */
#define sock_poll_connect sock_poll_writable

static int sock_recv(SOCKET s, void *buf, size_t len) {
#if defined(WS_PLATFORM_WIN)
  return recv(s, (char *)buf, (int)len, 0);
#else
  return (int)recv(s, buf, len, 0);
#endif
}

static int sock_send(SOCKET s, const void *buf, size_t len) {
#if defined(WS_PLATFORM_WIN)
  return send(s, (const char *)buf, (int)len, 0);
#else
  return (int)send(s, buf, len, 0);
#endif
}

/* Loops until all `len` bytes are sent, all on a *non-blocking* socket.
 * A short write is normal here (kernel send buffer momentarily full) --
 * it must NOT be treated as failure, only a hard error or a
 * WS_SEND_TIMEOUT_MS wait with no writability is. Fixes the bug where a
 * partial header/mask/payload write silently desynced the WS stream. */
static int sock_send_all(SOCKET s, const void *buf, size_t len) {
  const unsigned char *p = (const unsigned char *)buf;
  size_t sent = 0;
  while (sent < len) {
    int rc = sock_send(s, p + sent, len - sent);
    if (rc > 0) {
      sent += (size_t)rc;
      continue;
    }
    if (rc < 0 && sock_would_block()) {
      int wr = sock_poll_writable(s, WS_SEND_TIMEOUT_MS);
      if (wr <= 0) return -1; /* timeout or error while waiting to write */
      continue;
    }
    return -1; /* hard error, or rc == 0 which shouldn't happen for len > 0 */
  }
  return (int)sent;
}

static int sock_resolve(const char *host, uint16_t port, struct sockaddr_in *out) {
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", port);
  int rc = getaddrinfo(host, port_str, &hints, &res);
  if (rc != 0 || res == NULL) return -1;
  *out = *(struct sockaddr_in *)res->ai_addr;
  freeaddrinfo(res);
  return 0;
}

/* ================================================================== */
/*  CSPRNG — replaces rand() for masking keys and Sec-WebSocket-Key    */
/* ================================================================== */
static void ws_random_bytes(unsigned char *buf, size_t len) {
#if defined(_WIN32)
  BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif defined(__linux__)
  getrandom(buf, len, 0);
#elif defined(__APPLE__)
  arc4random_buf(buf, len);
#else
  /* Fallback for unsupported POSIX platforms */
  for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(rand() & 0xff);
#endif
}

/* ================================================================== */
/*  URL parsing (minimal, only ws://)                                  */
/* ================================================================== */
static int parse_url(const char *url, char **host, uint16_t *port, char **path) {
  /* Expect: ws://host[:port][/path] */
  if (strncmp(url, "ws://", 5) != 0) return -1;
  const char *p = url + 5;
  const char *host_start = p;
  while (*p && *p != ':' && *p != '/') p++;
  size_t host_len = (size_t)(p - host_start);
  if (host_len == 0) return -1;

  *host = (char *)malloc(host_len + 1);
  if (!*host) return -1;
  memcpy(*host, host_start, host_len);
  (*host)[host_len] = '\0';

  if (*p == ':') {
    p++;
    char *end;
    long pn = strtol(p, &end, 10);
    if (end == p || pn <= 0 || pn > 65535) { free(*host); return -1; }
    *port = (uint16_t)pn;
    p = end;
  } else {
    *port = 80;
  }

  if (*p == '/') {
    *path = strdup(p);
    if (!*path) { free(*host); return -1; }
  } else {
    *path = strdup("/");
    if (!*path) { free(*host); return -1; }
  }

  return 0;
}

/* ================================================================== */
/*  Forward declarations                                               */
/* ================================================================== */
static void base64_encode(const unsigned char *in, size_t inlen, char *out, size_t outsize);

/* ================================================================== */
/*  HTTP upgrade                                                        */
/* ================================================================== */
static int build_upgrade_request(char *buf, size_t size,
                                  const char *host, uint16_t port,
                                  const char *path,
                                  const char *key) {
  return snprintf(buf, size,
    "GET %s HTTP/1.1\r\n"
    "Host: %s:%u\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Origin: http://local.neuro-integration\r\n"
    "\r\n",
    path, host, port, key);
}

static int generate_key(char *out, size_t size) {
  unsigned char nonce[16];
  ws_random_bytes(nonce, 16);
  base64_encode(nonce, 16, out, size);
  return (int)strlen(out);
}

/* ================================================================== */
/*  SHA-1 (FIPS 180-4) — vendored, minimal, no external deps           */
/* ================================================================== */
struct sha1_ctx {
  uint32_t state[5];
  uint64_t count;
  unsigned char buffer[64];
};

static void sha1_init(struct sha1_ctx *ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->count = 0;
}

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const unsigned char block[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
  for (int i = 16; i < 80; i++)
    w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 20; i++) {
    uint32_t tmp = ROTL32(a, 5) + ((b & c) | (~b & d)) + e + w[i] + 0x5A827999;
    e = d; d = c; c = ROTL32(b, 30); b = a; a = tmp;
  }
  for (int i = 20; i < 40; i++) {
    uint32_t tmp = ROTL32(a, 5) + (b ^ c ^ d) + e + w[i] + 0x6ED9EBA1;
    e = d; d = c; c = ROTL32(b, 30); b = a; a = tmp;
  }
  for (int i = 40; i < 60; i++) {
    uint32_t tmp = ROTL32(a, 5) + ((b & c) | (d & (b | c))) + e + w[i] + 0x8F1BBCDC;
    e = d; d = c; c = ROTL32(b, 30); b = a; a = tmp;
  }
  for (int i = 60; i < 80; i++) {
    uint32_t tmp = ROTL32(a, 5) + (b ^ c ^ d) + e + w[i] + 0xCA62C1D6;
    e = d; d = c; c = ROTL32(b, 30); b = a; a = tmp;
  }

  state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_update(struct sha1_ctx *ctx, const unsigned char *data, size_t len) {
  size_t idx = (size_t)(ctx->count & 63);
  ctx->count += len;

  if (idx) {
    size_t fill = 64 - idx;
    if (len < fill) { memcpy(ctx->buffer + idx, data, len); return; }
    memcpy(ctx->buffer + idx, data, fill);
    sha1_transform(ctx->state, ctx->buffer);
    data += fill; len -= fill;
  }

  while (len >= 64) {
    sha1_transform(ctx->state, data);
    data += 64; len -= 64;
  }

  if (len) memcpy(ctx->buffer, data, len);
}

static void sha1_final(struct sha1_ctx *ctx, unsigned char out[20]) {
  uint64_t bits = ctx->count * 8;
  size_t idx = (size_t)(ctx->count & 63);

  ctx->buffer[idx++] = 0x80;
  if (idx > 56) {
    memset(ctx->buffer + idx, 0, 64 - idx);
    sha1_transform(ctx->state, ctx->buffer);
    idx = 0;
  }
  memset(ctx->buffer + idx, 0, 56 - idx);

  for (int i = 0; i < 8; i++)
    ctx->buffer[56 + i] = (unsigned char)(bits >> (56 - i * 8));
  sha1_transform(ctx->state, ctx->buffer);

  for (int i = 0; i < 5; i++) {
    out[i * 4]     = (unsigned char)(ctx->state[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
    out[i * 4 + 3] = (unsigned char)(ctx->state[i]);
  }
}

/* ================================================================== */
/*  Base64 encode (RFC 4648 §4) — used for key gen + accept check      */
/* ================================================================== */
static void base64_encode(const unsigned char *in, size_t inlen, char *out, size_t outsize) {
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0, o = 0;
  while (i < inlen && o + 4 < outsize) {
    unsigned long v = ((unsigned long)in[i]) << 16;
    if (i + 1 < inlen) v |= ((unsigned long)in[i + 1]) << 8;
    if (i + 2 < inlen) v |= (unsigned long)in[i + 2];
    out[o++] = b64[(v >> 18) & 0x3f];
    out[o++] = b64[(v >> 12) & 0x3f];
    out[o++] = (i + 1 < inlen) ? b64[(v >> 6) & 0x3f] : '=';
    out[o++] = (i + 2 < inlen) ? b64[v & 0x3f] : '=';
    i += 3;
  }
  if (o < outsize) out[o] = '\0';
}

/* ================================================================== */
/*  HTTP upgrade response parser                                        */
/* ================================================================== */

/* Looks for the end of the HTTP header block ("\r\n\r\n") before
 * declaring the upgrade done, reports header length, AND verifies the
 * Sec-WebSocket-Accept header (RFC 6455 §4.2.2) against our key. */
static int parse_upgrade_response(const char *buf, size_t len, size_t *header_len_out, const char *expected_key) {
  if (len >= 12 && memcmp(buf, "HTTP/1.1 ", 9) == 0 && memcmp(buf, "HTTP/1.1 101", 12) != 0) {
    return -1; /* non-101 status, fail fast even before full headers arrive */
  }
  if (len >= 4 && memcmp(buf, "HTTP", 4) != 0) {
    return -1; /* not an HTTP response at all */
  }

  const char *end = NULL;
  for (size_t i = 0; i + 4 <= len; i++) {
    if (memcmp(buf + i, "\r\n\r\n", 4) == 0) { end = buf + i + 4; break; }
  }
  if (!end) return 0; /* headers not fully received yet */

  if (len < 12 || memcmp(buf, "HTTP/1.1 101", 12) != 0) return -1;
  *header_len_out = (size_t)(end - buf);

  /* Verify Upgrade: websocket and Connection: Upgrade (§4.1 val.2-3) */
  int has_upgrade = 0, has_conn = 0;
  for (size_t i = 9; i + 2 < *header_len_out; i++) {
    if (strncasecmp(buf + i, "upgrade:", 8) == 0) {
      const char *v = buf + i + 8;
      while (*v == ' ' || *v == '\t') v++;
      if (strncasecmp(v, "websocket", 9) == 0) has_upgrade = 1;
    } else if (strncasecmp(buf + i, "connection:", 11) == 0) {
      const char *v = buf + i + 11;
      while (*v == ' ' || *v == '\t') v++;
      /* Check for "Upgrade" token (possibly among others) */
      while (*v && *v != '\r' && *v != '\n') {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        if (strncasecmp(v, "Upgrade", 7) == 0) { has_conn = 1; break; }
        while (*v && *v != ',' && *v != '\r' && *v != '\n') v++;
      }
    }
  }
  if (!has_upgrade || !has_conn) return -1;

  /* Verify Sec-WebSocket-Accept (RFC 6455 §4.2.2) */
  if (expected_key) {
    /* Find the Sec-WebSocket-Accept header value */
    const char *accept_hdr = NULL;
    for (size_t i = 9; i + 22 < *header_len_out; i++) {
      if (strncasecmp(buf + i, "Sec-WebSocket-Accept:", 21) == 0) {
        accept_hdr = buf + i + 21;
        while (*accept_hdr == ' ' || *accept_hdr == '\t') accept_hdr++;
        break;
      }
    }
    if (!accept_hdr) return -1;

    /* Compute expected accept: Base64(SHA-1(key + magic GUID)) */
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    struct sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const unsigned char *)expected_key, strlen(expected_key));
    sha1_update(&ctx, (const unsigned char *)magic, strlen(magic));
    unsigned char hash[20];
    sha1_final(&ctx, hash);

    char expected[64];
    base64_encode(hash, 20, expected, sizeof(expected));

    /* Compare with server's value (up to the end of the line or header
     * boundary — \r\n, \n, or end of buffer) */
    size_t accept_len = 0;
    while (accept_hdr[accept_len] && accept_hdr[accept_len] != '\r' && accept_hdr[accept_len] != '\n')
      accept_len++;
    if (accept_len != strlen(expected) || memcmp(accept_hdr, expected, accept_len) != 0)
      return -1;
  }

  return 1;
}

/* ================================================================== */
/*  Fragmented-message reassembly buffer                               */
/* ================================================================== */
static int frag_append(ws_t *ws, const unsigned char *data, size_t len) {
  if (ws->frag_len + len > ws->frag_cap) {
    size_t newcap = ws->frag_cap ? ws->frag_cap * 2 : 4096;
    while (newcap < ws->frag_len + len) newcap *= 2;
    unsigned char *nb = (unsigned char *)realloc(ws->frag_buf, newcap);
    if (!nb) return -1;
    ws->frag_buf = nb;
    ws->frag_cap = newcap;
  }
  if (len) memcpy(ws->frag_buf + ws->frag_len, data, len);
  ws->frag_len += len;
  return 0;
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

int ws_connect(ws_t **out, const char *url, ws_callbacks_t callbacks) {
  if (!out || !url) return -1;

  ws_t *ws = (ws_t *)calloc(1, sizeof(ws_t));
  if (!ws) return -1;

  ws->state = WS_STATE_INIT;
  ws->fd = INVALID_SOCKET;
  ws->callbacks = callbacks;

  if (parse_url(url, &ws->host, &ws->port, &ws->path) != 0) {
    free(ws);
    return -1;
  }

  if (sock_init() != 0) {
    free(ws->host); free(ws->path); free(ws);
    return -1;
  }

  ws->state = WS_STATE_CONNECTING;
  *out = ws;
  return 0;
}

int ws_send(ws_t *ws, const char *data, size_t len) {
  if (!ws || ws->state != WS_STATE_OPEN) return -1;
  unsigned char header[10];
  size_t hlen = 2;
  header[0] = 0x81; /* FIN + text opcode */
  header[1] = 0x80; /* MASK bit set */
  if (len < 126) {
    header[1] |= (unsigned char)len;
  } else if (len < 65536) {
    header[1] |= 126;
    header[2] = (unsigned char)(len >> 8);
    header[3] = (unsigned char)(len);
    hlen = 4;
  } else {
    header[1] |= 127;
    uint64_t n = (uint64_t)len;
    for (int i = 0; i < 8; i++)
      header[hlen + i] = (unsigned char)(n >> (56 - i * 8));
    hlen = 10;
  }

  unsigned char mask[4];
  ws_random_bytes(mask, 4);

  if (sock_send_all(ws->fd, header, hlen) != (int)hlen) return -1;
  if (sock_send_all(ws->fd, mask, 4) != 4) return -1;

  unsigned char *masked = (unsigned char *)malloc(len);
  if (!masked) return -1;
  for (size_t i = 0; i < len; i++) masked[i] = (unsigned char)data[i] ^ mask[i & 3];
  int rc = sock_send_all(ws->fd, masked, len);
  free(masked);
  return (rc == (int)len) ? 0 : -1;
}

int ws_send_binary(ws_t *ws, const char *data, size_t len) {
  if (!ws || ws->state != WS_STATE_OPEN) return -1;
  unsigned char header[10];
  size_t hlen = 2;
  header[0] = 0x82;
  header[1] = 0x80;
  if (len < 126) {
    header[1] |= (unsigned char)len;
  } else if (len < 65536) {
    header[1] |= 126;
    header[2] = (unsigned char)(len >> 8);
    header[3] = (unsigned char)(len);
    hlen = 4;
  } else {
    header[1] |= 127;
    uint64_t n = (uint64_t)len;
    for (int i = 0; i < 8; i++)
      header[hlen + i] = (unsigned char)(n >> (56 - i * 8));
    hlen = 10;
  }

  unsigned char mask[4];
  ws_random_bytes(mask, 4);

  if (sock_send_all(ws->fd, header, hlen) != (int)hlen) return -1;
  if (sock_send_all(ws->fd, mask, 4) != 4) return -1;

  unsigned char *masked = (unsigned char *)malloc(len);
  if (!masked) return -1;
  for (size_t i = 0; i < len; i++) masked[i] = (unsigned char)data[i] ^ mask[i & 3];
  int rc = sock_send_all(ws->fd, masked, len);
  free(masked);
  return (rc == (int)len) ? 0 : -1;
}

static int ws_utf8_valid(const unsigned char *s, size_t len) {
  size_t i = 0;
  while (i < len) {
    unsigned int cp;
    int n;
    if (s[i] <= 0x7F) { i++; continue; }
    else if ((s[i] & 0xE0) == 0xC0) { cp = s[i] & 0x1F; n = 2; }
    else if ((s[i] & 0xF0) == 0xE0) { cp = s[i] & 0x0F; n = 3; }
    else if ((s[i] & 0xF8) == 0xF0) { cp = s[i] & 0x07; n = 4; }
    else return 0;
    if (i + (size_t)n > len) return 0;
    for (int j = 1; j < n; j++) {
      if ((s[i+j] & 0xC0) != 0x80) return 0;
      cp = (cp << 6) | (s[i+j] & 0x3F);
    }
    if (n == 2 && cp < 0x80) return 0;
    if (n == 3 && cp < 0x800) return 0;
    if (n == 4 && cp < 0x10000) return 0;
    if (cp > 0x10FFFF) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    i += (size_t)n;
  }
  return 1;
}

/* Helper to build a masked Close or Pong response and send it.
 * Called when we need to reply to a received control frame. */
static void ws_send_control(ws_t *ws, unsigned char opcode, const unsigned char *payload, size_t payload_len) {
  unsigned char hdr[4] = {opcode, 0x80};
  unsigned char mask[4];
  unsigned char *tmp = NULL;
  size_t hlen = 2;
  if (payload_len > 125) payload_len = 125;
  hdr[1] |= (unsigned char)payload_len;
  ws_random_bytes(mask, 4);
  if (payload_len > 0) {
    tmp = (unsigned char *)malloc(payload_len);
    if (tmp) {
      for (size_t i = 0; i < payload_len; i++) tmp[i] = payload[i] ^ mask[i & 3];
    }
  }
  sock_send_all(ws->fd, hdr, hlen);
  sock_send_all(ws->fd, mask, 4);
  if (tmp) { sock_send_all(ws->fd, tmp, payload_len); free(tmp); }
}

ws_event_e ws_poll(ws_t *ws, int timeout_ms) {
  if (!ws) return WS_EVENT_ERROR;

  switch (ws->state) {

    /* ---- CONNECTING: initiate TCP connection ---- */
    case WS_STATE_CONNECTING: {
      struct sockaddr_in addr;
      if (sock_resolve(ws->host, ws->port, &addr) != 0) {
        ws->state = WS_STATE_ERROR;
        if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "DNS resolve failed", ws->callbacks.userdata);
        return WS_EVENT_ERROR;
      }

      SOCKET s = sock_create();
      if (s == INVALID_SOCKET) {
        ws->state = WS_STATE_ERROR;
        if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "socket() failed", ws->callbacks.userdata);
        return WS_EVENT_ERROR;
      }
      sock_set_nonblock(s);

      if (sock_connect_nonblock(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sock_close(s);
        ws->state = WS_STATE_ERROR;
        if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "connect() failed", ws->callbacks.userdata);
        return WS_EVENT_ERROR;
      }

      ws->fd = s;
      ws->state = WS_STATE_UPGRADING;
      /* fall through to UPGRADING — poll for connect completion */
    }

    /* ---- UPGRADING: wait for TCP connect, then send HTTP upgrade ---- */
    case WS_STATE_UPGRADING: {
      if (!ws->upgrade_done) {
        int rc = sock_poll_connect(ws->fd, timeout_ms);
        if (rc < 0) {
          ws->state = WS_STATE_ERROR;
          if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "TCP connect failed", ws->callbacks.userdata);
          return WS_EVENT_ERROR;
        }
        if (rc == 0) return WS_EVENT_NONE; /* still connecting */

        char key[32];
        generate_key(key, sizeof(key));
        memcpy(ws->key, key, sizeof(key));
        ws->key[sizeof(key)] = '\0';

        char req[1024];
        int reqlen = build_upgrade_request(req, sizeof(req), ws->host, ws->port, ws->path, key);
        /* snprintf returns the length that WOULD have been written; if
         * it's >= sizeof(req) the request was truncated and reqlen no
         * longer describes real bytes in req -- sending it as-is would
         * read past the buffer. Treat that as a hard error instead. */
        if (reqlen <= 0 || (size_t)reqlen >= sizeof(req)) {
          ws->state = WS_STATE_ERROR;
          if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "upgrade request too large for buffer", ws->callbacks.userdata);
          return WS_EVENT_ERROR;
        }

        int sent = sock_send_all(ws->fd, req, (size_t)reqlen);
        if (sent != reqlen) {
          ws->state = WS_STATE_ERROR;
          if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "failed to send upgrade request", ws->callbacks.userdata);
          return WS_EVENT_ERROR;
        }
        ws->upgrade_done = 1;
        return WS_EVENT_NONE; /* wait for response */
      }

      if (ws->recv_len >= WS_RECV_BUF_SIZE) {
        /* Headers alone filled the whole buffer without a "\r\n\r\n" in
         * sight -- something is very wrong upstream. */
        ws->state = WS_STATE_ERROR;
        if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "upgrade response too large for buffer", ws->callbacks.userdata);
        return WS_EVENT_ERROR;
      }

      int n = sock_recv(ws->fd, ws->recv_buf + ws->recv_len,
                        WS_RECV_BUF_SIZE - ws->recv_len);
      if (n < 0) {
        if (sock_would_block()) return WS_EVENT_NONE;
        ws->state = WS_STATE_ERROR;
        if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "recv failed during upgrade", ws->callbacks.userdata);
        return WS_EVENT_ERROR;
      }
      if (n == 0) {
        ws->state = WS_STATE_CLOSED;
        if (ws->callbacks.on_close) ws->callbacks.on_close(ws, 1006, "", 0, ws->callbacks.userdata);
        return WS_EVENT_CLOSE;
      }
      ws->recv_len += (size_t)n;

      size_t header_len = 0;
      int up = parse_upgrade_response(ws->recv_buf, ws->recv_len, &header_len, ws->key);
      if (up < 0) {
        ws->state = WS_STATE_ERROR;
        if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "bad HTTP response during upgrade", ws->callbacks.userdata);
        return WS_EVENT_ERROR;
      }
      if (up == 0) return WS_EVENT_NONE; /* wait for more data */

      /* Upgrade done! Keep anything received past the header block --
       * a server that writes its first WS frame right after the 101
       * response can land it in the same recv(). */
      ws->state = WS_STATE_OPEN;
      if (header_len < ws->recv_len) {
        memmove(ws->recv_buf, ws->recv_buf + header_len, ws->recv_len - header_len);
        ws->recv_len -= header_len;
      } else {
        ws->recv_len = 0;
      }
      if (ws->callbacks.on_open) ws->callbacks.on_open(ws, ws->callbacks.userdata);
      return WS_EVENT_OPEN;
    }

    /* ---- OPEN: normal data exchange ---- */
    case WS_STATE_OPEN: {
      if (ws->recv_len >= WS_RECV_BUF_SIZE) {
        /* Buffer is full and a previous pass still couldn't extract a
         * complete frame from it -- the message plainly doesn't fit
         * in WS_RECV_BUF_SIZE. Previously this fell through to
         * recv(fd, buf, 0), which returns 0 and was misread as "peer
         * closed the connection", tearing down a perfectly healthy
         * link. Report it for what it is instead. */
        ws->state = WS_STATE_ERROR;
        if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "message exceeds receive buffer size", ws->callbacks.userdata);
        return WS_EVENT_ERROR;
      }

      int n = sock_recv(ws->fd, ws->recv_buf + ws->recv_len,
                        WS_RECV_BUF_SIZE - ws->recv_len);
      if (n < 0) {
        if (!sock_would_block()) {
          ws->state = WS_STATE_ERROR;
          if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "recv failed", ws->callbacks.userdata);
          return WS_EVENT_ERROR;
        }
        n = 0; /* no new data this round -- still parse whatever's already buffered below */
      } else if (n == 0) {
        ws->state = WS_STATE_CLOSED;
        if (ws->callbacks.on_close) ws->callbacks.on_close(ws, 1006, "", 0, ws->callbacks.userdata);
        return WS_EVENT_CLOSE;
      } else {
        ws->recv_len += (size_t)n;
      }

      /* Frame parsing (RFC 6455), including FIN + continuation frames.
       * `delivered` tracks whether on_message actually fired at least
       * once this call -- previously the return value ignored this
       * entirely and always reported WS_EVENT_MESSAGE. */
      int delivered = 0;
      size_t consumed = 0;
      while (ws->recv_len - consumed >= 2) {
        unsigned char *p = (unsigned char *)ws->recv_buf + consumed;
        if (p[0] & 0x70) {
          ws->state = WS_STATE_ERROR;
          if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "non-zero RSV bits", ws->callbacks.userdata);
          return WS_EVENT_ERROR;
        }
        int fin = (p[0] & 0x80) ? 1 : 0;
        unsigned char opcode = p[0] & 0x0f;
        int masked = (p[1] & 0x80) ? 1 : 0;
        uint64_t payload_len = p[1] & 0x7f;
        size_t header_len = 2;

        if (payload_len == 126) {
          if (ws->recv_len - consumed < 4) break;
          payload_len = ((uint64_t)p[2] << 8) | p[3];
          header_len = 4;
        } else if (payload_len == 127) {
          if (ws->recv_len - consumed < 10) break;
          payload_len = 0;
          for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | p[2 + i];
          header_len = 10;
        }

        if (masked) header_len += 4;
        if (ws->recv_len - consumed < header_len + payload_len) break;

        unsigned char *payload = p + header_len;
        if (masked) {
          unsigned char *mask = p + header_len - 4;
          for (uint64_t i = 0; i < payload_len; i++)
            payload[i] ^= mask[i & 3];
        }

        /* Control frame validation: MUST NOT be fragmented, payload ≤ 125 */
        if (opcode >= 0x8) {
          if (!fin) {
            unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
            ws_send_control(ws, 0x88, fail, 2);
            ws->state = WS_STATE_ERROR;
            if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "fragmented control frame", ws->callbacks.userdata);
            return WS_EVENT_ERROR;
          }
          if (payload_len > 125) {
            unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
            ws_send_control(ws, 0x88, fail, 2);
            ws->state = WS_STATE_ERROR;
            if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "control frame payload too large", ws->callbacks.userdata);
            return WS_EVENT_ERROR;
          }
        }

        if (opcode == 0x8) {
          /* Validate close frame */
          int valid = 1;
          uint16_t code = 1005;
          const char *reason = "";
          size_t reason_len = 0;
          if (payload_len >= 2) {
            code = (uint16_t)((unsigned)payload[0] << 8 | payload[1]);
            if (payload_len > 2) { reason = (const char *)payload + 2; reason_len = (size_t)(payload_len - 2);
              if (!ws_utf8_valid((const unsigned char *)reason, reason_len)) valid = 0; }
            /* RFC 6455 + RFC 8441: valid close codes are 1000-1003, 1007-1011, 1014, 3000-4999 */
            if (code < 1000 || code > 4999 ||
                (code >= 1004 && code <= 1006) ||
                code == 1015 ||
                (code >= 1016 && code <= 2999))
              valid = 0;
          } else if (payload_len > 0) {
            valid = 0; /* body present but < 2 bytes — invalid */
          }
          if (!valid) {
            unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
            ws_send_control(ws, 0x88, fail, 2);
            ws->state = WS_STATE_ERROR;
            if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "invalid close frame", ws->callbacks.userdata);
            return WS_EVENT_ERROR;
          }
          unsigned char close_payload[2] = {(unsigned char)(code >> 8), (unsigned char)(code)};
          ws_send_control(ws, 0x88, close_payload, 2);
          ws->state = WS_STATE_CLOSING;
          ws->closing_initiated = 0;
          if (ws->callbacks.on_close) ws->callbacks.on_close(ws, code, reason, reason_len, ws->callbacks.userdata);
          return WS_EVENT_CLOSE;
        } else if (opcode == 0x9) {
          ws_send_control(ws, 0x8a, payload, (size_t)payload_len);
        } else if (opcode == 0xa) {
          /* Pong — ignore */
        } else if ((opcode >= 0x3 && opcode <= 0x7) || (opcode >= 0xb && opcode <= 0xf)) {
          unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
          ws_send_control(ws, 0x88, fail, 2);
          ws->state = WS_STATE_ERROR;
          if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "reserved opcode", ws->callbacks.userdata);
          return WS_EVENT_ERROR;
        } else if (opcode == 0x0) {
          /* Continuation frame: must belong to an active fragmented message */
          if (!ws->frag_active) {
            ws->state = WS_STATE_ERROR;
            if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "unexpected continuation frame", ws->callbacks.userdata);
            return WS_EVENT_ERROR;
          }
          if (frag_append(ws, payload, (size_t)payload_len) != 0) {
            ws->state = WS_STATE_ERROR;
            if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "out of memory reassembling fragmented message", ws->callbacks.userdata);
            return WS_EVENT_ERROR;
          }
          if (fin) {
            if (!ws->frag_binary && !ws_utf8_valid(ws->frag_buf, ws->frag_len)) {
              unsigned char fail[2] = {0x03, 0xef}; /* 1007 */
              ws_send_control(ws, 0x88, fail, 2);
              ws->state = WS_STATE_ERROR;
              if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "invalid UTF-8 in text message", ws->callbacks.userdata);
              return WS_EVENT_ERROR;
            }
            if (ws->callbacks.on_message)
              ws->callbacks.on_message(ws, (const char *)ws->frag_buf, ws->frag_len, ws->frag_binary, ws->callbacks.userdata);
            delivered = 1;
            ws->frag_active = 0;
            ws->frag_len = 0;
          }
        } else if (opcode == 0x1 || opcode == 0x2) {
          /* If inside a fragmented message, non-continuation frames are a protocol violation */
          if (ws->frag_active) {
            ws->state = WS_STATE_ERROR;
            if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "expected continuation frame", ws->callbacks.userdata);
            return WS_EVENT_ERROR;
          }
          if (!fin) {
            /* First fragment of a fragmented message -- start buffering. */
            ws->frag_active = 1;
            ws->frag_binary = (opcode == 0x2);
            ws->frag_len = 0;
            if (frag_append(ws, payload, (size_t)payload_len) != 0) {
              ws->state = WS_STATE_ERROR;
              if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "out of memory reassembling fragmented message", ws->callbacks.userdata);
              return WS_EVENT_ERROR;
            }
          } else if (ws->callbacks.on_message) {
            int binary = (opcode == 0x2);
            if (!binary && !ws_utf8_valid(payload, (size_t)payload_len)) {
              unsigned char fail[2] = {0x03, 0xef}; /* 1007 */
              ws_send_control(ws, 0x88, fail, 2);
              ws->state = WS_STATE_ERROR;
              if (ws->callbacks.on_error) ws->callbacks.on_error(ws, "invalid UTF-8 in text message", ws->callbacks.userdata);
              return WS_EVENT_ERROR;
            }
            ws->callbacks.on_message(ws, (const char *)payload, (size_t)payload_len, binary, ws->callbacks.userdata);
            delivered = 1;
          }
        }

        consumed += header_len + (size_t)payload_len;
      }

      if (consumed > 0 && consumed < ws->recv_len) {
        memmove(ws->recv_buf, ws->recv_buf + consumed, ws->recv_len - consumed);
        ws->recv_len -= consumed;
      } else if (consumed >= ws->recv_len) {
        ws->recv_len = 0;
      }

      return delivered ? WS_EVENT_MESSAGE : WS_EVENT_NONE;
    }

    /* ---- CLOSING: wait for peer's Close frame, then close TCP ---- */
    case WS_STATE_CLOSING: {
      if (ws->fd == INVALID_SOCKET) { ws->state = WS_STATE_CLOSED; return WS_EVENT_CLOSE; }
      /* If we initiated the close, wait a bit for the server's Close frame */
      if (ws->closing_initiated) {
        int n = sock_recv(ws->fd, ws->recv_buf, WS_RECV_BUF_SIZE);
        if (n > 0) {
          /* Got some data — might be a Close frame, but we don't care at this point */
        } else if (n == 0 || (n < 0 && !sock_would_block())) {
          /* Connection already closed or error */
        }
      }
      sock_close(ws->fd);
      ws->fd = INVALID_SOCKET;
      ws->state = WS_STATE_CLOSED;
      if (!ws->closing_initiated) {
        /* Server initiated: on_close already called when we received the Close frame */
      } else if (ws->callbacks.on_close) {
        ws->callbacks.on_close(ws, 1000, "", 0, ws->callbacks.userdata);
      }
      return WS_EVENT_CLOSE;
    }

    case WS_STATE_CLOSED:
    case WS_STATE_ERROR:
      return WS_EVENT_CLOSE;

    default:
      return WS_EVENT_NONE;
  }
}

void ws_close(ws_t *ws) {
  if (!ws) return;
  if (ws->state == WS_STATE_OPEN) {
    unsigned char close_payload[2] = {0x03, 0xe8}; /* code 1000 */
    ws_send_control(ws, 0x88, close_payload, 2);
    ws->state = WS_STATE_CLOSING;
    ws->closing_initiated = 1;
  } else {
    ws->state = WS_STATE_CLOSED;
    if (ws->fd != INVALID_SOCKET) sock_close(ws->fd);
    ws->fd = INVALID_SOCKET;
  }
}

void ws_destroy(ws_t *ws) {
  if (!ws) return;
  ws_close(ws);
  free(ws->host);
  free(ws->path);
  free(ws->frag_buf);
  free(ws);
  sock_cleanup();
}

ws_state_e ws_state(ws_t *ws) {
  return ws ? ws->state : WS_STATE_ERROR;
}
