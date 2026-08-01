#include <stdio.h>
#include <stdlib.h>
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

#define EXPECTED_LEN 300000

static int s_connected = 0;
static int s_done = 0;
static int s_failed = 0;
static int s_message_count = 0;
static int s_closed = 0;

static void on_open(ws_t *ws, void *userdata) {
	(void)ws;
	(void)userdata;
	s_connected = 1;
}

static void on_message(ws_t *ws,
                       char const *data,
                       size_t len,
                       int binary,
                       void *userdata) {
	(void)ws;
	(void)userdata;
	s_message_count++;
	if (s_message_count != 1 || len != EXPECTED_LEN || binary) {
		fprintf(stderr, "FAIL: unexpected message (%zu bytes, binary=%d)\n", len,
		        binary);
		s_failed = 1;
		return;
	}
	for (size_t i = 0; i < len; i++) {
		if (data[i] != 'X') {
			fprintf(stderr, "FAIL: payload mismatch at byte %zu\n", i);
			s_failed = 1;
			return;
		}
	}
}

static void on_close(ws_t *ws,
                     uint16_t code,
                     char const *reason,
                     size_t reason_len,
                     void *userdata) {
	(void)ws;
	(void)reason;
	(void)reason_len;
	(void)userdata;
	if (code != 1000) {
		fprintf(stderr, "FAIL: expected close code 1000, got %u\n", code);
		s_failed = 1;
	}
	s_closed = 1;
	s_done = 1;
}

static void on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	(void)userdata;
	fprintf(stderr, "FAIL: unexpected error: %s\n", msg);
	s_failed = 1;
	s_done = 1;
}

int main(int argc, char *argv[]) {
	char const *url = argc > 1 ? argv[1] : "ws://127.0.0.1:19008/";
	ws_callbacks_t cbs = {on_open, on_message, on_close, on_error, NULL};
	ws_t *ws = NULL;
	if (ws_connect(&ws, url, cbs) != 0) {
		fprintf(stderr, "FAIL: ws_connect\n");
		return 1;
	}
	uint64_t deadline = now_ms() + 30000;
	while (!s_done && now_ms() < deadline) {
		ws_poll(ws, 100);
	}
	ws_state_e final_state = ws_state(ws);
	ws_destroy(ws);
	if (!s_connected) {
		fprintf(stderr, "FAIL: never connected\n");
		return 1;
	}
	if (s_failed || s_message_count != 1 || !s_closed ||
	    final_state != WS_STATE_CLOSED) {
		fprintf(stderr, "FAIL: messages=%d closed=%d state=%d timeout=%d\n",
		        s_message_count, s_closed, (int)final_state, now_ms() >= deadline);
		return 1;
	}
	return 0;
}
