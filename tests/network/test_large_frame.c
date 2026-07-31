/* Large-frame receive test: connect to a server that pushes a single
 * WS frame with a payload far larger than the client's recv buffer
 * (like Autobahn 9.1/9.2). The client must stream the frame across
 * many reads and deliver it as one message.
 *
 * Usage: test_large_frame <url> <expected_len> [text|binary]
 *   expected_len = exact payload length the message must have
 *   default kind = text ('A' payload); binary uses 0xAA bytes.
 */
#include "ws_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int got_message = 0;
static int got_close = 0;
static int got_error = 0;

static size_t recv_len = 0;
static size_t expected_len = 0;
static int expected_binary = 0;
static int content_ok = 0;
static int type_ok = 0;

static void on_open(ws_t *ws, void *userdata) {
  (void)ws; (void)userdata;
  connected = 1;
}

static void on_message(ws_t *ws, const char *data, size_t len, int binary, void *userdata) {
  (void)ws; (void)userdata;
  recv_len = len;
  got_message = 1;
  type_ok = (binary == expected_binary);
  content_ok = (len == expected_len);

  const unsigned char expect = expected_binary ? 0xAA : (unsigned char)'A';
  const unsigned char *payload = (const unsigned char *)data;
  for (size_t i = 0; content_ok && i < len; i++) {
    if (payload[i] != expect) content_ok = 0;
  }
}

static void on_close(ws_t *ws, uint16_t code, const char *reason, size_t reason_len, void *userdata) {
  (void)ws; (void)userdata;
  printf("[close] code=%u reason=%.*s\n", code, (int)reason_len, reason);
  got_close = 1;
}

static void on_error(ws_t *ws, const char *msg, void *userdata) {
  (void)ws; (void)userdata;
  printf("[error] %s\n", msg);
  got_error = 1;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <url> <expected_len> [text|binary]\n", argv[0]);
    return 1;
  }
  const char *url = argv[1];
  expected_len = (size_t)strtoull(argv[2], NULL, 10);
  if (argc > 3 && strcmp(argv[3], "binary") == 0) expected_binary = 1;

  ws_callbacks_t callbacks = { on_open, on_message, on_close, on_error, NULL };
  ws_t *ws = NULL;
  if (ws_connect(&ws, url, callbacks) != 0) {
    fprintf(stderr, "ws_connect failed\n");
    return 1;
  }
  printf("[info] Connecting to %s, expecting %zu-byte %s message ...\n",
         url, expected_len, expected_binary ? "binary" : "text");

  int timeout = 300; /* 100ms per poll, 30s total */
  while (!got_message && !got_close && !got_error && timeout-- > 0)
    ws_poll(ws, 100);

  int fail = 0;
  if (got_error) {
    fprintf(stderr, "[fail] error during test\n");
    fail = 1;
  } else if (!got_message) {
    fprintf(stderr, "[fail] timeout waiting for large frame\n");
    fail = 1;
  } else if (recv_len != expected_len) {
    fprintf(stderr, "[fail] length mismatch: got %zu, expected %zu\n", recv_len, expected_len);
    fail = 1;
  } else if (!type_ok) {
    fprintf(stderr, "[fail] message type mismatch: got %s, expected %s\n",
            expected_binary ? "text" : "binary",
            expected_binary ? "binary" : "text");
    fail = 1;
  } else if (!content_ok) {
    fprintf(stderr, "[fail] content mismatch (expected byte 0x%02X)\n",
            expected_binary ? 0xAA : (unsigned char)'A');
    fail = 1;
  } else {
    printf("[pass] received %zu-byte large %s frame\n", recv_len,
           expected_binary ? "binary" : "text");
  }

  ws_close(ws);
  for (int i = 0; i < 10 && ws_state(ws) == WS_STATE_CLOSING; i++)
    ws_poll(ws, 100);
  ws_destroy(ws);
  return fail ? 1 : 0;
}
