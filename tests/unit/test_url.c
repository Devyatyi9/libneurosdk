#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/ws_client.c"

static int failed = 0;

static void test(const char *name, int cond) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", name); failed = 1; }
    else       { printf("  OK: %s\n", name); }
}

int main(void) {
    char *host, *path;
    uint16_t port;

    test("basic ws://host:port/path",
        parse_url("ws://localhost:9001/echo", &host, &port, &path) == 0 &&
        strcmp(host, "localhost") == 0 && port == 9001 &&
        strcmp(path, "/echo") == 0);
    if (host) free(host);
    if (path) free(path); host = NULL; path = NULL;

    test("default port 80",
        parse_url("ws://example.com/", &host, &port, &path) == 0 &&
        strcmp(host, "example.com") == 0 && port == 80 &&
        strcmp(path, "/") == 0);
    if (host) free(host);
    if (path) free(path); host = NULL; path = NULL;

    test("no path gets slash",
        parse_url("ws://localhost", &host, &port, &path) == 0 &&
        port == 80 && strcmp(path, "/") == 0);
    if (host) free(host);
    if (path) free(path); host = NULL; path = NULL;

    test("query params preserved",
        parse_url("ws://host:9001/a/b?k=v", &host, &port, &path) == 0 &&
        strcmp(path, "/a/b?k=v") == 0);
    if (host) free(host);
    if (path) free(path); host = NULL; path = NULL;

    test("missing ws:// prefix",
        parse_url("http://localhost/", &host, &port, &path) != 0);
    test("host NULL after prefix error", host == NULL);
    test("path NULL after prefix error", path == NULL);

    test("empty host",
        parse_url("ws:///path", &host, &port, &path) != 0);
    test("host NULL after empty host", host == NULL);
    test("path NULL after empty host", path == NULL);

    test("port zero",
        parse_url("ws://host:0/path", &host, &port, &path) != 0);
    test("host NULL after port zero", host == NULL);
    test("path NULL after port zero", path == NULL);

    test("port too large",
        parse_url("ws://host:99999/path", &host, &port, &path) != 0);
    test("host NULL after port too large", host == NULL);
    test("path NULL after port too large", path == NULL);

    test("port negative",
        parse_url("ws://host:-1/path", &host, &port, &path) != 0);
    test("host NULL after port negative", host == NULL);
    test("path NULL after port negative", path == NULL);

    test("long host",
        parse_url("ws://0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789:8080/x", &host, &port, &path) == 0 &&
        strlen(host) == 100 && port == 8080);
    if (host) free(host);
    if (path) free(path);

    return failed ? 1 : 0;
}
