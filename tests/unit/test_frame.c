#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ws_client.c"

static int failed = 0;

static void test(char const *name, int cond) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", name);
		failed = 1;
	} else {
		printf("  OK: %s\n", name);
	}
}

static int parse_extensions(char const *extension,
                            int offered,
                            struct ws_permessage_deflate *agreed) {
	char response[512];
	int len;
	if (extension) {
		len = snprintf(response, sizeof(response),
		               "HTTP/1.1 101 Switching Protocols\r\n"
		               "Sec-WebSocket-Extensions: %s\r\n\r\n",
		               extension);
	} else {
		len = snprintf(response, sizeof(response),
		               "HTTP/1.1 101 Switching Protocols\r\n\r\n");
	}
	return parse_permessage_deflate_response(response, (size_t)len, offered,
	                                         agreed);
}

/* Parse a frame header (reverse of ws_build_frame_header).
 * Returns header length (incl. 4 mask bytes) or -1. */
static int parse_frame_header(unsigned char const *buf,
                              size_t buflen,
                              int *fin,
                              int *opcode,
                              int *masked,
                              uint64_t *payload_len) {
	if (buflen < 2)
		return -1;
	*fin = (buf[0] & 0x80) != 0;
	*opcode = buf[0] & 0x0f;
	*masked = (buf[1] & 0x80) != 0;
	uint64_t len = buf[1] & 0x7f;
	size_t hlen;
	if (len < 126) {
		hlen = 2;
	} else if (len == 126) {
		if (buflen < 4)
			return -1;
		len = ((uint64_t)buf[2] << 8) | buf[3];
		hlen = 4;
	} else {
		if (buflen < 10)
			return -1;
		len = 0;
		for (int i = 0; i < 8; i++)
			len = (len << 8) | buf[2 + i];
		hlen = 10;
	}
	hlen += *masked ? 4 : 0;
	*payload_len = len;
	return (int)hlen;
}

