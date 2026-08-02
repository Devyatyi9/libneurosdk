#ifndef WS_DEFLATE_H
#define WS_DEFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ws_deflate_compressor ws_deflate_compressor;
typedef struct ws_deflate_decompressor ws_deflate_decompressor;

typedef enum ws_deflate_status {
	WS_DEFLATE_OK = 0,
	WS_DEFLATE_INVALID_ARGUMENT,
	WS_DEFLATE_OUT_OF_MEMORY,
	WS_DEFLATE_OUTPUT_LIMIT,
	WS_DEFLATE_CODEC_ERROR,
	WS_DEFLATE_OVERFLOW
} ws_deflate_status;

/* Context objects are small; the codec state is allocated on first use. */
ws_deflate_status ws_deflate_compressor_create(ws_deflate_compressor **context,
                                               unsigned int window_bits);
ws_deflate_status ws_deflate_decompressor_create(
    ws_deflate_decompressor **context);

void ws_deflate_compressor_destroy(ws_deflate_compressor *context);
void ws_deflate_decompressor_destroy(ws_deflate_decompressor *context);

ws_deflate_status ws_deflate_compressor_reset(ws_deflate_compressor *context);
ws_deflate_status ws_deflate_decompressor_reset(
    ws_deflate_decompressor *context);

/*
 * Successful calls return a malloc-allocated buffer in output (or NULL for an
 * empty decompressed message). The caller owns it and must call
 * ws_deflate_free(). Input may be NULL only when input_size is zero.
 *
 * no_context_takeover resets the direction after this message succeeds.
 */
ws_deflate_status ws_deflate_compress(ws_deflate_compressor *context,
                                      uint8_t const *input,
                                      size_t input_size,
                                      bool no_context_takeover,
                                      uint8_t **output,
                                      size_t *output_size);
ws_deflate_status ws_deflate_decompress(ws_deflate_decompressor *context,
                                        uint8_t const *input,
                                        size_t input_size,
                                        size_t output_limit,
                                        bool no_context_takeover,
                                        uint8_t **output,
                                        size_t *output_size);

void ws_deflate_free(void *buffer);

#endif
