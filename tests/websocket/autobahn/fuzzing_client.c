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

static int s_done = 0;
static int s_connected = 0;

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
	(void)userdata;
	int rc = binary ? ws_send_binary(ws, data, len) : ws_send(ws, data, len);
	if (rc != 0)
		fprintf(stderr, "[fuzzing] send failed\n");
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
	s_done = 1;
}

static void on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	(void)userdata;
	fprintf(stderr, "[fuzzing] error: %s\n", msg);
	s_done = 1;
}

static char const *default_url = "ws://127.0.0.1:9001";

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <case_number> [url]\n", argv[0]);
		fprintf(stderr, "  url defaults to %s\n", default_url);
		return 1;
	}

	char const *base = argc > 2 ? argv[2] : default_url;
	char url[512];
	snprintf(url, sizeof(url), "%s/runCase?case=%s&agent=ws_client", base,
	         argv[1]);

	ws_callbacks_t cbs = {on_open, on_message, on_close, on_error, NULL};
	ws_t *ws = NULL;
	if (ws_connect(&ws, url, cbs) != 0) {
		fprintf(stderr, "[fuzzing] ws_connect failed\n");
		return 1;
	}

	while (!s_done)
		ws_poll(ws, 100);

	ws_destroy(ws);
	/* Infrastructure error (never connected) → exit 1.
	 * Protocol error detected after connection → exit 0 (expected behaviour). */
	return s_connected ? 0 : 1;
}