int main(void) {
	unsigned char hdr[16];
	int fin, opcode, masked;
	uint64_t plen;
	int hlen;

	/* Text frame, small payload */
	hlen = (int)ws_build_frame_header(hdr, 0x1, 1, 42);
	test("text frame header length 2", hlen == 2);
	test("text frame FIN+opcode", hdr[0] == 0x81);
	test("text frame MASK bit set", (hdr[1] & 0x80) != 0);
	test("text frame length", (hdr[1] & 0x7f) == 42);

	hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &masked, &plen);
	test("parse FIN", fin == 1);
	test("parse opcode text", opcode == 0x1);
	test("parse masked", masked == 1);
	test("parse length 42", plen == 42);
	test("parse hlen 6 (incl mask)", hlen == 6);

	/* Binary frame, medium payload (126-65535) */
	hlen = (int)ws_build_frame_header(hdr, 0x2, 1, 500);
	test("medium frame header length 4", hlen == 4);
	test("medium frame FIN+opcode", hdr[0] == 0x82);
	test("medium frame extlen 126", (hdr[1] & 0x7f) == 126);
	test("medium frame len hi", hdr[2] == (500 >> 8));
	test("medium frame len lo", hdr[3] == (500 & 0xff));

	hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &masked, &plen);
	test("parse medium FIN", fin == 1);
	test("parse medium opcode", opcode == 0x2);
	test("parse medium length 500", plen == 500);

	/* Large payload (>65535) */
	hlen = (int)ws_build_frame_header(hdr, 0x1, 1, 100000);
	test("large frame header length 10", hlen == 10);
	test("large frame extlen 127", (hdr[1] & 0x7f) == 127);
	test("large frame length correct", (hdr[2] == 0 && hdr[9] == 160));

	hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &masked, &plen);
	test("parse large length 100000", plen == 100000);

	/* Ping frame */
	hlen = (int)ws_build_frame_header(hdr, 0x9, 1, 0);
	test("ping header length 2", hlen == 2);
	test("ping opcode 0x9", hdr[0] == 0x89);
	test("ping zero length", (hdr[1] & 0x7f) == 0);
	test("ping MASK bit set", (hdr[1] & 0x80) != 0);

	hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &masked, &plen);
	test("parse ping FIN", fin == 1);
	test("parse ping opcode", opcode == 0x9);
	test("parse ping length 0", plen == 0);

	/* Control frame with payload, FIN=0 */
	hlen = (int)ws_build_frame_header(hdr, 0xA, 0, 4);
	test("pong header length 2", hlen == 2);
	test("pong FIN=0 bit", (hdr[0] & 0x80) == 0);
	test("pong opcode 0xA", hdr[0] == 0x0A);

	hlen = parse_frame_header(hdr, sizeof(hdr), &fin, &opcode, &masked, &plen);
	test("parse pong FIN=0", fin == 0);
	test("parse pong opcode", opcode == 0xA);
	test("parse pong length 4", plen == 4);

	/* Close frame */
	hlen = (int)ws_build_frame_header(hdr, 0x8, 1, 2);
	test("close header opcode", hdr[0] == 0x88);
	test("close length 2", (hdr[1] & 0x7f) == 2);

	/* permessage-deflate puts RSV1 on data frames before masking. */
	hlen = (int)ws_build_data_frame_header(hdr, 0x1, 1, 12);
	test("compressed text RSV1 header", hlen == 2 && hdr[0] == 0xc1);
	hlen = (int)ws_build_data_frame_header(hdr, 0x2, 0, 12);
	test("uncompressed binary has no RSV1", hlen == 2 && hdr[0] == 0x82);

	{
		char request[1024];
		int request_len = build_upgrade_request(request, sizeof(request),
		                                        "localhost", 80, "/", "key");
#ifdef WS_ENABLE_PERMESSAGE_DEFLATE
		test("exact permessage-deflate offer",
		     request_len > 0 &&
		         strstr(request,
		                "Sec-WebSocket-Extensions: permessage-deflate\r\n") !=
		             NULL &&
		         strstr(request, "client_max_window_bits") == NULL);
#else
		test(
		    "disabled build has no permessage-deflate offer",
		    request_len > 0 && strstr(request, "Sec-WebSocket-Extensions") == NULL);
#endif
	}

	{
		struct ws_permessage_deflate agreed;
		test("extension decline accepted",
		     parse_extensions(NULL, 1, &agreed) == 0 && !agreed.negotiated);
		test("bare permessage-deflate accepted",
		     parse_extensions("permessage-deflate", 1, &agreed) == 0 &&
		         agreed.negotiated);
		test("parameterized response accepted",
		     parse_extensions(
		         "PerMessage-Deflate; CLIENT_NO_CONTEXT_TAKEOVER; "
		         "server_no_context_takeover; server_max_window_bits = 12",
		         1, &agreed) == 0 &&
		         agreed.client_no_context_takeover &&
		         agreed.server_no_context_takeover &&
		         agreed.server_max_window_bits == 12);
		test("duplicate parameter rejected",
		     parse_extensions("permessage-deflate; server_no_context_takeover; "
		                      "SERVER_NO_CONTEXT_TAKEOVER",
		                      1, &agreed) < 0);
		test("unknown parameter rejected",
		     parse_extensions("permessage-deflate; unknown", 1, &agreed) < 0);
		test("malformed quoted value rejected",
		     parse_extensions("permessage-deflate; server_max_window_bits=\"12\"",
		                      1, &agreed) < 0);
		test("missing server window value rejected",
		     parse_extensions("permessage-deflate; server_max_window_bits=", 1,
		                      &agreed) < 0);
		test("missing parameter separator rejected",
		     parse_extensions("permessage-deflate server_no_context_takeover", 1,
		                      &agreed) < 0);
		test("extension list rejected",
		     parse_extensions("permessage-deflate, permessage-deflate", 1,
		                      &agreed) < 0);
		{
			char const duplicate_headers[] =
			    "HTTP/1.1 101 Switching Protocols\r\n"
			    "Sec-WebSocket-Extensions: permessage-deflate\r\n"
			    "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
			test("duplicate extension header rejected",
			     parse_permessage_deflate_response(duplicate_headers,
			                                       sizeof(duplicate_headers) - 1, 1,
			                                       &agreed) < 0);
		}
		test("unsolicited response rejected",
		     parse_extensions("permessage-deflate", 0, &agreed) < 0);
		test("client_max_window_bits response rejected",
		     parse_extensions("permessage-deflate; client_max_window_bits=15", 1,
		                      &agreed) < 0);
		test("out of range server window rejected",
		     parse_extensions("permessage-deflate; server_max_window_bits=7", 1,
		                      &agreed) < 0);
		test("non-canonical server window rejected",
		     parse_extensions("permessage-deflate; server_max_window_bits=08", 1,
		                      &agreed) < 0);
	}

	/* Masking: apply -> unmask round trip */
	{
		unsigned char const mask[4] = {0x0F, 0xF0, 0x5A, 0xA5};
		unsigned char const data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
		unsigned char masked_buf[sizeof(data)];
		unsigned char unmasked[sizeof(data)];
		ws_apply_mask(data, sizeof(data), mask, masked_buf);
		test("mask key rotates i&3", masked_buf[0] == (data[0] ^ mask[0]) &&
		                                 masked_buf[1] == (data[1] ^ mask[1]) &&
		                                 masked_buf[2] == (data[2] ^ mask[2]) &&
		                                 masked_buf[3] == (data[3] ^ mask[3]));
		test("mask key wraps at 4", masked_buf[4] == (data[4] ^ mask[0]));
		ws_apply_mask(masked_buf, sizeof(data), mask, unmasked);
		test("unmask round trip", memcmp(unmasked, data, sizeof(data)) == 0);
	}

	/* Masking: len 0 and len not multiple of 4 */
	{
		unsigned char const mask[4] = {0x01, 0x02, 0x03, 0x04};
		unsigned char const data[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
		unsigned char masked_buf[sizeof(data)];
		ws_apply_mask(data, 0, mask, masked_buf);
		test("mask len 0 no-op", 1);
		ws_apply_mask(data, sizeof(data), mask, masked_buf);
		test("mask len 6", masked_buf[5] == (data[5] ^ mask[1]));
	}

	/* Incomplete header */
	hlen = parse_frame_header(hdr, 1, &fin, &opcode, &masked, &plen);
	test("incomplete header returns -1", hlen == -1);

	return failed ? 1 : 0;
}
