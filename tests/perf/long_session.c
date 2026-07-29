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
static int got_error = 0;
static unsigned long sent = 0;
static unsigned long received = 0;

static void on_open(ws_t *ws, void *userdata) {
  (void)ws; (void)userdata;
  printf("[open] Connection established\n");
  connected = 1;
}

static void on_message(ws_t *ws, const char *data, size_t len, int binary, void *userdata) {
  (void)ws; (void)userdata; (void)data; (void)binary;
  received++;
}

static void on_close(ws_t *ws, uint16_t code, const char *reason, size_t reason_len, void *userdata) {
  (void)ws; (void)userdata;
  printf("[close] code=%u reason=%.*s (sent=%lu recv=%lu)\n",
         code, (int)reason_len, reason, sent, received);
}

static void on_error(ws_t *ws, const char *msg, void *userdata) {
  (void)ws; (void)userdata;
  printf("[error] %s\n", msg);
  got_error = 1;
}

int main(int argc, char *argv[]) {
  const char *url = argc > 1 ? argv[1] : "ws://localhost:9001/";
  int interval_ms = argc > 2 ? atoi(argv[2]) * 1000 : 60000;
  int duration_h = argc > 3 ? atoi(argv[3]) : 3;

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

  while (!connected && !got_error) ws_poll(ws, 100);
  if (got_error) { ws_destroy(ws); return 1; }

  time_t end = time(NULL) + (time_t)duration_h * 3600;
  printf("[info] Connected. Sending every %dms for %dh\n", interval_ms, duration_h);

  while (time(NULL) < end && !got_error) {
    const char *msg = "ping";
    if (ws_send(ws, msg, strlen(msg)) != 0) {
      printf("[error] ws_send failed\n");
      break;
    }
    sent++;

    /* Wait for echo (up to interval_ms) */
    int waited = 0;
    while (received < sent && waited < interval_ms && !got_error) {
      ws_poll(ws, 100);
      waited += 100;
    }

    if (got_error) break;
    if (received < sent) printf("[warn] echo not received for message %lu\n", sent);

    /* Wait remainder of interval */
    int remaining = interval_ms - waited;
    if (remaining > 0 && !got_error) {
      int step = remaining < 100 ? remaining : 100;
      for (int i = 0; i < remaining / step && !got_error; i++)
        ws_poll(ws, step);
    }
  }

  printf("[info] Closing (sent=%lu recv=%lu errors=%d)\n", sent, received, got_error);
  ws_close(ws);
  for (int i = 0; i < 10 && ws_state(ws) == WS_STATE_CLOSING; i++)
    ws_poll(ws, 100);
  ws_destroy(ws);
  return got_error ? 1 : 0;
}
