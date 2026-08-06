#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

// Keep static SDK dependency directives with the networking implementation.
// MSVC records them in ws_client.obj.
#if defined(_WIN32) && defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- State machine --- */
typedef enum {
	WS_STATE_INIT,
	WS_STATE_CONNECTING,
	WS_STATE_PROXY_TUNNEL,
	WS_STATE_UPGRADING,
	WS_STATE_OPEN,
	WS_STATE_CLOSING,
	WS_STATE_CLOSED,
	WS_STATE_ERROR
} ws_state_e;

/* --- Event types returned by ws_poll --- */
typedef enum {
	WS_EVENT_NONE,
	WS_EVENT_OPEN,
	WS_EVENT_MESSAGE,
	WS_EVENT_CLOSE,
	WS_EVENT_ERROR
} ws_event_e;

/* --- Opaque connection handle --- */
typedef struct ws_t ws_t;

/* --- Callbacks --- */
typedef void (*ws_on_open_fn)(ws_t *ws, void *userdata);
typedef void (*ws_on_message_fn)(ws_t *ws,
                                 char const *data,
                                 size_t len,
                                 int binary,
                                 void *userdata);
typedef void (*ws_on_close_fn)(ws_t *ws,
                               uint16_t code,
                               char const *reason,
                               size_t reason_len,
                               void *userdata);
typedef void (*ws_on_error_fn)(ws_t *ws, char const *msg, void *userdata);

typedef struct {
	ws_on_open_fn on_open;
	ws_on_message_fn on_message;
	ws_on_close_fn on_close;
	ws_on_error_fn on_error;
	void *userdata;
} ws_callbacks_t;

/* --- Public API --- */

/* Connect to ws://host:port/path.
 * host may be a hostname or IP literal (no ws:// prefix).
 * url format: "ws://host:port/path"
 * Returns 0 on success (async), -1 on immediate error (bad URL). */
int ws_connect(ws_t **out, char const *url, ws_callbacks_t callbacks);

/* Connect via an HTTP CONNECT proxy. proxy may be:
 *   "host:port"                       -- plain host
 *   "http://host:port"                -- explicit scheme
 *   "http://user:pass@host:port"      -- with credentials
 * NULL or "" means: use env HTTP_PROXY/ALL_PROXY if set (subject to
 * NO_PROXY), otherwise connect directly. Returns 0 on success. */
int ws_connect_via_proxy(ws_t **out,
                         char const *url,
                         ws_callbacks_t callbacks,
                         char const *proxy);

/* Send a text frame. Returns 0 on success, -1 on error. */
int ws_send(ws_t *ws, char const *data, size_t len);

/* Send a binary frame. Returns 0 on success, -1 on error. */
int ws_send_binary(ws_t *ws, char const *data, size_t len);

/* Poll for events. timeout_ms: max wait in ms (0 = no wait, -1 = infinite).
 * Returns WS_EVENT_* if event fired, WS_EVENT_NONE on timeout. */
ws_event_e ws_poll(ws_t *ws, int timeout_ms);

/* Close the connection gracefully. */
void ws_close(ws_t *ws);

/* Destroy and free all resources. */
void ws_destroy(ws_t *ws);

/* Get current state. */
ws_state_e ws_state(ws_t *ws);

#ifdef __cplusplus
}
#endif

#endif /* WS_CLIENT_H */
