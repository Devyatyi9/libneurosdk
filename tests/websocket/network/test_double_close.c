#include <stdio.h>
#include <stdlib.h>
#include "ws_client.h"
#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 5105)
#include <windows.h>
#pragma warning(pop)
#define SLEEP_MS(x) Sleep(x)
#else
#include <unistd.h>
#define SLEEP_MS(x) usleep((x) * 1000)
#endif

static int s_connected = 0;
static int s_failed = 0;

static void on_open(ws_t *ws, void *userdata) {
	(void)userdata;
	s_connected = 1;
	/* Call ws_close twice — must not crash or double-free */
	ws_close(ws);
	ws_close(ws);
}

static void on_message(ws_t *ws,
                       char const *data,
                       size_t len,
                       int binary,
                       void *userdata) {
	(void)ws;
	(void)data;
	(void)len;
	(void)binary;
	(void)userdata;
}

static void on_close(ws_t *ws,
                     uint16_t code,
                     char const *reason,
                     size_t reason_len,
                     void *userdata) {
	(void)ws;
	(void)code;
	(void)reason;
	(void)reason_len;
	(void)userdata;
}

static void on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	(void)userdata;
	fprintf(stderr, "FAIL: error: %s\n", msg);
	s_failed = 1;
}

int main(int argc, char *argv[]) {
	char const *url = argc > 1 ? argv[1] : "ws://127.0.0.1:19007/";
	ws_callbacks_t cbs = {on_open, on_message, on_close, on_error, NULL};
	ws_t *ws = NULL;
	if (ws_connect(&ws, url, cbs) != 0) {
		fprintf(stderr, "FAIL: ws_connect\n");
		return 1;
	}
	/* Second ws_close() transitions CLOSING→CLOSED directly (no on_close
	 * callback), so wait for the state explicitly. Budget is small since
	 * ws_poll() in CLOSED returns immediately. */
	int budget = 100;
	while (ws_state(ws) != WS_STATE_CLOSED && budget > 0) {
		ws_poll(ws, 100);
		budget--;
	}
	if (!s_connected) {
		fprintf(stderr, "FAIL: never connected\n");
		ws_destroy(ws);
		return 1;
	}
	if (s_failed) {
		ws_destroy(ws);
		return 1;
	}
	if (ws_state(ws) != WS_STATE_CLOSED) {
		fprintf(stderr, "FAIL: state=%d, expected WS_STATE_CLOSED\n", ws_state(ws));
		ws_destroy(ws);
		return 1;
	}
	/* Calling ws_close() again via ws_destroy() must be safe (no double-free) */
	ws_destroy(ws);
	printf("OK: double ws_close reached CLOSED without crash\n");
	return 0;
}
