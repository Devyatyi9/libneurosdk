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

static int total, passed, failed;

#define TEST(name, expr)                    \
	do {                                      \
		total++;                                \
		if (!(expr)) {                          \
			fprintf(stderr, "[FAIL] %s\n", name); \
			failed++;                             \
		} else {                                \
			fprintf(stderr, "[PASS] %s\n", name); \
			passed++;                             \
		}                                       \
	} while (0)

typedef struct {
	char const *payload;
	size_t len;
	int binary;
} test_ctx_t;

static int test_connected, test_got_close, test_got_error;
static uint16_t test_close_code;
static size_t test_message_count;

static void t_on_open(ws_t *ws, void *userdata) {
	(void)ws;
	(void)userdata;
	test_connected = 1;
}

static void t_on_message(ws_t *ws,
                         char const *data,
                         size_t len,
                         int binary,
                         void *userdata) {
	(void)ws;
	test_ctx_t *ctx = (test_ctx_t *)userdata;
	test_message_count++;
	if (binary != ctx->binary) {
		fprintf(stderr, "  type mismatch\n");
		test_got_error = 1;
		return;
	}
	if (len != ctx->len || memcmp(data, ctx->payload, len) != 0) {
		fprintf(stderr, "  payload mismatch: got %zu exp %zu\n", len, ctx->len);
		test_got_error = 1;
		return;
	}
}

static void t_on_close(ws_t *ws,
                       uint16_t code,
                       char const *reason,
                       size_t reason_len,
                       void *userdata) {
	(void)ws;
	(void)reason;
	(void)reason_len;
	(void)userdata;
	fprintf(stderr, "  close code=%u\n", code);
	test_close_code = code;
	test_got_close = 1;
}

static void t_on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	(void)userdata;
	fprintf(stderr, "  error: %s\n", msg);
	test_got_error = 1;
}

static int run_one(char const *url,
                   char const *payload,
                   size_t len,
                   int binary) {
	test_connected = 0;
	test_got_close = 0;
	test_got_error = 0;
	test_close_code = 0;
	test_message_count = 0;
	test_ctx_t ctx = {payload, len, binary};

	ws_callbacks_t cbs = {t_on_open, t_on_message, t_on_close, t_on_error, &ctx};

	ws_t *ws = NULL;
	if (ws_connect(&ws, url, cbs) != 0)
		return -1;

	uint64_t deadline = now_ms() + 10000;
	while (!test_connected && !test_got_error && now_ms() < deadline)
		ws_poll(ws, 100);
	if (!test_connected || test_got_error) {
		fprintf(stderr, "  %s waiting for connect\n",
		        test_got_error ? "error" : "timeout");
		ws_destroy(ws);
		return -1;
	}

	int rc =
	    binary ? ws_send_binary(ws, payload, len) : ws_send(ws, payload, len);
	if (rc != 0) {
		ws_destroy(ws);
		return -1;
	}

	deadline = now_ms() + 10000;
	while (test_message_count == 0 && !test_got_close && !test_got_error &&
	       now_ms() < deadline)
		ws_poll(ws, 100);

	if (ws_state(ws) == WS_STATE_OPEN)
		ws_close(ws);
	deadline = now_ms() + 10000;
	while (!test_got_close && !test_got_error && now_ms() < deadline)
		ws_poll(ws, 100);

	ws_state_e final_state = ws_state(ws);
	int fail = test_got_error || test_message_count != 1 || !test_got_close ||
	           test_close_code != 1000 || final_state != WS_STATE_CLOSED;
	if (fail)
		fprintf(stderr,
		        "  connected=%d messages=%zu close=%d close_code=%u error=%d "
		        "state=%d\n",
		        test_connected, test_message_count, test_got_close, test_close_code,
		        test_got_error, (int)final_state);
	ws_destroy(ws);
	return fail ? -1 : 0;
}

int main(int argc, char *argv[]) {
	char const *url = argc > 1 ? argv[1] : "ws://localhost:9001/";

	{ /* 1. Normal text */
		char const *msg = "Hello, WebSocket!";
		fprintf(stderr, "[test] Normal text\n");
		TEST("normal text", run_one(url, msg, strlen(msg), 0) == 0);
	}

	{ /* 2. Binary */
		unsigned char bin[] = {0x00, 0x01, 0x02, 0xFE, 0xFF, 0x80, 0x7F};
		fprintf(stderr, "[test] Binary (%zu bytes)\n", sizeof(bin));
		TEST("binary", run_one(url, (char const *)bin, sizeof(bin), 1) == 0);
	}

	{ /* 3. Large >125 */
		size_t len = 200;
		char *buf = (char *)malloc(len);
		for (size_t i = 0; i < len; i++)
			buf[i] = (char)('a' + (i % 26));
		fprintf(stderr, "[test] Large %zu bytes (>125)\n", len);
		TEST("large >125", run_one(url, buf, len, 0) == 0);
		free(buf);
	}

	{ /* 4. Huge >65535 */
		size_t len = 70000;
		char *buf = (char *)malloc(len);
		for (size_t i = 0; i < len; i++)
			buf[i] = (char)('0' + (i % 10));
		fprintf(stderr, "[test] Huge %zu bytes (>65535)\n", len);
		TEST("huge >65535", run_one(url, buf, len, 0) == 0);
		free(buf);
	}

	fprintf(stderr, "\n%d tests: %d passed, %d failed\n", total, passed, failed);
	return failed > 0 ? 1 : 0;
}
