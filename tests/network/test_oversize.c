#include <stdio.h>
#include <stdlib.h>
#include "ws_client.h"
#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(x) Sleep(x)
#else
#include <unistd.h>
#define SLEEP_MS(x) usleep((x) * 1000)
#endif

#define EXPECTED_LEN 300000

static int s_connected = 0;
static int s_done = 0;
static int s_got_message = 0;

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
	(void)data;
	(void)userdata;
	/* The client now streams frames larger than its recv buffer
	 * (Autobahn 9.x), so the oversized payload must be delivered whole. */
	if (len == EXPECTED_LEN && !binary) {
		printf("OK: streamed oversized frame (%zu bytes)\n", len);
		s_got_message = 1;
	} else {
		fprintf(stderr, "FAIL: unexpected message (%zu bytes, binary=%d)\n", len,
		        binary);
	}
	s_done = 1;
}

static void on_close(ws_t *ws,
                     uint16_t code,
                     char const *reason,
                     size_t reason_len,
                     void *userdata) {
	(void)ws;
	(void)userdata;
	printf("close with code %u (%.*s)\n", code, (int)reason_len, reason);
	s_done = 1;
}

static void on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	(void)userdata;
	fprintf(stderr, "FAIL: unexpected error: %s\n", msg);
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
	int budget = 500;
	while (!s_done && budget > 0) {
		ws_poll(ws, 100);
		budget--;
	}
	ws_destroy(ws);
	if (!s_connected) {
		fprintf(stderr, "FAIL: never connected\n");
		return 1;
	}
	if (!s_got_message) {
		fprintf(stderr, "FAIL: oversized message not delivered\n");
		return 1;
	}
	return 0;
}
