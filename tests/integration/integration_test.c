#include "ws_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

static int total, passed, failed;

#define TEST(name, expr) do { \
  total++; \
  if (!(expr)) { \
    fprintf(stderr, "[FAIL] %s\n", name); \
    failed++; \
  } else { \
    fprintf(stderr, "[PASS] %s\n", name); \
    passed++; \
  } \
} while(0)

typedef struct {
  const char *payload;
  size_t len;
  int binary;
} test_ctx_t;

static int test_connected, test_got_msg, test_got_close, test_got_error;

static void t_on_open(ws_t *ws, void *userdata) {
  (void)ws; (void)userdata;
  test_connected = 1;
}

static void t_on_message(ws_t *ws, const char *data, size_t len, int binary, void *userdata) {
  (void)ws;
  test_ctx_t *ctx = (test_ctx_t *)userdata;
  if (binary != ctx->binary) {
    fprintf(stderr, "  type mismatch\n");
    test_got_error = 1; return;
  }
  if (len != ctx->len || memcmp(data, ctx->payload, len) != 0) {
    fprintf(stderr, "  payload mismatch: got %zu exp %zu\n", len, ctx->len);
    test_got_error = 1; return;
  }
  test_got_msg = 1;
}

static void t_on_close(ws_t *ws, uint16_t code, const char *reason, size_t reason_len, void *userdata) {
  (void)ws; (void)reason; (void)reason_len; (void)userdata;
  fprintf(stderr, "  close code=%u\n", code);
  test_got_close = 1;
}

static void t_on_error(ws_t *ws, const char *msg, void *userdata) {
  (void)ws; (void)userdata;
  fprintf(stderr, "  error: %s\n", msg);
  test_got_error = 1;
}

static int run_one(const char *url, const char *payload, size_t len, int binary) {
  test_connected = 0; test_got_msg = 0; test_got_close = 0; test_got_error = 0;
  test_ctx_t ctx = { payload, len, binary };
  int timeout;

  ws_callbacks_t cbs = { t_on_open, t_on_message, t_on_close, t_on_error, &ctx };

  ws_t *ws = NULL;
  if (ws_connect(&ws, url, cbs) != 0) return -1;

  timeout = 100;
  while (!test_connected && !test_got_error && timeout-- > 0) ws_poll(ws, 100);
  if (!test_connected) { fprintf(stderr, "  timeout waiting for connect\n"); ws_destroy(ws); return -1; }

  int rc = binary ? ws_send_binary(ws, payload, len) : ws_send(ws, payload, len);
  if (rc != 0) { ws_destroy(ws); return -1; }

  timeout = 100;
  while (!test_got_msg && !test_got_close && !test_got_error && timeout-- > 0) ws_poll(ws, 100);
  if (!test_got_msg) { fprintf(stderr, "  timeout waiting for echo\n"); ws_destroy(ws); return -1; }

  ws_close(ws);
  for (int i = 0; i < 10 && ws_state(ws) == WS_STATE_CLOSING; i++)
    ws_poll(ws, 100);
  ws_destroy(ws);
  return 0;
}

int main(int argc, char *argv[]) {
  const char *url = argc > 1 ? argv[1] : "ws://localhost:9001/";

  { /* 1. Normal text */
    const char *msg = "Hello, WebSocket!";
    fprintf(stderr, "[test] Normal text\n");
    TEST("normal text", run_one(url, msg, strlen(msg), 0) == 0);
  }

  { /* 2. Binary */
    unsigned char bin[] = { 0x00, 0x01, 0x02, 0xFE, 0xFF, 0x80, 0x7F };
    fprintf(stderr, "[test] Binary (%zu bytes)\n", sizeof(bin));
    TEST("binary", run_one(url, (const char *)bin, sizeof(bin), 1) == 0);
  }

  { /* 3. Large >125 */
    size_t len = 200;
    char *buf = (char *)malloc(len);
    for (size_t i = 0; i < len; i++) buf[i] = (char)('a' + (i % 26));
    fprintf(stderr, "[test] Large %zu bytes (>125)\n", len);
    TEST("large >125", run_one(url, buf, len, 0) == 0);
    free(buf);
  }

  { /* 4. Huge >65535 */
    size_t len = 70000;
    char *buf = (char *)malloc(len);
    for (size_t i = 0; i < len; i++) buf[i] = (char)('0' + (i % 10));
    fprintf(stderr, "[test] Huge %zu bytes (>65535)\n", len);
    TEST("huge >65535", run_one(url, buf, len, 0) == 0);
    free(buf);
  }

  fprintf(stderr, "\n%d tests: %d passed, %d failed\n", total, passed, failed);
  return failed > 0 ? 1 : 0;
}
