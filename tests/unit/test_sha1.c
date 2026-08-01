#include <stdio.h>
#include <string.h>
#include "../../src/ws_client.c"

static int failed = 0;

static void hex(unsigned char const *in, char *out) {
	for (int i = 0; i < 20; i++)
		out += sprintf(out, "%02x", in[i]);
}

static void test(char const *name,
                 unsigned char const *got,
                 char const *expected_hex) {
	char got_hex[41];
	hex(got, got_hex);
	got_hex[40] = '\0';
	if (strcmp(got_hex, expected_hex) != 0) {
		fprintf(stderr, "FAIL: %s\ngot:      %s\nexpected: %s\n", name, got_hex,
		        expected_hex);
		failed = 1;
	} else {
		printf("  OK: %s\n", name);
	}
}

int main(void) {
	unsigned char out[20];
	struct sha1_ctx ctx;

	/* RFC 3174 test vectors */
	/* 1. "abc" */
	sha1_init(&ctx);
	sha1_update(&ctx, (unsigned char const *)"abc", 3);
	sha1_final(&ctx, out);
	test("abc", out, "a9993e364706816aba3e25717850c26c9cd0d89d");

	/* 2. empty string */
	sha1_init(&ctx);
	sha1_update(&ctx, (unsigned char const *)"", 0);
	sha1_final(&ctx, out);
	test("empty", out, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

	/* 3. "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" */
	sha1_init(&ctx);
	sha1_update(&ctx,
	            (unsigned char const
	                 *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	            56);
	sha1_final(&ctx, out);
	test("abcdbcdecdef...nopq", out, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

	/* 4. one million 'a' — test streaming with multiple update calls */
	sha1_init(&ctx);
	for (int i = 0; i < 1000000; i += 1000)
		sha1_update(&ctx, (unsigned char const*)"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1000);
	sha1_final(&ctx, out);
	test("1M 'a'", out, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

	return failed ? 1 : 0;
}
