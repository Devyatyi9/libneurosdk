#include "ws_client.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  Platform detection                                                 */
/* ================================================================== */
#if defined(_WIN32)
#define WS_PLATFORM_WIN 1
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 5105)
#endif
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
// clang-format on
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
/* MSVC deprecates POSIX names; map to ISO C equivalents */
#define strncasecmp _strnicmp
#define strdup _strdup
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define WS_PLATFORM_POSIX 1
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (-1)
typedef int SOCKET;
#else
#error "Unsupported platform"
#endif

/*
 * Fallthrough annotation — MSVC doesn't support __attribute__ for C,
 * GCC 7+ and Clang do. C23 [[fallthrough]] is not yet universally
 * available in MSVC's C mode, so we stick with the attribute syntax
 * guarded to a no-op on Windows compilers that lack it.
 */
#if defined(__GNUC__) || defined(__clang__)
#define WS_FALLTHROUGH __attribute__((fallthrough))
#else
#define WS_FALLTHROUGH (void)0
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

	/* HTTP CONNECT proxy (optional). proxy_host != NULL when proxying. */
	char *proxy_host;
	uint16_t proxy_port;
	char *proxy_auth; /* base64 "user:pass", or NULL */

	/* Resolved address list (getaddrinfo, AF_UNSPEC) + current attempt.
	 * Freed via freeaddrinfo() when the connection opens or is destroyed. */
	struct addrinfo *ai_list;
	struct addrinfo *ai_cur;

	/* Proxy tunnel state */
	int proxy_tunnel_sent; /* CONNECT request already sent */
	int proxy_tunnel_done; /* 200 Connection Established received */

	/* Upgrade response state */
	int upgrade_done;
	char key[64]; /* Sec-WebSocket-Key sent during upgrade */

	/* Fragmented-message reassembly (FIN=0 ... continuation ... FIN=1) */
	int frag_active;
	int frag_binary;
	unsigned char *frag_buf;
	size_t frag_len;
	size_t frag_cap;

	/* Large single-frame streaming (OPEN state). When a data frame's
	 * payload is too big to fit in the fixed recv_buf, the bytes are
	 * accumulated here (into frag_buf) across ws_poll() calls instead
	 * of erroring out with "message exceeds receive buffer size". */
	int stream_active; /* currently streaming a large frame */
	int stream_fin;    /* FIN bit of the streamed frame */
	unsigned char stream_opcode;
	int stream_masked; /* frame carried a client mask (defensive) */
	unsigned char stream_mask[4];
	uint64_t stream_payload_len;   /* total payload length of the frame */
	uint64_t stream_payload_recvd; /* payload bytes received so far */
	uint64_t stream_remaining;     /* payload bytes still to receive */

	/* Close handshake state */
	int closing_initiated; /* non-zero if WE initiated the close */
	uint16_t peer_close_code;
	char peer_close_reason[123];
	size_t peer_close_reason_len;
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
	if (--sock_refcount == 0)
		WSACleanup();
}
#else
static int sock_init(void) {
	/* On POSIX, writing to a peer that already closed the connection
	 * raises SIGPIPE (default action: terminate the process) instead of
	 * making send() return an error. A WS client must survive a peer that
	 * dropped mid-send — suppress the signal so EPIPE surfaces through
	 * send()'s return value instead. */
	signal(SIGPIPE, SIG_IGN);
	return 0;
}
static void sock_cleanup(void) { }
#endif

static SOCKET sock_create(int family) {
	SOCKET s = socket(family, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
		return INVALID_SOCKET;
	/* Allow reusing local ports still in TIME_WAIT — critical on Windows
	 * when many connect/close cycles exhaust the ephemeral port range.
	 * Without this, after ~400 rapid iterations connect() never completes
	 * and the loop hangs until the internal timeout fires (10s). */
	int reuse = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char const *)&reuse, sizeof(reuse));
	return s;
}

static void sock_close(SOCKET s) {
	if (s == INVALID_SOCKET)
		return;
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
	if (flags == -1)
		return -1;
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

static int sock_connect_nonblock(SOCKET s,
                                 const struct sockaddr *addr,
                                 socklen_t addrlen) {
	int rc = connect(s, addr, addrlen);
	if (rc == 0)
		return 0; /* immediate connect */
#if defined(WS_PLATFORM_WIN)
	if (WSAGetLastError() == WSAEWOULDBLOCK)
		return 0;
#else
	if (errno == EINPROGRESS)
		return 0;
#endif
	return -1;
}

#if defined(WS_PLATFORM_WIN)
static int sock_poll_writable(SOCKET s, int timeout_ms) {
	struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	fd_set wfds;
	FD_ZERO(&wfds);
	FD_SET(s, &wfds);
	fd_set efds;
	FD_ZERO(&efds);
	FD_SET(s, &efds);
	int rc =
	    select((int)(s + 1), NULL, &wfds, &efds, timeout_ms < 0 ? NULL : &tv);
	if (rc <= 0)
		return rc;
	if (FD_ISSET(s, &efds))
		return -1;
	int err = 0;
	socklen_t errlen = sizeof(err);
	if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) != 0 ||
	    err != 0)
		return -1;
	return 1;
}
#else
static int sock_poll_writable(SOCKET s, int timeout_ms) {
	struct pollfd pfd = {s, POLLOUT, 0};
	int rc = poll(&pfd, 1, timeout_ms);
	if (rc <= 0)
		return rc;
	if (pfd.revents & POLLERR)
		return -1;
	int err = 0;
	socklen_t errlen = sizeof(err);
	if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0)
		return -1;
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

