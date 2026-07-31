#include "ws_client.h"

#include <stdio.h>

static int opened;
static int closed;
static int failed;

static void on_open(ws_t *ws, void *userdata) {
	(void)ws;
	(void)userdata;
	opened = 1;
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
	failed = 1;
}

static void on_close(ws_t *ws,
                     uint16_t code,
                     char const *reason,
                     size_t reason_len,
                     void *userdata) {
	(void)ws;
	(void)userdata;
	if (code != 1000 || reason_len != 3 || reason[0] != 'b' || reason[1] != 'y' ||
	    reason[2] != 'e')
		failed = 1;
	closed = 1;
}

static void on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	(void)userdata;
	fprintf(stderr, "peer-close error: %s\n", msg);
	failed = 1;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <ws-url>\n", argv[0]);
		return 2;
	}

	ws_callbacks_t callbacks = {on_open, on_message, on_close, on_error, NULL};
	ws_t *ws = NULL;
	if (ws_connect(&ws, argv[1], callbacks) != 0)
		return 1;

	for (int i = 0; i < 100 && !closed && !failed; i++)
		ws_poll(ws, 100);
	int success = opened && closed && !failed && ws_state(ws) == WS_STATE_CLOSED;
	if (!success)
		fprintf(stderr, "peer-close result: opened=%d closed=%d failed=%d state=%d\n",
		        opened, closed, failed, (int)ws_state(ws));
	ws_destroy(ws);
	return success ? 0 : 1;
}
