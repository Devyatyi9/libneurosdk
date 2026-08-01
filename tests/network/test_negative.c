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

static int opened;
static int got_error;
static int got_close;
static uint16_t close_code;
static char error_message[256];

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
	got_close = 1;
	close_code = code;
}

static void on_error(ws_t *ws, char const *message, void *userdata) {
	(void)ws;
	(void)userdata;
	got_error = 1;
	snprintf(error_message, sizeof(error_message), "%s", message);
}

static int fail(ws_t *ws, char const *expected) {
	fprintf(stderr,
	        "FAIL: expected %s; opened=%d error=%d message='%s' close=%d "
	        "code=%u state=%d\n",
	        expected, opened, got_error, error_message, got_close, close_code,
	        (int)ws_state(ws));
	return 1;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr,
		        "usage: test_negative <url> <error-before-open|error-after-open|"
		        "close-after-open|timeout-before-open> [value]\n");
		return 2;
	}

	char const *url = argv[1];
	char const *mode = argv[2];
	char const *value = argc > 3 ? argv[3] : "";
	ws_callbacks_t callbacks = {on_open, on_message, on_close, on_error, NULL};
	ws_t *ws = NULL;
	if (ws_connect(&ws, url, callbacks) != 0) {
		fprintf(stderr, "FAIL: ws_connect returned an immediate error\n");
		return 1;
	}

	uint64_t duration = strcmp(mode, "timeout-before-open") == 0 ? 3000 : 15000;
	uint64_t deadline = now_ms() + duration;
	while (!got_error && !got_close && now_ms() < deadline)
		ws_poll(ws, 100);

	int result = 1;
	if (strcmp(mode, "error-before-open") == 0) {
		result =
		    got_error && !opened && !got_close && strcmp(error_message, value) == 0
		        ? 0
		        : fail(ws, mode);
	} else if (strcmp(mode, "error-after-open") == 0) {
		result =
		    got_error && opened && !got_close && strcmp(error_message, value) == 0
		        ? 0
		        : fail(ws, mode);
	} else if (strcmp(mode, "close-after-open") == 0) {
		uint16_t expected_code = (uint16_t)strtoul(value, NULL, 10);
		result = got_close && opened && !got_error && close_code == expected_code
		             ? 0
		             : fail(ws, mode);
	} else if (strcmp(mode, "timeout-before-open") == 0) {
		result = !opened && !got_error && !got_close && now_ms() >= deadline
		             ? 0
		             : fail(ws, mode);
	} else {
		fprintf(stderr, "FAIL: unknown mode '%s'\n", mode);
	}

	ws_destroy(ws);
	return result;
}
