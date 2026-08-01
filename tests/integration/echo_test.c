#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int connected = 0;
static int got_message = 0;
static int got_close = 0;
static int got_error = 0;
static char const *expected_message = "Hello, WebSocket!";

static void on_open(ws_t *ws, void *userdata) {
	(void)ws;
	(void)userdata;
	printf("[open] Connection established\n");
	connected = 1;
}

static void on_message(ws_t *ws,
                       char const *data,
                       size_t len,
                       int binary,
                       void *userdata) {
	(void)ws;
	(void)userdata;
	printf("[message] %s%.*s\n", binary ? "BINARY " : "", (int)len, data);
	if (binary || len != strlen(expected_message) ||
	    memcmp(data, expected_message, len) != 0) {
		fprintf(stderr, "unexpected echo payload\n");
		got_error = 1;
		return;
	}
	got_message = 1;
}

static void on_close(ws_t *ws,
                     uint16_t code,
                     char const *reason,
                     size_t reason_len,
                     void *userdata) {
	(void)ws;
	(void)userdata;
	printf("[close] code=%u reason=%.*s\n", code, (int)reason_len, reason);
	got_close = 1;
}

static void on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	(void)userdata;
	printf("[error] %s\n", msg);
	got_error = 1;
}

int main(int argc, char *argv[]) {
	char const *url = argc > 1 ? argv[1] : "ws://localhost:9001/";
	if (argc > 2)
		expected_message = argv[2];

	ws_callbacks_t callbacks = {
	    .on_open = on_open,
	    .on_message = on_message,
	    .on_close = on_close,
	    .on_error = on_error,
	};

	ws_t *ws = NULL;
	if (ws_connect(&ws, url, callbacks) != 0) {
		fprintf(stderr, "ws_connect failed\n");
		return 1;
	}
	printf("[info] Connecting to %s ...\n", url);

	/* Wait for connection (max 10s) */
	int timeout = 100; /* 100ms per poll, 100 iterations = 10s */
	while (!connected && !got_error && timeout-- > 0) {
		ws_poll(ws, 100);
	}
	if (got_error) {
		ws_destroy(ws);
		return 1;
	}
	if (!connected) {
		fprintf(stderr, "timeout waiting for connect\n");
		ws_destroy(ws);
		return 1;
	}

	/* Send a message */
	char const *msg = "Hello, WebSocket!";
	printf("[send] %s\n", msg);
	if (ws_send(ws, msg, strlen(msg)) != 0) {
		fprintf(stderr, "ws_send failed\n");
		ws_destroy(ws);
		return 1;
	}

	/* Wait for echo (max 10s) */
	timeout = 100;
	while (!got_message && !got_close && !got_error && timeout-- > 0) {
		ws_poll(ws, 100);
	}
	if (!got_message && !got_error) {
		fprintf(stderr, "timeout waiting for echo\n");
	}

	if (got_message) {
		printf("[pass] Echo received successfully\n");
	} else if (got_error) {
		fprintf(stderr, "[fail] Error during test\n");
	}

	ws_close(ws);
	/* Let CLOSING handshake complete */
	for (int i = 0; i < 10 && ws_state(ws) == WS_STATE_CLOSING; i++)
		ws_poll(ws, 100);

	ws_destroy(ws);
	return (got_message && !got_error) ? 0 : 1;
}
