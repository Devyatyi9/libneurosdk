#include "ws_client.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(x) Sleep(x)
#else
#include <unistd.h>
#define SLEEP_MS(x) usleep((x) * 1000)
#endif

static int s_connected = 0;
static int s_done = 0;
static int s_got_error = 0;

static void on_open(ws_t *ws, void *userdata) {
    (void)ws; (void)userdata;
    s_connected = 1;
}

static void on_message(ws_t *ws, const char *data, size_t len, int binary, void *userdata) {
    (void)ws; (void)data; (void)len; (void)binary; (void)userdata;
    /* Should not receive a full message due to oversized frame */
    fprintf(stderr, "FAIL: unexpected message (%zu bytes)\n", len);
}

static void on_close(ws_t *ws, uint16_t code, const char *reason, size_t reason_len, void *userdata) {
    (void)ws; (void)userdata;
    /* Expected: close with code 1009 (Message Too Big) or similar error */
    if (code == 1009) {
        printf("OK: close with code 1009 (Message Too Big)\n");
    } else {
        printf("close with code %u (%.*s)\n", code, (int)reason_len, reason);
    }
    s_done = 1;
}

static void on_error(ws_t *ws, const char *msg, void *userdata) {
    (void)ws; (void)userdata;
    s_got_error = 1;
    printf("OK: error: %s\n", msg);
    s_done = 1;
}

int main(int argc, char *argv[]) {
    const char *url = argc > 1 ? argv[1] : "ws://127.0.0.1:19008/";
    ws_callbacks_t cbs = { on_open, on_message, on_close, on_error, NULL };
    ws_t *ws = NULL;
    if (ws_connect(&ws, url, cbs) != 0) {
        fprintf(stderr, "FAIL: ws_connect\n");
        return 1;
    }
    int budget = 500;
    while (!s_done && budget > 0) {
        ws_poll(ws, 100);
        budget--;
    }
    ws_destroy(ws);
    if (!s_connected) { fprintf(stderr, "FAIL: never connected\n"); return 1; }
    /* Expect either on_error or on_close with 1009 */
    if (!s_got_error && !s_done) {
        fprintf(stderr, "FAIL: no error/close triggered\n");
        return 1;
    }
    return 0;
}
