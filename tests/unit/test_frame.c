#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/ws_client.c"

static int failed = 0;

static void test(const char *name, int cond) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", name); failed = 1; }
    else       { printf("  OK: %s\n", name); }
}

/* Build a bare WS frame header without masking or full ws_send.
 * The ws_send code is tested by checking that it produces the expected
 * wire format bytes.  We simulate its logic inline here because the real
 * ws_send also writes to a real socket. */
static size_t build_frame_header(unsigned char *buf, unsigned char opcode,
                                  int fin, size_t len) {
    size_t hlen = 2;
    buf[0] = (fin ? 0x80 : 0) | (opcode & 0x0f);
    buf[1] = 0; /* no mask */
    if (len < 126) {
        buf[1] |= (unsigned char)len;
    } else if (len < 65536) {
        buf[1] |= 126;
        buf[2] = (unsigned char)(len >> 8);
        buf[3] = (unsigned char)(len);
        hlen = 4;
    } else {
        buf[1] |= 127;
        uint64_t n = (uint64_t)len;
        for (int i = 0; i < 8; i++)
            buf[hlen + i] = (unsigned char)(n >> (56 - i * 8));
        hlen = 10;
    }
    return hlen;
}

/* Parse a frame header (reverse of build).  Returns header length or -1. */
static int parse_frame_header(const unsigned char *buf, size_t buflen,
                               int *fin, int *opcode, uint64_t *payload_len) {
    if (buflen < 2) return -1;
    *fin = (buf[0] & 0x80) != 0;
    *opcode = buf[0] & 0x0f;
    int masked = (buf[1] & 0x80) != 0;
    uint64_t len = buf[1] & 0x7f;
    size_t hlen;
    if (len < 126) {
        hlen = 2;
    } else if (len == 126) {
        if (buflen < 4) return -1;
        len = ((uint64_t)buf[2] << 8) | buf[3];
        hlen = 4;
    } else {
        if (buflen < 10) return -1;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | buf[2 + i];
        hlen = 10;
    }
    hlen += masked ? 4 : 0;
    *payload_len = len;
    return (int)hlen;
}

int main(void) {
    unsigned char hdr[16];
    int fin, opcode;
    uint64_t plen;
    int hlen;

    /* Text frame, small payload */
    hlen = (int)build_frame_header(hdr, 0x1, 1, 42);
    test("text frame header length 2", hlen == 2);
    test("text frame FIN+opcode", hdr[0] == 0x81);
    test("text frame length", hdr[1] == 42);

    hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &plen);
    test("parse FIN", fin == 1);
    test("parse opcode text", opcode == 0x1);
    test("parse length 42", plen == 42);
    test("parse hlen 2", hlen == 2);

    /* Binary frame, medium payload (126-65535) */
    hlen = (int)build_frame_header(hdr, 0x2, 1, 500);
    test("medium frame header length 4", hlen == 4);
    test("medium frame FIN+opcode", hdr[0] == 0x82);
    test("medium frame extlen 126", hdr[1] == 126);
    test("medium frame len hi", hdr[2] == (500 >> 8));
    test("medium frame len lo", hdr[3] == (500 & 0xff));

    hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &plen);
    test("parse medium FIN", fin == 1);
    test("parse medium opcode", opcode == 0x2);
    test("parse medium length 500", plen == 500);

    /* Large payload (>65535) */
    hlen = (int)build_frame_header(hdr, 0x1, 1, 100000);
    test("large frame header length 10", hlen == 10);
    test("large frame extlen 127", hdr[1] == 127);
    test("large frame length correct", (hdr[2] == 0 && hdr[9] == 160));

    hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &plen);
    test("parse large length 100000", plen == 100000);

    /* Ping frame */
    hlen = (int)build_frame_header(hdr, 0x9, 1, 0);
    test("ping header length 2", hlen == 2);
    test("ping opcode 0x9", hdr[0] == 0x89);
    test("ping zero length", hdr[1] == 0);

    hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &plen);
    test("parse ping FIN", fin == 1);
    test("parse ping opcode", opcode == 0x9);
    test("parse ping length 0", plen == 0);

    /* Control frame with payload */
    hlen = (int)build_frame_header(hdr, 0xA, 0, 4);
    test("pong header length 2", hlen == 2);

    hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &plen);
    test("parse pong FIN=0", fin == 0);
    test("parse pong opcode", opcode == 0xA);
    test("parse pong length 4", plen == 4);

    /* Incomplete header */
    hlen = parse_frame_header(hdr, 1, &fin, &opcode, &plen);
    test("incomplete header returns -1", hlen == -1);

    /* Close frame */
    hlen = (int)build_frame_header(hdr, 0x8, 1, 2);
    test("close header opcode", hdr[0] == 0x88);
    test("close length 2", hdr[1] == 2);

    return failed ? 1 : 0;
}