static int sock_send(SOCKET s, void const *buf, size_t len) {
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
static int sock_send_all(SOCKET s, void const *buf, size_t len) {
	unsigned char const *p = (unsigned char const *)buf;
	size_t sent = 0;
	while (sent < len) {
		int rc = sock_send(s, p + sent, len - sent);
		if (rc > 0) {
			sent += (size_t)rc;
			continue;
		}
		if (rc < 0 && sock_would_block()) {
			int wr = sock_poll_writable(s, WS_SEND_TIMEOUT_MS);
			if (wr <= 0)
				return -1; /* timeout or error while waiting to write */
			continue;
		}
		return -1; /* hard error, or rc == 0 which shouldn't happen for len > 0 */
	}
	return (int)sent;
}

/* Resolve host+port into an addrinfo list (AF_UNSPEC → IPv4 and/or
 * IPv6).  Returns 0 on success (res != NULL), -1 on failure. */
static int sock_resolve(char const *host,
                        uint16_t port,
                        struct addrinfo **out) {
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%u", port);
	int rc = getaddrinfo(host, port_str, &hints, out);
	if (rc != 0 || *out == NULL)
		return -1;
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
	for (size_t i = 0; i < len; i++)
		buf[i] = (unsigned char)(rand() & 0xff);
#endif
}

/* ================================================================== */
/*  URL parsing (minimal, only ws://)                                  */
/* ================================================================== */
static int parse_url(char const *url,
                     char **host,
                     uint16_t *port,
                     char **path) {
	*host = NULL;
	*path = NULL;
	/* Expect: ws://host[:port][/path] or ws://[v6addr][:port][/path] */
	if (strncmp(url, "ws://", 5) != 0)
		return -1;
	char const *p = url + 5;
	char const *host_start;
	size_t host_len;

	if (*p == '[') {
		/* Bracketed IPv6 literal: [::1][:port][/path] */
		p++;
		host_start = p;
		while (*p && *p != ']')
			p++;
		if (*p != ']')
			return -1; /* unterminated '[' */
		host_len = (size_t)(p - host_start);
		p++; /* consume ']' */
	} else {
		host_start = p;
		while (*p && *p != ':' && *p != '/')
			p++;
		host_len = (size_t)(p - host_start);
	}
	if (host_len == 0)
		return -1;

	*host = (char *)malloc(host_len + 1);
	if (!*host)
		return -1;
	memcpy(*host, host_start, host_len);
	(*host)[host_len] = '\0';

	if (*p == ':') {
		p++;
		char *end;
		long pn = strtol(p, &end, 10);
		if (end == p || pn <= 0 || pn > 65535) {
			free(*host);
			*host = NULL;
			return -1;
		}
		*port = (uint16_t)pn;
		p = end;
	} else {
		*port = 80;
	}

	if (*p == '/') {
		*path = strdup(p);
		if (!*path) {
			free(*host);
			*host = NULL;
			return -1;
		}
	} else {
		*path = strdup("/");
		if (!*path) {
			free(*host);
			*host = NULL;
			return -1;
		}
	}

	return 0;
}

/* ================================================================== */
/*  Forward declarations                                               */
/* ================================================================== */
static void base64_encode(unsigned char const *in,
                          size_t inlen,
                          char *out,
                          size_t outsize);

/* ================================================================== */
/*  HTTP upgrade                                                        */
/* ================================================================== */
static int build_upgrade_request(char *buf,
                                 size_t size,
                                 char const *host,
                                 uint16_t port,
                                 char const *path,
                                 char const *key) {
	/* RFC 3986 §3.2.2: IPv6 literals in the Host header MUST be bracketed.
	 * A colon in the host means it's a literal v6 address (hostnames can't
	 * contain ':'). */
	int is_ipv6 = strchr(host, ':') != NULL;
	return snprintf(buf, size,
	                "GET %s HTTP/1.1\r\n"
	                "Host: %s%s%s:%u\r\n"
	                "Upgrade: websocket\r\n"
	                "Connection: Upgrade\r\n"
	                "Sec-WebSocket-Version: 13\r\n"
	                "Sec-WebSocket-Key: %s\r\n"
	                "Origin: http://local.neuro-integration\r\n"
	                "\r\n",
	                path, is_ipv6 ? "[" : "", host, is_ipv6 ? "]" : "", port,
	                key);
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
		e = d;
		d = c;
		c = ROTL32(b, 30);
		b = a;
		a = tmp;
	}
	for (int i = 20; i < 40; i++) {
		uint32_t tmp = ROTL32(a, 5) + (b ^ c ^ d) + e + w[i] + 0x6ED9EBA1;
		e = d;
		d = c;
		c = ROTL32(b, 30);
		b = a;
		a = tmp;
	}
	for (int i = 40; i < 60; i++) {
		uint32_t tmp =
		    ROTL32(a, 5) + ((b & c) | (d & (b | c))) + e + w[i] + 0x8F1BBCDC;
		e = d;
		d = c;
		c = ROTL32(b, 30);
		b = a;
		a = tmp;
	}
	for (int i = 60; i < 80; i++) {
		uint32_t tmp = ROTL32(a, 5) + (b ^ c ^ d) + e + w[i] + 0xCA62C1D6;
		e = d;
		d = c;
		c = ROTL32(b, 30);
		b = a;
		a = tmp;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

static void sha1_update(struct sha1_ctx *ctx,
                        unsigned char const *data,
                        size_t len) {
	size_t idx = (size_t)(ctx->count & 63);
	ctx->count += len;

	if (idx) {
		size_t fill = 64 - idx;
		if (len < fill) {
			memcpy(ctx->buffer + idx, data, len);
			return;
		}
		memcpy(ctx->buffer + idx, data, fill);
		sha1_transform(ctx->state, ctx->buffer);
		data += fill;
		len -= fill;
	}

	while (len >= 64) {
		sha1_transform(ctx->state, data);
		data += 64;
		len -= 64;
	}

	if (len)
		memcpy(ctx->buffer, data, len);
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
		out[i * 4] = (unsigned char)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (unsigned char)(ctx->state[i]);
	}
}

/* ================================================================== */
/*  Base64 encode (RFC 4648 §4) — used for key gen + accept check      */
/* ================================================================== */
static void base64_encode(unsigned char const *in,
                          size_t inlen,
                          char *out,
                          size_t outsize) {
	static char const b64[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i = 0, o = 0;
	while (i < inlen && o + 4 < outsize) {
		unsigned long v = ((unsigned long)in[i]) << 16;
		if (i + 1 < inlen)
			v |= ((unsigned long)in[i + 1]) << 8;
		if (i + 2 < inlen)
			v |= (unsigned long)in[i + 2];
		out[o++] = b64[(v >> 18) & 0x3f];
		out[o++] = b64[(v >> 12) & 0x3f];
		out[o++] = (i + 1 < inlen) ? b64[(v >> 6) & 0x3f] : '=';
		out[o++] = (i + 2 < inlen) ? b64[v & 0x3f] : '=';
		i += 3;
	}
	if (o < outsize)
		out[o] = '\0';
}

/* ================================================================== */
/*  HTTP CONNECT proxy                                                 */
/* ================================================================== */

/* Parse a proxy string: "host:port", "http://host:port",
 * "http://user:pass@host:port". Scheme (http://) is optional but only
 * http is accepted (CONNECT works over plain TCP). Credentials, if any,
 * are base64-encoded into *auth_out (may be NULL). */
static int parse_proxy_url(char const *proxy,
                           char **host,
                           uint16_t *port,
                           char **auth) {
	*host = NULL;
	*auth = NULL;
	*port = 8080; /* common HTTP proxy default */

	char const *p = proxy;
	if (strncmp(p, "http://", 7) == 0)
		p += 7;
	else if (strncmp(p, "https://", 8) == 0)
		return -1; /* no TLS to proxy */

	/* user:pass@host:port */
	char const *at = strrchr(p, '@');
	if (at) {
		size_t ulen = (size_t)(at - p);
		if (ulen == 0 || ulen >= 256)
			return -1;
		char userpass[256];
		memcpy(userpass, p, ulen);
		userpass[ulen] = '\0';
		char b64[512];
		base64_encode((unsigned char const *)userpass, ulen, b64, sizeof(b64));
		*auth = (char *)malloc(strlen(b64) + 1);
		if (!*auth)
			return -1;
		strcpy(*auth, b64);
		p = at + 1;
	}

	/* host[:port] -- hostname or bracketed IPv6 */
	char const *host_start;
	size_t host_len;
	if (*p == '[') {
		p++;
		host_start = p;
		while (*p && *p != ']')
			p++;
		if (*p != ']')
			return -1;
		host_len = (size_t)(p - host_start);
		p++;
	} else {
		host_start = p;
		while (*p && *p != ':' && *p != '/')
			p++;
		host_len = (size_t)(p - host_start);
	}
	if (host_len == 0)
		return -1;

	*host = (char *)malloc(host_len + 1);
	if (!*host) {
		free(*auth);
		*auth = NULL;
		return -1;
	}
	memcpy(*host, host_start, host_len);
	(*host)[host_len] = '\0';

	if (*p == ':') {
		p++;
		char *end;
		long pn = strtol(p, &end, 10);
		if (end == p || pn <= 0 || pn > 65535) {
			free(*host);
			*host = NULL;
			free(*auth);
			*auth = NULL;
			return -1;
		}
		*port = (uint16_t)pn;
	}

	return 0;
}

/* Build a CONNECT request for an HTTP proxy (RFC 7231 §4.3.6).
 * host/port are the WS target; auth is base64 "user:pass" or NULL. */
static int build_connect_request(char *buf,
                                 size_t size,
                                 char const *host,
                                 uint16_t port,
                                 char const *auth) {
	int is_ipv6 = strchr(host, ':') != NULL;
	char const *h = is_ipv6 ? "[" : "";
	char const *hc = is_ipv6 ? "]" : "";
	if (auth) {
		return snprintf(buf, size,
		                "CONNECT %s%s%s:%u HTTP/1.1\r\n"
		                "Host: %s%s%s:%u\r\n"
		                "Proxy-Authorization: Basic %s\r\n"
		                "\r\n",
		                h, host, hc, port, h, host, hc, port, auth);
	}
	return snprintf(buf, size,
	                "CONNECT %s%s%s:%u HTTP/1.1\r\n"
	                "Host: %s%s%s:%u\r\n"
	                "\r\n",
	                h, host, hc, port, h, host, hc, port);
}

/* Look for a "200 Connection Established" response. Returns:
 *   1 = complete 200 received (*header_len set), 0 = need more data,
 *  -1 = non-200 / not an HTTP response. */
static int parse_proxy_response(char const *buf,
                                size_t len,
                                size_t *header_len) {
	if (len >= 4 && memcmp(buf, "HTTP", 4) != 0)
		return -1;
	if (len >= 12 && memcmp(buf, "HTTP/1.1 101", 12) == 0)
		return -1; /* misrouted */

	char const *end = NULL;
	for (size_t i = 0; i + 4 <= len; i++) {
		if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
			end = buf + i + 4;
			break;
		}
	}
	if (!end)
		return 0; /* headers not fully received yet */

	if (len < 12 || memcmp(buf, "HTTP/1.1 200", 12) != 0)
		return -1;
	*header_len = (size_t)(end - buf);
	return 1;
}

/* Check whether `host` is exempted by NO_PROXY (comma-separated entries,
 * entries may be "*", "host", "host:port", ".domain", "domain"). */
static int no_proxy_match(char const *no_proxy, char const *host) {
	size_t host_len = strlen(host);
	char const *entry = no_proxy;
	while (*entry) {
		while (*entry == ',' || *entry == ' ' || *entry == '\t')
			entry++;
		if (!*entry)
			break;
		char const *start = entry;
		while (*entry && *entry != ',' && *entry != ' ')
			entry++;
		size_t e_len = (size_t)(entry - start);

		/* strip optional ":port" suffix */
		size_t port_at = e_len;
		for (size_t i = 0; i < e_len; i++) {
			if (start[i] == ':') {
				port_at = i;
				break;
			}
		}
		if (port_at == 0)
			continue;
		if (port_at == 1 && start[0] == '*')
			return 1;

		if (start[0] == '.') {
			/* ".domain" or ".domain:port" matches host suffix */
			char const *d = start + 1;
			size_t d_len = port_at - 1;
			if (host_len == d_len && strncasecmp(host, d, d_len) == 0)
				return 1;
			if (host_len > d_len &&
			    strncasecmp(host + host_len - d_len, d, d_len) == 0 &&
			    host[host_len - d_len - 1] == '.')
				return 1;
		} else {
			if (host_len == port_at && strncasecmp(host, start, port_at) == 0)
				return 1;
			/* bare "domain" also matches subdomains, like curl */
			if (host_len > port_at &&
			    strncasecmp(host + host_len - port_at, start, port_at) == 0 &&
			    host[host_len - port_at - 1] == '.')
				return 1;
		}
		entry += (entry[0] == ' ' || entry[0] == '\t' || entry[0] == ',') ? 1 : 0;
	}
	return 0;
}

/* ================================================================== */
/*  HTTP upgrade response parser                                        */
/* ================================================================== */

/* Looks for the end of the HTTP header block ("\r\n\r\n") before
 * declaring the upgrade done, reports header length, AND verifies the
 * Sec-WebSocket-Accept header (RFC 6455 §4.2.2) against our key. */
static int parse_upgrade_response(char const *buf,
                                  size_t len,
                                  size_t *header_len_out,
                                  char const *expected_key) {
	if (len >= 12 && memcmp(buf, "HTTP/1.1 ", 9) == 0 &&
	    memcmp(buf, "HTTP/1.1 101", 12) != 0) {
		return -1; /* non-101 status, fail fast even before full headers arrive */
	}
	if (len >= 4 && memcmp(buf, "HTTP", 4) != 0) {
		return -1; /* not an HTTP response at all */
	}

	char const *end = NULL;
	for (size_t i = 0; i + 4 <= len; i++) {
		if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
			end = buf + i + 4;
			break;
		}
	}
	if (!end)
		return 0; /* headers not fully received yet */

	if (len < 12 || memcmp(buf, "HTTP/1.1 101", 12) != 0)
		return -1;
	*header_len_out = (size_t)(end - buf);

	/* Verify Upgrade: websocket and Connection: Upgrade (§4.1 val.2-3) */
	int has_upgrade = 0, has_conn = 0;
	for (size_t i = 9; i + 11 < *header_len_out; i++) {
		if (strncasecmp(buf + i, "upgrade:", 8) == 0) {
			char const *v = buf + i + 8;
			while (*v == ' ' || *v == '\t')
				v++;
			if (strncasecmp(v, "websocket", 9) == 0)
				has_upgrade = 1;
		} else if (strncasecmp(buf + i, "connection:", 11) == 0) {
			char const *v = buf + i + 11;
			while (*v == ' ' || *v == '\t')
				v++;
			/* Check for "Upgrade" token (possibly among others) */
			while (*v && *v != '\r' && *v != '\n') {
				while (*v == ' ' || *v == '\t' || *v == ',')
					v++;
				if (strncasecmp(v, "Upgrade", 7) == 0) {
					has_conn = 1;
					break;
				}
				while (*v && *v != ',' && *v != '\r' && *v != '\n')
					v++;
			}
		}
	}
	if (!has_upgrade || !has_conn)
		return -1;

	/* Verify Sec-WebSocket-Accept (RFC 6455 §4.2.2) */
	if (expected_key) {
		/* Find the Sec-WebSocket-Accept header value */
		char const *accept_hdr = NULL;
		for (size_t i = 9; i + 22 < *header_len_out; i++) {
			if (strncasecmp(buf + i, "Sec-WebSocket-Accept:", 21) == 0) {
				accept_hdr = buf + i + 21;
				while (*accept_hdr == ' ' || *accept_hdr == '\t')
					accept_hdr++;
				break;
			}
		}
		if (!accept_hdr)
			return -1;

		/* Compute expected accept: Base64(SHA-1(key + magic GUID)) */
		static char const magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		struct sha1_ctx ctx;
		sha1_init(&ctx);
		sha1_update(&ctx, (unsigned char const *)expected_key,
		            strlen(expected_key));
		sha1_update(&ctx, (unsigned char const *)magic, strlen(magic));
		unsigned char hash[20];
		sha1_final(&ctx, hash);

		char expected[64];
		base64_encode(hash, 20, expected, sizeof(expected));

		/* Compare with server's value (up to the end of the line or header
		 * boundary — \r\n, \n, or end of buffer) */
		size_t accept_len = 0;
		while (accept_hdr[accept_len] && accept_hdr[accept_len] != '\r' &&
		       accept_hdr[accept_len] != '\n')
			accept_len++;
		if (accept_len != strlen(expected) ||
		    memcmp(accept_hdr, expected, accept_len) != 0)
			return -1;
	}

	return 1;
}

