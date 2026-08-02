#include "ws_deflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define assert(condition)                                                    \
	do {                                                                       \
		if (!(condition)) {                                                      \
			fprintf(stderr, "assertion failed at %s:%d: %s\n", __FILE__, __LINE__, \
			        #condition);                                                   \
			exit(EXIT_FAILURE);                                                    \
		}                                                                        \
	} while (0)

static void assert_roundtrip(ws_deflate_compressor *compressor,
                             ws_deflate_decompressor *decompressor,
                             uint8_t const *input,
                             size_t input_size,
                             bool no_context_takeover) {
	static uint8_t const tail[] = {0x00, 0x00, 0xff, 0xff};
	uint8_t *compressed = NULL;
	uint8_t *decompressed = NULL;
	size_t compressed_size = 0;
	size_t decompressed_size = 0;

	assert(ws_deflate_compress(compressor, input, input_size, no_context_takeover,
	                           &compressed, &compressed_size) == WS_DEFLATE_OK);
	assert(compressed_size < sizeof(tail) ||
	       memcmp(compressed + compressed_size - sizeof(tail), tail,
	              sizeof(tail)) != 0);
	assert(ws_deflate_decompress(decompressor, compressed, compressed_size,
	                             input_size + 64, no_context_takeover,
	                             &decompressed,
	                             &decompressed_size) == WS_DEFLATE_OK);
	assert(decompressed_size == input_size);
	assert(input_size == 0 || memcmp(decompressed, input, input_size) == 0);
	ws_deflate_free(compressed);
	ws_deflate_free(decompressed);
}

int main(void) {
	static uint8_t const known[] = "The quick brown fox jumps over the lazy dog";
	static uint8_t const takeover[] =
	    "takeover dictionary payload takeover dictionary payload takeover "
	    "dictionary payload";
	static uint8_t const binary[] = {0x00, 0xff, 0x01, 0x00, 0x7f,
	                                 0x80, 0x42, 0x00, 0xfe};
	static uint8_t const malformed[] = {0xff, 0xff, 0xff};
	uint8_t incompressible[1024];
	ws_deflate_compressor *compressor = NULL;
	ws_deflate_decompressor *decompressor = NULL;
	uint8_t *compressed = NULL;
	uint8_t *second_compressed = NULL;
	uint8_t *output = NULL;
	size_t compressed_size = 0;
	size_t second_compressed_size = 0;
	size_t output_size = 0;
	size_t i;

	assert(ws_deflate_compressor_create(&compressor) == WS_DEFLATE_OK);
	assert(ws_deflate_decompressor_create(&decompressor) == WS_DEFLATE_OK);
	assert_roundtrip(compressor, decompressor, known, sizeof(known) - 1, false);
	assert(ws_deflate_compressor_reset(compressor) == WS_DEFLATE_OK);
	assert(ws_deflate_decompressor_reset(decompressor) == WS_DEFLATE_OK);
	assert(ws_deflate_compress(compressor, takeover, sizeof(takeover) - 1, false,
	                           &compressed, &compressed_size) == WS_DEFLATE_OK);
	assert(ws_deflate_decompress(decompressor, compressed, compressed_size,
	                             sizeof(takeover), false, &output,
	                             &output_size) == WS_DEFLATE_OK);
	ws_deflate_free(compressed);
	ws_deflate_free(output);
	compressed = NULL;
	output = NULL;
	assert(ws_deflate_compress(compressor, takeover, sizeof(takeover) - 1, false,
	                           &second_compressed,
	                           &second_compressed_size) == WS_DEFLATE_OK);
	assert(ws_deflate_decompress(decompressor, second_compressed,
	                             second_compressed_size, sizeof(takeover), false,
	                             &output, &output_size) == WS_DEFLATE_OK);
	assert(output_size == sizeof(takeover) - 1);
	assert(memcmp(output, takeover, output_size) == 0);
	ws_deflate_free(second_compressed);
	ws_deflate_free(output);
	second_compressed = NULL;
	output = NULL;
	assert(ws_deflate_compressor_reset(compressor) == WS_DEFLATE_OK);
	assert(ws_deflate_decompressor_reset(decompressor) == WS_DEFLATE_OK);
	assert_roundtrip(compressor, decompressor, NULL, 0, false);
	assert_roundtrip(compressor, decompressor, binary, sizeof(binary), false);

	for (i = 0; i < sizeof(incompressible); ++i)
		incompressible[i] = (uint8_t)((i * 73U + i / 7U) & 0xffU);
	assert_roundtrip(compressor, decompressor, incompressible,
	                 sizeof(incompressible), false);

	assert(ws_deflate_compressor_reset(compressor) == WS_DEFLATE_OK);
	assert(ws_deflate_decompressor_reset(decompressor) == WS_DEFLATE_OK);
	assert_roundtrip(compressor, decompressor, known, sizeof(known) - 1, true);
	assert_roundtrip(compressor, decompressor, known, sizeof(known) - 1, true);

	assert(ws_deflate_compress(compressor, known, sizeof(known) - 1, true,
	                           &compressed, &compressed_size) == WS_DEFLATE_OK);
	assert(ws_deflate_decompress(decompressor, compressed, compressed_size, 4,
	                             true, &output,
	                             &output_size) == WS_DEFLATE_OUTPUT_LIMIT);
	ws_deflate_free(compressed);
	compressed = NULL;
	assert(ws_deflate_decompressor_reset(decompressor) == WS_DEFLATE_OK);
	assert(ws_deflate_decompress(decompressor, malformed, sizeof(malformed), 1024,
	                             false, &output,
	                             &output_size) == WS_DEFLATE_CODEC_ERROR);

	ws_deflate_compressor_destroy(compressor);
	ws_deflate_decompressor_destroy(decompressor);
	ws_deflate_compressor_destroy(NULL);
	ws_deflate_decompressor_destroy(NULL);
	ws_deflate_free(NULL);
	puts("ws_deflate tests passed");
	return 0;
}
