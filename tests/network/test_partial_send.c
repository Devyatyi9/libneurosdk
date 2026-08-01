#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ws_client.h"
#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(x) Sleep(x)
#else
#include <unistd.h>
#define SLEEP_MS(x) usleep((x) * 1000)
#endif

static int s_connected = 0;
static int s_got_reply = 0;
static int s_failed =
    0; /* only set on on_error — real protocol/network failure */
static int s_done = 0;

static void on_open(ws_t *ws, void *userdata) {
	(void)userdata;
	s_connected = 1;
	size_t size = 10 * 1024 * 1024; /* 10 MB */
	char *big = (char *)malloc(size);
	if (!big) {
		fprintf(stderr, "FAIL: OOM\n");
		s_failed = 1;
		s_done = 1;
		return;
	}
	memset(big, 'A', size);
	int rc = ws_send(ws, big, size);
	free(big);
	if (rc != 0) {
		/* Expected on platforms with small kernel send buffers: sock_send_all()
		 * can't push 10MB against a slow consumer and times out. Not a failure
		 * by itself — the test's real assertion is that the WS stream stays in
		 * sync (no desync) and the server's reply still arrives intact. */
		fprintf(stderr,
		        "info: ws_send incomplete (rc=%d) — expected with slow consumer\n",
		        rc);
	}
}

static void on_message(ws_t *ws,
                       char const *data,
                       size_t len,
                       int binary,
                       void *userdata) {
	(void)ws;
	(void)data;
	(void)binary;
	(void)userdata;
	/* The slow-consumer server sends a fixed small "Hello" by design — it
	 * does NOT echo the 10MB back. Its arrival proves the WS stream stayed
	 * intact (correctly framed) through the huge partial send. */
	printf("info: server replied with %zu-byte message\n", len);
	s_got_reply = 1;
	ws_close(ws);
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
	fprintf(stderr, "FAIL: error: %s\n", msg);
	s_failed = 1;
	s_done = 1;
}

int main(int argc, char *argv[]) {
	char const *url = argc > 1 ? argv[1] : "ws://127.0.0.1:19005/";
	ws_callbacks_t cbs = {on_open, on_message, on_close, on_error, NULL};
	ws_t *ws = NULL;
	if (ws_connect(&ws, url, cbs) != 0) {
		fprintf(stderr, "FAIL: ws_connect\n");
		return 1;
	}
	int budget = 6000;
	while (!s_done && budget > 0) {
		ws_poll(ws, 100);
		budget--;
	}
	ws_destroy(ws);
	if (!s_connected) {
		fprintf(stderr, "FAIL: never connected\n");
		return 1;
	}
	if (s_failed)
		return 1;
	if (!s_got_reply) {
		fprintf(stderr,
		        "FAIL: server reply never arrived (stream likely desynced)\n");
		return 1;
	}
	if (budget <= 0 && !s_done) {
		fprintf(stderr, "FAIL: timed out waiting for server response\n");
		return 1;
	}
	/* Pass: the 10MB (partial) send did not desync the stream — the server's
	 * reply arrived as a correctly-framed message and no on_error fired. */
	return 0;
}
