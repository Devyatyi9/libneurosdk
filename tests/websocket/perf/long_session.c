#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
static int got_error = 0;
static int closed = 0;
static int invalid_message = 0;
static int send_failed = 0;
static int echo_missing = 0;
static uint16_t close_code = 0;
static unsigned long sent = 0;
static unsigned long received = 0;

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
	if (binary || len != 4 || memcmp(data, "ping", 4) != 0) {
		fprintf(stderr, "[error] invalid echo: binary=%d len=%zu\n", binary, len);
		invalid_message = 1;
		return;
	}
	received++;
}

static void on_close(ws_t *ws,
                     uint16_t code,
                     char const *reason,
                     size_t reason_len,
                     void *userdata) {
	(void)ws;
	(void)userdata;
	closed = 1;
	close_code = code;
	printf("[close] code=%u reason=%.*s (sent=%lu recv=%lu)\n", code,
	       (int)reason_len, reason, sent, received);
}

static void on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	(void)userdata;
	printf("[error] %s\n", msg);
	got_error = 1;
}

int main(int argc, char *argv[]) {
	char const *url = argc > 1 ? argv[1] : "ws://localhost:9001/";
	int interval_ms = argc > 2 ? atoi(argv[2]) * 1000 : 60000;
	double duration_h = argc > 3 ? strtod(argv[3], NULL) : 3.0;
	time_t duration_s = (time_t)(duration_h * 3600.0);
	if (interval_ms <= 0 || duration_s <= 0) {
		fprintf(stderr, "interval and duration must be positive\n");
		return 2;
	}

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

	int connect_budget = 300;
	while (!connected && !got_error && connect_budget-- > 0)
		ws_poll(ws, 100);
	if (!connected || got_error) {
		fprintf(stderr, "[error] connection did not open: error=%d state=%d\n",
		        got_error, ws_state(ws));
		ws_destroy(ws);
		return 1;
	}

	time_t end = time(NULL) + duration_s;
	time_t last_progress = 0;
	printf("[info] Connected. Sending every %dms for %.3gh\n", interval_ms,
	       duration_h);
	printf("[info] Progress: ");

	do {
		char const *msg = "ping";
		if (ws_send(ws, msg, strlen(msg)) != 0) {
			printf("[error] ws_send failed\n");
			send_failed = 1;
			break;
		}
		sent++;

		/* Wait for echo (up to interval_ms) */
		int waited = 0;
		while (received < sent && waited < interval_ms && !got_error) {
			ws_poll(ws, 100);
			waited += 100;
		}

		if (got_error)
			break;
		if (received < sent) {
			fprintf(stderr, "[error] echo not received for message %lu\n", sent);
			echo_missing = 1;
			break;
		}

		/* Wait remainder of interval */
		int remaining = interval_ms - waited;
		if (remaining > 0 && !got_error) {
			int step = remaining < 100 ? remaining : 100;
			for (int i = 0; i < remaining / step && !got_error; i++)
				ws_poll(ws, step);
		}

		time_t now = time(NULL);
		if (now - last_progress >= 60) { /* every minute */
			int left_h = (int)((end - now) / 3600);
			int left_m = (int)((end - now) % 3600 / 60);
			printf("\r[info] %dh %dm left, sent=%lu recv=%lu    ", left_h, left_m,
			       sent, received);
			fflush(stdout);
			last_progress = now;
		}
	} while (time(NULL) < end && !got_error && !invalid_message);
	printf("\r[info] Done.                          \n");

	printf("[info] Closing (sent=%lu recv=%lu errors=%d)\n", sent, received,
	       got_error);
	if (ws_state(ws) == WS_STATE_OPEN)
		ws_close(ws);
	for (int i = 0; i < 50 && ws_state(ws) == WS_STATE_CLOSING; i++)
		ws_poll(ws, 100);
	int state = ws_state(ws);
	int result = got_error || invalid_message || send_failed || echo_missing ||
	             received != sent || !closed || close_code != 1000 ||
	             state != WS_STATE_CLOSED;
	if (result) {
		fprintf(stderr,
		        "[fail] connected=%d sent=%lu received=%lu invalid=%d "
		        "send_failed=%d echo_missing=%d errors=%d closed=%d "
		        "close_code=%u state=%d\n",
		        connected, sent, received, invalid_message, send_failed,
		        echo_missing, got_error, closed, close_code, state);
	}
	ws_destroy(ws);
	return result;
}
