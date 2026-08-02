#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ws_client.h"

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 5105)
#endif
#include <windows.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <time.h>
static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
#endif

static char const messages[][128] = {
    "shared context dictionary shared context dictionary AAAAAAAAAAAAAAAAAAAAAAAA",
    "shared context dictionary shared context dictionary BBBBBBBBBBBBBBBBBBBBBBBB"};
static int opened;
static int received;
static int failed;
static int closed;

static void on_open(ws_t *ws, void *userdata) {
	(void)userdata;
	opened = 1;
	if (ws_send(ws, messages[0], strlen(messages[0])) != 0)
		failed = 1;
}

static void on_message(ws_t *ws,
	                   char const *data,
	                   size_t len,
	                   int binary,
	                   void *userdata) {
	(void)userdata;
	if (binary || received >= 2 || len != strlen(messages[received]) ||
	    memcmp(data, messages[received], len) != 0) {
		failed = 1;
		return;
	}
	received++;
	if (received == 1) {
		if (ws_send(ws, messages[1], strlen(messages[1])) != 0)
			failed = 1;
	} else {
		ws_close(ws);
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
	closed = 1;
	if (code != 1000)
		failed = 1;
}

static void on_error(ws_t *ws, char const *message, void *userdata) {
	(void)ws;
	(void)message;
	(void)userdata;
	failed = 1;
}

int main(int argc, char **argv) {
	if (argc != 2)
		return 2;
	ws_callbacks_t callbacks = {on_open, on_message, on_close, on_error, NULL};
	ws_t *ws = NULL;
	if (ws_connect(&ws, argv[1], callbacks) != 0)
		return 1;
	uint64_t deadline = now_ms() + 10000;
	while (!closed && !failed && now_ms() < deadline)
		ws_poll(ws, 100);
	int result = !opened || received != 2 || !closed || failed;
	ws_destroy(ws);
	return result;
}
