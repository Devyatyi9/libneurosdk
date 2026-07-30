#include <stdio.h>
#include <string.h>
#include "../../src/ws_client.c"

static int failed = 0;

static void test(const char *name, const unsigned char *in, size_t inlen, const char *expected) {
    char out[128];
    base64_encode(in, inlen, out, sizeof(out));
    if (strcmp(out, expected) != 0) {
        fprintf(stderr, "FAIL: %s\ngot:      '%s'\nexpected: '%s'\n", name, out, expected);
        failed = 1;
    } else {
        printf("  OK: %s\n", name);
    }
}

int main(void) {
    /* RFC 4648 test vectors */
    test("empty", (const unsigned char*)"", 0, "");
    test("f",     (const unsigned char*)"f", 1, "Zg==");
    test("fo",    (const unsigned char*)"fo", 2, "Zm8=");
    test("foo",   (const unsigned char*)"foo", 3, "Zm9v");
    test("foob",  (const unsigned char*)"foob", 4, "Zm9vYg==");
    test("fooba", (const unsigned char*)"fooba", 5, "Zm9vYmE=");
    test("foobar",(const unsigned char*)"foobar", 6, "Zm9vYmFy");

    /* All bytes 0x00-0xFF */
    unsigned char all[256];
    for (int i = 0; i < 256; i++) all[i] = (unsigned char)i;
    char out_all[512];
    base64_encode(all, 256, out_all, sizeof(out_all));
    /* Expected: known base64 of 0x00..0xFF */
    const char *expected =
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v"
        "MDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f"
        "YGFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6P"
        "kJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmqq6ytrq+wsbKztLW2t7i5uru8vb6/"
        "wMHCw8TFxsfIycrLzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj5OXm5+jp6uvs7e7v"
        "8PHy8/T19vf4+fr7/P3+/w==";
    if (strcmp(out_all, expected) != 0) {
        fprintf(stderr, "FAIL: 0x00-0xFF\ngot length=%zu, expected length=%zu\n", strlen(out_all), strlen(expected));
        fprintf(stderr, "got:      '%s'\nexpected: '%s'\n", out_all, expected);
        failed = 1;
    } else {
        printf("  OK: 0x00-0xFF\n");
    }

    /* 16-byte nonce (Sec-WebSocket-Key length) */
    unsigned char nonce[16];
    memset(nonce, 0xAB, 16);
    test("16-byte key", nonce, 16, "q6urq6urq6urq6urq6urqw==");

    return failed ? 1 : 0;
}