/* ================================================================== */
/*  Fragmented-message reassembly buffer                               */
/* ================================================================== */
static int frag_append(ws_t *ws, unsigned char const *data, size_t len) {
	if (len > SIZE_MAX - ws->frag_len)
		return -1;
	size_t required = ws->frag_len + len;
	if (required > ws->frag_cap) {
		size_t newcap = ws->frag_cap ? ws->frag_cap : 4096;
		while (newcap < required) {
			if (newcap > SIZE_MAX / 2) {
				newcap = required;
				break;
			}
			newcap *= 2;
		}
		unsigned char *nb = (unsigned char *)realloc(ws->frag_buf, newcap);
		if (!nb)
			return -1;
		ws->frag_buf = nb;
		ws->frag_cap = newcap;
	}
	if (len)
		memcpy(ws->frag_buf + ws->frag_len, data, len);
	ws->frag_len += len;
	return 0;
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

int ws_connect(ws_t **out, char const *url, ws_callbacks_t callbacks) {
	return ws_connect_via_proxy(out, url, callbacks, NULL);
}

int ws_connect_via_proxy(ws_t **out,
                         char const *url,
                         ws_callbacks_t callbacks,
                         char const *proxy) {
	if (!out || !url)
		return -1;

	ws_t *ws = (ws_t *)calloc(1, sizeof(ws_t));
	if (!ws)
		return -1;

	ws->state = WS_STATE_INIT;
	ws->fd = INVALID_SOCKET;
	ws->callbacks = callbacks;

	if (parse_url(url, &ws->host, &ws->port, &ws->path) != 0) {
		free(ws);
		return -1;
	}

	/* Resolve the proxy: explicit argument wins; else read env
	 * HTTP_PROXY then ALL_PROXY, honouring NO_PROXY. */
	char const *env_px = NULL;
	if (proxy == NULL || *proxy == '\0') {
		char const *np = getenv("NO_PROXY");
		if (!np || !*np)
			np = getenv("no_proxy");
		if (!np || !no_proxy_match(np, ws->host)) {
			env_px = getenv("HTTP_PROXY");
			if (!env_px || !*env_px)
				env_px = getenv("http_proxy");
			if (!env_px || !*env_px)
				env_px = getenv("ALL_PROXY");
			if (!env_px || !*env_px)
				env_px = getenv("all_proxy");
			if (env_px && *env_px == '\0')
				env_px = NULL;
		}
		proxy = env_px;
	}

	if (proxy && *proxy) {
		if (parse_proxy_url(proxy, &ws->proxy_host, &ws->proxy_port,
		                    &ws->proxy_auth) != 0) {
			free(ws->host);
			free(ws->path);
			free(ws);
			return -1;
		}
	}

	if (sock_init() != 0) {
		free(ws->host);
		free(ws->path);
		free(ws->proxy_host);
		free(ws->proxy_auth);
		free(ws);
		return -1;
	}

	ws->state = WS_STATE_CONNECTING;
	*out = ws;
	return 0;
}

/* Apply RFC 6455 client masking: masked[i] = data[i] ^ mask[i & 3]. */
static void ws_apply_mask(unsigned char const *data,
                          size_t len,
                          unsigned char const mask[4],
                          unsigned char *out) {
	for (size_t i = 0; i < len; i++)
		out[i] = (unsigned char)data[i] ^ mask[i & 3];
}

/* Build a WS frame header: FIN + opcode + length, with the MASK bit set.
 * Returns header length (2, 4 or 10). */
static size_t ws_build_frame_header(unsigned char *header,
                                    unsigned char opcode,
                                    int fin,
                                    size_t len) {
	size_t hlen = 2;
	header[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0f);
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
	return hlen;
}

static int ws_send_data_frame(ws_t *ws,
                              unsigned char opcode,
                              char const *data,
                              size_t len) {
	if (!ws || ws->state != WS_STATE_OPEN)
		return -1;
	if (len > SIZE_MAX - 14)
		return -1;

	unsigned char *frame = (unsigned char *)malloc(14 + len);
	if (!frame)
		return -1;
	size_t hlen = ws_build_frame_header(frame, opcode, 1, len);

	unsigned char mask[4];
	ws_random_bytes(mask, 4);
	memcpy(frame + hlen, mask, sizeof(mask));
	ws_apply_mask((unsigned char const *)data, len, mask,
	              frame + hlen + sizeof(mask));

	size_t frame_len = hlen + sizeof(mask) + len;
	int rc = sock_send_all(ws->fd, frame, frame_len);
	free(frame);
	return rc == (int)frame_len ? 0 : -1;
}

int ws_send(ws_t *ws, char const *data, size_t len) {
	return ws_send_data_frame(ws, 0x1, data, len);
}

int ws_send_binary(ws_t *ws, char const *data, size_t len) {
	return ws_send_data_frame(ws, 0x2, data, len);
}

static int ws_utf8_valid(unsigned char const *s, size_t len) {
	size_t i = 0;
	while (i < len) {
		unsigned int cp;
		int n;
		if (s[i] <= 0x7F) {
			i++;
			continue;
		} else if ((s[i] & 0xE0) == 0xC0) {
			cp = s[i] & 0x1F;
			n = 2;
		} else if ((s[i] & 0xF0) == 0xE0) {
			cp = s[i] & 0x0F;
			n = 3;
		} else if ((s[i] & 0xF8) == 0xF0) {
			cp = s[i] & 0x07;
			n = 4;
		} else
			return 0;
		if (i + (size_t)n > len)
			return 0;
		for (int j = 1; j < n; j++) {
			if ((s[i + j] & 0xC0) != 0x80)
				return 0;
			cp = (cp << 6) | (s[i + j] & 0x3F);
		}
		if (n == 2 && cp < 0x80)
			return 0;
		if (n == 3 && cp < 0x800)
			return 0;
		if (n == 4 && cp < 0x10000)
			return 0;
		if (cp > 0x10FFFF)
			return 0;
		if (cp >= 0xD800 && cp <= 0xDFFF)
			return 0;
		i += (size_t)n;
	}
	return 1;
}

/* Helper to build a masked Close or Pong response and send it.
 * Called when we need to reply to a received control frame. */
static void ws_send_control(ws_t *ws,
                            unsigned char opcode,
                            unsigned char const *payload,
                            size_t payload_len) {
	unsigned char frame[131] = {opcode, 0x80};
	unsigned char mask[4];
	if (payload_len > 125)
		payload_len = 125;
	frame[1] |= (unsigned char)payload_len;
	ws_random_bytes(mask, 4);
	memcpy(frame + 2, mask, sizeof(mask));
	ws_apply_mask(payload, payload_len, mask, frame + 6);
	sock_send_all(ws->fd, frame, 6 + payload_len);
}

/* Begin streaming a data frame whose payload is too large to fit in the
 * fixed recv_buf. `p` points at the (complete) frame header in recv_buf,
 * `header_len` bytes long; the bytes after it (up to recv_len) are the
 * partial payload. That partial payload is unmasked (if needed) and copied
 * into the dynamic frag buffer; the rest is appended by the OPEN-state
 * streaming loop as it arrives. Protocol checks mirror the inline frame
 * path so a violation is reported immediately. Returns 0 on success,
 * -1 on protocol error (close frame + on_error already signalled). */
static int ws_start_large_frame(ws_t *ws,
                                unsigned char const *p,
                                size_t header_len,
                                uint64_t payload_len,
                                int fin,
                                unsigned char opcode,
                                int masked) {
	if (opcode >= 0x8) {
		unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
		ws_send_control(ws, 0x88, fail, 2);
		ws->state = WS_STATE_ERROR;
		if (ws->callbacks.on_error)
			ws->callbacks.on_error(ws, "control frame payload too large",
			                       ws->callbacks.userdata);
		return -1;
	}
	if ((opcode >= 0x3 && opcode <= 0x7) || (opcode >= 0xb && opcode <= 0xf)) {
		unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
		ws_send_control(ws, 0x88, fail, 2);
		ws->state = WS_STATE_ERROR;
		if (ws->callbacks.on_error)
			ws->callbacks.on_error(ws, "reserved opcode", ws->callbacks.userdata);
		return -1;
	}
	if (opcode == 0x0 && !ws->frag_active) {
		ws->state = WS_STATE_ERROR;
		if (ws->callbacks.on_error)
			ws->callbacks.on_error(ws, "unexpected continuation frame",
			                       ws->callbacks.userdata);
		return -1;
	}
	if (opcode != 0x0 && ws->frag_active) {
		ws->state = WS_STATE_ERROR;
		if (ws->callbacks.on_error)
			ws->callbacks.on_error(ws, "expected continuation frame",
			                       ws->callbacks.userdata);
		return -1;
	}

	size_t prior_len = opcode == 0x0 ? ws->frag_len : 0;
	if (payload_len > SIZE_MAX || payload_len > SIZE_MAX - prior_len) {
		unsigned char fail[2] = {0x03, 0xf1}; /* 1009 */
		ws_send_control(ws, 0x88, fail, 2);
		ws->state = WS_STATE_ERROR;
		if (ws->callbacks.on_error)
			ws->callbacks.on_error(ws, "message size exceeds addressable memory",
			                       ws->callbacks.userdata);
		return -1;
	}

	ws->stream_active = 1;
	ws->stream_fin = fin;
	ws->stream_opcode = opcode;
	ws->stream_payload_len = payload_len;
	ws->stream_masked = masked;
	if (masked)
		memcpy(ws->stream_mask, p + header_len - 4, 4);

	if (opcode == 0x1 || opcode == 0x2) {
		/* Data frame: start a fresh message in the frag buffer. */
		ws->frag_len = 0;
		if (!fin) {
			ws->frag_active = 1;
			ws->frag_binary = (opcode == 0x2);
		}
	}
	/* opcode 0x0 (continuation): append to the existing fragmented message
	 * -- frag_len already holds the earlier fragments. */

	/* Copy whatever payload bytes are already buffered after the header. */
	size_t off = (size_t)(p - (unsigned char const *)ws->recv_buf) + header_len;
	size_t avail = ws->recv_len > off ? ws->recv_len - off : 0;
	unsigned char const *payload = p + header_len;
	if (masked) {
		unsigned char *q = (unsigned char *)payload;
		for (size_t i = 0; i < avail; i++)
			q[i] ^= ws->stream_mask[i & 3];
	}
	if (avail > 0 && frag_append(ws, payload, avail) != 0) {
		ws->state = WS_STATE_ERROR;
		if (ws->callbacks.on_error)
			ws->callbacks.on_error(ws, "out of memory reassembling large frame",
			                       ws->callbacks.userdata);
		return -1;
	}
	ws->stream_payload_recvd = avail;
	ws->stream_remaining = payload_len - avail;
	return 0;
}

/* Process a fully-received large frame that was streamed into frag_buf.
 * Behaves exactly like the inline frame path for the same opcode: text
 * messages are UTF-8 validated, continuation frames finalize a fragmented
 * message, and on_message fires when a complete message is assembled.
 * Returns 1 if on_message fired, 0 otherwise, -1 on protocol error. */
static int ws_finish_large_frame(ws_t *ws) {
	unsigned char opcode = ws->stream_opcode;
	if (opcode == 0x0) {
		/* Continuation frame completing a fragmented message. */
		if (ws->stream_fin) {
			if (!ws->frag_binary && !ws_utf8_valid(ws->frag_buf, ws->frag_len)) {
				unsigned char fail[2] = {0x03, 0xef}; /* 1007 */
				ws_send_control(ws, 0x88, fail, 2);
				ws->state = WS_STATE_ERROR;
				if (ws->callbacks.on_error)
					ws->callbacks.on_error(ws, "invalid UTF-8 in text message",
					                       ws->callbacks.userdata);
				return -1;
			}
			if (ws->callbacks.on_message)
				ws->callbacks.on_message(ws, (char const *)ws->frag_buf, ws->frag_len,
				                         ws->frag_binary, ws->callbacks.userdata);
			ws->frag_active = 0;
			ws->frag_len = 0;
			return 1;
		}
		return 0;
	}

	/* Data frame (0x1 / 0x2). */
	int binary = (opcode == 0x2);
	if (ws->stream_fin) {
		if (!binary && !ws_utf8_valid(ws->frag_buf, ws->frag_len)) {
			unsigned char fail[2] = {0x03, 0xef}; /* 1007 */
			ws_send_control(ws, 0x88, fail, 2);
			ws->state = WS_STATE_ERROR;
			if (ws->callbacks.on_error)
				ws->callbacks.on_error(ws, "invalid UTF-8 in text message",
				                       ws->callbacks.userdata);
			return -1;
		}
		if (ws->callbacks.on_message)
			ws->callbacks.on_message(ws, (char const *)ws->frag_buf, ws->frag_len,
			                         binary, ws->callbacks.userdata);
		ws->frag_active = 0;
		ws->frag_len = 0;
		return 1;
	}
	/* First fragment (frag_active/frag_binary already set) -- wait for the
	 * continuation frames. */
	return 0;
}

ws_event_e ws_poll(ws_t *ws, int timeout_ms) {
	if (!ws)
		return WS_EVENT_ERROR;

	switch (ws->state) {
		/* ---- CONNECTING: initiate TCP connection ---- */
		case WS_STATE_CONNECTING: {
			if (ws->ai_list == NULL) {
				/* Through a proxy we connect to the proxy, not the WS target;
				 * the target is only used in the CONNECT request later. */
				char const *res_host = ws->proxy_host ? ws->proxy_host : ws->host;
				uint16_t res_port = ws->proxy_host ? ws->proxy_port : ws->port;
				if (sock_resolve(res_host, res_port, &ws->ai_list) != 0 ||
				    ws->ai_list == NULL) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "DNS resolve failed",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				ws->ai_cur = ws->ai_list;
			}

			/* Try addresses in order until one accepts a non-blocking connect.
			 * getaddrinfo returns v6 and v4 interleaved; a v6 address whose
			 * network is down must not prevent falling back to v4. */
			while (ws->ai_cur != NULL) {
				SOCKET s = sock_create(ws->ai_cur->ai_family);
				if (s == INVALID_SOCKET) {
					ws->ai_cur = ws->ai_cur->ai_next;
					continue;
				}
				int no_delay = 1;
				setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char const *)&no_delay,
				           sizeof(no_delay));
				sock_set_nonblock(s);

				if (sock_connect_nonblock(s, ws->ai_cur->ai_addr,
				                          (socklen_t)ws->ai_cur->ai_addrlen) == 0) {
					ws->fd = s;
					ws->state =
					    ws->proxy_host ? WS_STATE_PROXY_TUNNEL : WS_STATE_UPGRADING;
					break; /* fall through to the matching state below */
				}
				sock_close(s); /* refused/unreachable — try next address */
				ws->ai_cur = ws->ai_cur->ai_next;
			}

			if (ws->state != WS_STATE_UPGRADING &&
			    ws->state != WS_STATE_PROXY_TUNNEL) {
				ws->state = WS_STATE_ERROR;
				if (ws->callbacks.on_error)
					ws->callbacks.on_error(ws, "connect() failed for all addresses",
					                       ws->callbacks.userdata);
				return WS_EVENT_ERROR;
			}
			WS_FALLTHROUGH;
		}

		/* ---- PROXY_TUNNEL: send CONNECT, wait for 200 from the proxy ---- */
		case WS_STATE_PROXY_TUNNEL: {
			if (ws->proxy_host && !ws->proxy_tunnel_sent) {
				int rc = sock_poll_connect(ws->fd, timeout_ms);
				if (rc < 0) {
					/* TCP connect to the proxy failed. If the resolved list has
					 * more addresses, fall back to the next one. */
					sock_close(ws->fd);
					ws->fd = INVALID_SOCKET;
					if (ws->ai_cur != NULL)
						ws->ai_cur = ws->ai_cur->ai_next;
					if (ws->ai_cur != NULL) {
						ws->state = WS_STATE_CONNECTING;
						return WS_EVENT_NONE;
					}
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "TCP connect failed",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				if (rc == 0)
					return WS_EVENT_NONE; /* still connecting to proxy */

				char req[512];
				int reqlen = build_connect_request(req, sizeof(req), ws->host, ws->port,
				                                   ws->proxy_auth);
				if (reqlen <= 0 || (size_t)reqlen >= sizeof(req)) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "CONNECT request too large for buffer",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				int sent = sock_send_all(ws->fd, req, (size_t)reqlen);
				if (sent != reqlen) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "failed to send CONNECT request",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				ws->proxy_tunnel_sent = 1;
				/* Fall through to the response-waiting loop so one ws_poll()
				 * can complete the whole tunnel when the proxy replies fast. */
			}

			if (ws->proxy_host && !ws->proxy_tunnel_done) {
				/* Internal loop, same rationale as the UPGRADING loop below:
				 * accumulate a possibly-fragmented proxy response and verify
				 * the 200 status line before moving on to the WS upgrade. */
				int elapsed_ms = 0;
				int poll_interval = 10;
				for (;;) {
					if (ws->recv_len >= WS_RECV_BUF_SIZE) {
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "proxy response too large for buffer",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}

					fd_set rfds;
					FD_ZERO(&rfds);
					FD_SET(ws->fd, &rfds);
					int wait_ms = (timeout_ms < 0) ? poll_interval
					              : (timeout_ms - elapsed_ms < poll_interval)
					                  ? timeout_ms - elapsed_ms
					                  : poll_interval;
					if (wait_ms <= 0)
						return WS_EVENT_NONE;

					struct timeval tv = {wait_ms / 1000, (wait_ms % 1000) * 1000};
					int sel = select((int)(ws->fd + 1), &rfds, NULL, NULL, &tv);
					if (sel < 0) {
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "select failed during proxy CONNECT",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (timeout_ms >= 0)
						elapsed_ms += wait_ms;
					if (sel == 0)
						continue;

					while (ws->recv_len < WS_RECV_BUF_SIZE) {
						int n = sock_recv(ws->fd, ws->recv_buf + ws->recv_len,
						                  WS_RECV_BUF_SIZE - ws->recv_len);
						if (n < 0) {
							if (sock_would_block())
								break;
							ws->state = WS_STATE_ERROR;
							if (ws->callbacks.on_error)
								ws->callbacks.on_error(ws, "recv failed during proxy CONNECT",
								                       ws->callbacks.userdata);
							return WS_EVENT_ERROR;
						}
						if (n == 0) {
							ws->state = WS_STATE_CLOSED;
							if (ws->callbacks.on_close)
								ws->callbacks.on_close(ws, 1006, "", 0, ws->callbacks.userdata);
							return WS_EVENT_CLOSE;
						}
						ws->recv_len += (size_t)n;
					}

					size_t header_len = 0;
					int pr =
					    parse_proxy_response(ws->recv_buf, ws->recv_len, &header_len);
					if (pr < 0) {
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "proxy CONNECT failed (non-200)",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (pr == 1) {
						ws->proxy_tunnel_done = 1;
						/* Keep any bytes past the proxy header; the WS server may
						 * have already started its response through the tunnel. */
						if (header_len < ws->recv_len) {
							memmove(ws->recv_buf, ws->recv_buf + header_len,
							        ws->recv_len - header_len);
							ws->recv_len -= header_len;
						} else {
							ws->recv_len = 0;
						}
						break;
					}
					/* pr == 0: need more data, loop back to select() */
				}
			}

			ws->state = WS_STATE_UPGRADING;
			WS_FALLTHROUGH;
		}

		/* ---- UPGRADING: wait for TCP connect, then send HTTP upgrade ---- */
		case WS_STATE_UPGRADING: {
			if (!ws->upgrade_done) {
				int rc = sock_poll_connect(ws->fd, timeout_ms);
				if (rc < 0) {
					/* TCP connect failed (e.g. SO_ERROR after a v6 connect that
					 * didn't complete). If the resolved list has more addresses,
					 * fall back to the next one instead of giving up. */
					sock_close(ws->fd);
					ws->fd = INVALID_SOCKET;
					if (ws->ai_cur != NULL)
						ws->ai_cur = ws->ai_cur->ai_next;
					if (ws->ai_cur != NULL) {
						ws->state = WS_STATE_CONNECTING;
						return WS_EVENT_NONE;
					}
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "TCP connect failed",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				if (rc == 0)
					return WS_EVENT_NONE; /* still connecting */

				char key[32];
				generate_key(key, sizeof(key));
				memcpy(ws->key, key, sizeof(key));
				ws->key[sizeof(key)] = '\0';

				char req[1024];
				int reqlen = build_upgrade_request(req, sizeof(req), ws->host, ws->port,
				                                   ws->path, key);
				if (reqlen <= 0 || (size_t)reqlen >= sizeof(req)) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "upgrade request too large for buffer",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}

				int sent = sock_send_all(ws->fd, req, (size_t)reqlen);
				if (sent != reqlen) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "failed to send upgrade request",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				ws->upgrade_done = 1;
				/* Fall through into the response-waiting loop below so one
				 * ws_poll() call can complete the entire handshake when the
				 * server responds quickly — the caller doesn't need another
				 * round trip through the event loop. */
			}

			/* Internal loop: the upgrade response MUST arrive before we
			 * transition to OPEN.  There is nothing useful the caller can
			 * do between partial reads of the 101 response, so we keep
			 * driving select/recv/parse here until we either succeed, fail,
			 * or exhaust the timeout budget.
			 *
			 * This is critical with Toxiproxy's `slicer` toxic, which
			 * breaks the TCP stream into 1-byte chunks — without the
			 * internal loop each byte would cost one full ws_poll()
			 * round-trip and the caller's iteration budget (echo_test:
			 * 100 polls × 100ms = 10s) would be exhausted long before
			 * the ~200-byte response is fully assembled. */
			int elapsed_ms = 0;
			int poll_interval = 10; /* small steps so the overall budget

			                           * is a good approximation of real time

			                           * rather than pure iteration count */
			int peer_eof = 0;
			for (;;) {
				if (ws->recv_len >= WS_RECV_BUF_SIZE) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "upgrade response too large for buffer",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}

				/* Wait for readability (or timeout) */
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(ws->fd, &rfds);
				int wait_ms = (timeout_ms < 0) ? poll_interval
				              : (timeout_ms - elapsed_ms < poll_interval)
				                  ? timeout_ms - elapsed_ms
				                  : poll_interval;
				if (wait_ms <= 0)
					return WS_EVENT_NONE; /* caller's timeout exhausted */

				struct timeval tv = {wait_ms / 1000, (wait_ms % 1000) * 1000};
				int sel = select((int)(ws->fd + 1), &rfds, NULL, NULL, &tv);
				if (sel < 0) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "select failed during upgrade",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}

				if (timeout_ms >= 0)
					elapsed_ms += wait_ms;

				if (sel == 0)
					continue; /* no data yet, loop again */

				/* Drain whatever is readable (may be multiple TCP segments
				 * from slicer) */
				while (ws->recv_len < WS_RECV_BUF_SIZE) {
					int n = sock_recv(ws->fd, ws->recv_buf + ws->recv_len,
					                  WS_RECV_BUF_SIZE - ws->recv_len);
					if (n < 0) {
						if (sock_would_block())
							break;
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "recv failed during upgrade",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (n == 0) {
						peer_eof = 1;
						break;
					}
					ws->recv_len += (size_t)n;
				}

				/* See if we have a complete 101 response now */
				size_t header_len = 0;
				int up = parse_upgrade_response(ws->recv_buf, ws->recv_len, &header_len,
				                                ws->key);
				if (up < 0) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "bad HTTP response during upgrade",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				if (up == 1) {
					ws->state = WS_STATE_OPEN;
					/* Address list no longer needed once connected. */
					freeaddrinfo(ws->ai_list);
					ws->ai_list = NULL;
					ws->ai_cur = NULL;
					if (header_len < ws->recv_len) {
						memmove(ws->recv_buf, ws->recv_buf + header_len,
						        ws->recv_len - header_len);
						ws->recv_len -= header_len;
					} else {
						ws->recv_len = 0;
					}
					if (ws->callbacks.on_open)
						ws->callbacks.on_open(ws, ws->callbacks.userdata);
					return WS_EVENT_OPEN;
				}
				if (peer_eof) {
					ws->state = WS_STATE_CLOSED;
					if (ws->callbacks.on_close)
						ws->callbacks.on_close(ws, 1006, "", 0, ws->callbacks.userdata);
					return WS_EVENT_CLOSE;
				}
				/* up == 0: need more data, loop back to select() */
			}
		}

		/* ---- OPEN: normal data exchange ---- */
		case WS_STATE_OPEN: {
			/* If we're mid-stream of a large frame, keep reading its payload
			 * into the frag buffer before doing anything else. Control frames
			 * cannot be interleaved inside a single frame's payload (RFC 6455
			 * §5.4), so all bytes until the frame completes belong to it. */
			if (ws->stream_active) {
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(ws->fd, &rfds);
				struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
				int sel = select((int)(ws->fd + 1), &rfds, NULL, NULL,
				                 timeout_ms < 0 ? NULL : &tv);
				if (sel < 0) {
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "select failed during large frame",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				if (sel == 0)
					return WS_EVENT_NONE; /* still accumulating; nothing to report */

				/* Drain the socket into the frag buffer (recv_buf doubles as the
				 * scratch space) until the frame completes or we'd block. */
				while (ws->stream_remaining > 0) {
					size_t want = ws->stream_remaining > WS_RECV_BUF_SIZE
					                  ? WS_RECV_BUF_SIZE
					                  : (size_t)ws->stream_remaining;
					int n = sock_recv(ws->fd, ws->recv_buf, want);
					if (n < 0) {
						if (sock_would_block())
							break;
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "recv failed during large frame",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (n == 0) {
						ws->state = WS_STATE_CLOSED;
						if (ws->callbacks.on_close)
							ws->callbacks.on_close(ws, 1006, "", 0, ws->callbacks.userdata);
						return WS_EVENT_CLOSE;
					}
					if (ws->stream_masked) {
						for (int i = 0; i < n; i++)
							ws->recv_buf[i] ^=
							    ws->stream_mask[(ws->stream_payload_recvd + (unsigned)i) & 3];
					}
					if (frag_append(ws, (unsigned char const *)ws->recv_buf, (size_t)n) !=
					    0) {
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws,
							                       "out of memory reassembling large frame",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					ws->stream_payload_recvd += (uint64_t)n;
					ws->stream_remaining -= (uint64_t)n;
					if (n < (int)want)
						break; /* would-block -- come back next poll */
				}

				if (ws->stream_remaining > 0)
					return WS_EVENT_NONE;

				int done = ws_finish_large_frame(ws);
				if (done < 0)
					return WS_EVENT_ERROR;
				ws->stream_active = 0;
				if (done > 0)
					return WS_EVENT_MESSAGE;
				/* First fragment of a fragmented message (or partial continuation):
				 * fall through so subsequent fragments/control frames are handled
				 * by the normal recv + parse path below. */
			}

			if (ws->recv_len >= WS_RECV_BUF_SIZE) {
				/* Buffer is full and a previous pass still couldn't extract a
				 * complete frame from it -- the message plainly doesn't fit
				 * in WS_RECV_BUF_SIZE. Previously this fell through to
				 * recv(fd, buf, 0), which returns 0 and was misread as "peer
				 * closed the connection", tearing down a perfectly healthy
				 * link. Report it for what it is instead. */
				ws->state = WS_STATE_ERROR;
				if (ws->callbacks.on_error)
					ws->callbacks.on_error(ws, "message exceeds receive buffer size",
					                       ws->callbacks.userdata);
				return WS_EVENT_ERROR;
			}

			int need_select = 1;
			int peer_eof = 0;
			while (ws->recv_len < WS_RECV_BUF_SIZE) {
				if (need_select) {
					fd_set rfds;
					FD_ZERO(&rfds);
					FD_SET(ws->fd, &rfds);
					struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
					int sel = select((int)(ws->fd + 1), &rfds, NULL, NULL,
					                 timeout_ms < 0 ? NULL : &tv);
					if (sel < 0) {
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "select failed before recv",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (sel == 0)
						break;         /* timeout -- parse whatever is buffered */
					need_select = 0; /* one select is enough; loop recv after it */
				}
				int n = sock_recv(ws->fd, ws->recv_buf + ws->recv_len,
				                  WS_RECV_BUF_SIZE - ws->recv_len);
				if (n < 0) {
					if (sock_would_block())
						break;
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "recv failed", ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				if (n == 0) {
					peer_eof = 1;
					break;
				}
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
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "non-zero RSV bits",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				}
				int fin = (p[0] & 0x80) ? 1 : 0;
				unsigned char opcode = p[0] & 0x0f;
				int masked = (p[1] & 0x80) ? 1 : 0;
				uint64_t payload_len = p[1] & 0x7f;
				size_t header_len = 2;

				if (payload_len == 126) {
					if (ws->recv_len - consumed < 4)
						break;
					payload_len = ((uint64_t)p[2] << 8) | p[3];
					header_len = 4;
				} else if (payload_len == 127) {
					if (ws->recv_len - consumed < 10)
						break;
					if (p[2] & 0x80) {
						unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
						ws_send_control(ws, 0x88, fail, 2);
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "invalid 64-bit payload length",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					payload_len = 0;
					for (int i = 0; i < 8; i++)
						payload_len = (payload_len << 8) | p[2 + i];
					header_len = 10;
				}

				if (masked)
					header_len += 4;

				/* A frame that can never fit in the fixed recv_buf: stream its
				 * payload into the dynamic frag buffer instead of erroring out
				 * when the buffer fills up. Only data frames (0x0/0x1/0x2) can
				 * be this large; anything else is a protocol violation caught
				 * by ws_start_large_frame(). */
				if ((uint64_t)header_len + payload_len > WS_RECV_BUF_SIZE) {
					if (ws->recv_len - consumed < header_len)
						break; /* header not complete yet */
					if (ws_start_large_frame(ws, p, header_len, payload_len, fin, opcode,
					                         masked) != 0)
						return WS_EVENT_ERROR;
					consumed = ws->recv_len; /* header + partial payload consumed */
					break;
				}

				if (ws->recv_len - consumed < header_len + payload_len)
					break;

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
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "fragmented control frame",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (payload_len > 125) {
						unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
						ws_send_control(ws, 0x88, fail, 2);
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "control frame payload too large",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
				}

				if (opcode == 0x8) {
					/* Validate close frame */
					int valid = 1;
					uint16_t code = 1005;
					char const *reason = "";
					size_t reason_len = 0;
					if (payload_len >= 2) {
						code = (uint16_t)((unsigned)payload[0] << 8 | payload[1]);
						if (payload_len > 2) {
							reason = (char const *)payload + 2;
							reason_len = (size_t)(payload_len - 2);
							if (!ws_utf8_valid((unsigned char const *)reason, reason_len))
								valid = 0;
						}
						/* RFC 6455 + RFC 8441: valid close codes are 1000-1003, 1007-1011,
						 * 1014, 3000-4999 */
						if (code < 1000 || code > 4999 || (code >= 1004 && code <= 1006) ||
						    code == 1015 || (code >= 1016 && code <= 2999))
							valid = 0;
					} else if (payload_len > 0) {
						valid = 0; /* body present but < 2 bytes — invalid */
					}
					if (!valid) {
						unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
						ws_send_control(ws, 0x88, fail, 2);
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "invalid close frame",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					/* Echo the complete Close payload, including its optional reason. */
					ws_send_control(ws, 0x88, payload, (size_t)payload_len);
					ws->state = WS_STATE_CLOSING;
					ws->closing_initiated = 0;
					ws->peer_close_code = code;
					ws->peer_close_reason_len = reason_len;
					if (reason_len > 0)
						memcpy(ws->peer_close_reason, reason, reason_len);
					return WS_EVENT_NONE;
				} else if (opcode == 0x9) {
					ws_send_control(ws, 0x8a, payload, (size_t)payload_len);
				} else if (opcode == 0xa) {
					/* Pong — ignore */
				} else if ((opcode >= 0x3 && opcode <= 0x7) ||
				           (opcode >= 0xb && opcode <= 0xf)) {
					unsigned char fail[2] = {0x03, 0xea}; /* 1002 */
					ws_send_control(ws, 0x88, fail, 2);
					ws->state = WS_STATE_ERROR;
					if (ws->callbacks.on_error)
						ws->callbacks.on_error(ws, "reserved opcode",
						                       ws->callbacks.userdata);
					return WS_EVENT_ERROR;
				} else if (opcode == 0x0) {
					/* Continuation frame: must belong to an active fragmented message */
					if (!ws->frag_active) {
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "unexpected continuation frame",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (frag_append(ws, payload, (size_t)payload_len) != 0) {
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(
							    ws, "out of memory reassembling fragmented message",
							    ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (fin) {
						if (!ws->frag_binary &&
						    !ws_utf8_valid(ws->frag_buf, ws->frag_len)) {
							unsigned char fail[2] = {0x03, 0xef}; /* 1007 */
							ws_send_control(ws, 0x88, fail, 2);
							ws->state = WS_STATE_ERROR;
							if (ws->callbacks.on_error)
								ws->callbacks.on_error(ws, "invalid UTF-8 in text message",
								                       ws->callbacks.userdata);
							return WS_EVENT_ERROR;
						}
						if (ws->callbacks.on_message)
							ws->callbacks.on_message(ws, (char const *)ws->frag_buf,
							                         ws->frag_len, ws->frag_binary,
							                         ws->callbacks.userdata);
						delivered = 1;
						ws->frag_active = 0;
						ws->frag_len = 0;
					}
				} else if (opcode == 0x1 || opcode == 0x2) {
					/* If inside a fragmented message, non-continuation frames are a
					 * protocol violation */
					if (ws->frag_active) {
						ws->state = WS_STATE_ERROR;
						if (ws->callbacks.on_error)
							ws->callbacks.on_error(ws, "expected continuation frame",
							                       ws->callbacks.userdata);
						return WS_EVENT_ERROR;
					}
					if (!fin) {
						/* First fragment of a fragmented message -- start buffering. */
						ws->frag_active = 1;
						ws->frag_binary = (opcode == 0x2);
						ws->frag_len = 0;
						if (frag_append(ws, payload, (size_t)payload_len) != 0) {
							ws->state = WS_STATE_ERROR;
							if (ws->callbacks.on_error)
								ws->callbacks.on_error(
								    ws, "out of memory reassembling fragmented message",
								    ws->callbacks.userdata);
							return WS_EVENT_ERROR;
						}
					} else if (ws->callbacks.on_message) {
						int binary = (opcode == 0x2);
						if (!binary && !ws_utf8_valid(payload, (size_t)payload_len)) {
							unsigned char fail[2] = {0x03, 0xef}; /* 1007 */
							ws_send_control(ws, 0x88, fail, 2);
							ws->state = WS_STATE_ERROR;
							if (ws->callbacks.on_error)
								ws->callbacks.on_error(ws, "invalid UTF-8 in text message",
								                       ws->callbacks.userdata);
							return WS_EVENT_ERROR;
						}
						ws->callbacks.on_message(ws, (char const *)payload,
						                         (size_t)payload_len, binary,
						                         ws->callbacks.userdata);
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

			if (peer_eof) {
				ws->state = WS_STATE_CLOSED;
				if (ws->callbacks.on_close)
					ws->callbacks.on_close(ws, 1006, "", 0, ws->callbacks.userdata);
				return WS_EVENT_CLOSE;
			}

			return delivered ? WS_EVENT_MESSAGE : WS_EVENT_NONE;
		}

		/* ---- CLOSING: wait for peer's Close frame, then close TCP ---- */
		case WS_STATE_CLOSING: {
			if (ws->fd == INVALID_SOCKET) {
				ws->state = WS_STATE_CLOSED;
				return WS_EVENT_CLOSE;
			}
			if (!ws->closing_initiated) {
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(ws->fd, &rfds);
				struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
				int sel = select((int)(ws->fd + 1), &rfds, NULL, NULL,
				                 timeout_ms < 0 ? NULL : &tv);
				if (sel == 0)
					return WS_EVENT_NONE;
				int n = sock_recv(ws->fd, ws->recv_buf, WS_RECV_BUF_SIZE);
				if (n > 0 || (n < 0 && sock_would_block()))
					return WS_EVENT_NONE;
			} else {
				/* If we initiated the close, wait a bit for the server's Close frame */
				int n = sock_recv(ws->fd, ws->recv_buf, WS_RECV_BUF_SIZE);
				if (n > 0) {
					/* Got some data — might be a Close frame, but we don't care at this
					 * point */
				} else if (n == 0 || (n < 0 && !sock_would_block())) {
					/* Connection already closed or error */
				}
			}
			sock_close(ws->fd);
			ws->fd = INVALID_SOCKET;
			ws->state = WS_STATE_CLOSED;
			if (!ws->closing_initiated) {
				if (ws->callbacks.on_close)
					ws->callbacks.on_close(ws, ws->peer_close_code, ws->peer_close_reason,
					                       ws->peer_close_reason_len,
					                       ws->callbacks.userdata);
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
	if (!ws)
		return;
	if (ws->state == WS_STATE_OPEN) {
		unsigned char close_payload[2] = {0x03, 0xe8}; /* code 1000 */
		ws_send_control(ws, 0x88, close_payload, 2);
		ws->state = WS_STATE_CLOSING;
		ws->closing_initiated = 1;
	} else {
		ws->state = WS_STATE_CLOSED;
		if (ws->fd != INVALID_SOCKET)
			sock_close(ws->fd);
		ws->fd = INVALID_SOCKET;
	}
}

void ws_destroy(ws_t *ws) {
	if (!ws)
		return;
	ws_close(ws);
	free(ws->host);
	free(ws->path);
	free(ws->frag_buf);
	free(ws->proxy_host);
	free(ws->proxy_auth);
	if (ws->ai_list)
		freeaddrinfo(ws->ai_list);
	free(ws);
	sock_cleanup();
}

ws_state_e ws_state(ws_t *ws) {
	return ws ? ws->state : WS_STATE_ERROR;
}
