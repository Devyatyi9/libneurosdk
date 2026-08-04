#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ws_client.h"

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 5105)
#include <windows.h>
#pragma warning(pop)
static uint64_t now_ms(void) {
	return (uint64_t)GetTickCount64();
}
#else
#include <time.h>
static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
#endif

static int connected = 0;
static int got_close = 0;
static int got_error = 0;
static uint16_t close_code = 0;
static size_t message_count = 0;
static int payload_ok = 1;
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
	message_count++;
	printf("[message] %s%.*s\n", binary ? "BINARY " : "", (int)len, data);
	if (binary || len != strlen(expected_message) ||
	    memcmp(data, expected_message, len) != 0) {
		fprintf(stderr, "unexpected echo payload\n");
		payload_ok = 0;
	}
}

static void on_close(ws_t *ws,
                     uint16_t code,
                     char const *reason,
                     size_t reason_len,
                     void *userdata) {
	(void)ws;
	(void)userdata;
	printf("[close] code=%u reason=%.*s\n", code, (int)reason_len, reason);
	close_code = code;
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

	uint64_t deadline = now_ms() + 10000;
	while (!connected && !got_error && now_ms() < deadline)
		ws_poll(ws, 100);
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

	deadline = now_ms() + 10000;
	while (message_count == 0 && !got_close && !got_error && now_ms() < deadline)
		ws_poll(ws, 100);

	if (ws_state(ws) == WS_STATE_OPEN)
		ws_close(ws);
	deadline = now_ms() + 10000;
	while (!got_close && !got_error && now_ms() < deadline)
		ws_poll(ws, 100);

	ws_state_e final_state = ws_state(ws);
	int fail = got_error || !connected || message_count != 1 || !payload_ok ||
	           !got_close || close_code != 1000 || final_state != WS_STATE_CLOSED;
	if (fail) {
		fprintf(stderr,
		        "[fail] connected=%d messages=%zu payload_ok=%d close=%d "
		        "close_code=%u error=%d state=%d\n",
		        connected, message_count, payload_ok, got_close, close_code,
		        got_error, (int)final_state);
	} else {
		printf("[pass] Echo and clean Close completed successfully\n");
	}

	ws_destroy(ws);
	return fail ? 1 : 0;
}
