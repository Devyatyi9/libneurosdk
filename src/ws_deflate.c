#include "ws_deflate.h"

#include "miniz.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define WS_DEFLATE_CHUNK ((size_t)UINT_MAX)
#define WS_DEFLATE_INITIAL_CAPACITY 256U

struct ws_deflate_compressor {
	mz_stream *stream;
};

struct ws_deflate_decompressor {
	mz_stream *stream;
};

static ws_deflate_status ws_deflate_grow(uint8_t **buffer,
                                         size_t *capacity,
                                         size_t used,
                                         size_t limit) {
	size_t next_capacity;
	void *next;

	if (*capacity >= limit)
		return WS_DEFLATE_OUTPUT_LIMIT;

	next_capacity = *capacity ? *capacity : WS_DEFLATE_INITIAL_CAPACITY;
	if (next_capacity > limit)
		next_capacity = limit;
	while (next_capacity <= used) {
		if (next_capacity > SIZE_MAX / 2) {
			if (limit == SIZE_MAX)
				return WS_DEFLATE_OVERFLOW;
			next_capacity = limit;
			break;
		}
		next_capacity *= 2;
		if (next_capacity > limit)
			next_capacity = limit;
	}
	if (next_capacity <= used)
		return WS_DEFLATE_OUTPUT_LIMIT;

	next = realloc(*buffer, next_capacity);
	if (!next)
		return WS_DEFLATE_OUT_OF_MEMORY;
	*buffer = next;
	*capacity = next_capacity;
	return WS_DEFLATE_OK;
}

static ws_deflate_status ws_deflate_init_stream(mz_stream **stream,
                                                bool compressor) {
	mz_stream *created;
	int result;

	if (*stream)
		return WS_DEFLATE_OK;
	created = calloc(1, sizeof(*created));
	if (!created)
		return WS_DEFLATE_OUT_OF_MEMORY;

	result = compressor
	             ? mz_deflateInit2(created, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
	                               -15, 9, MZ_DEFAULT_STRATEGY)
	             : mz_inflateInit2(created, -15);
	if (result != MZ_OK) {
		free(created);
		return result == MZ_MEM_ERROR ? WS_DEFLATE_OUT_OF_MEMORY
		                              : WS_DEFLATE_CODEC_ERROR;
	}
	*stream = created;
	return WS_DEFLATE_OK;
}

ws_deflate_status ws_deflate_compressor_create(
    ws_deflate_compressor **context) {
	if (!context)
		return WS_DEFLATE_INVALID_ARGUMENT;
	*context = calloc(1, sizeof(**context));
	return *context ? WS_DEFLATE_OK : WS_DEFLATE_OUT_OF_MEMORY;
}

ws_deflate_status ws_deflate_decompressor_create(
    ws_deflate_decompressor **context) {
	if (!context)
		return WS_DEFLATE_INVALID_ARGUMENT;
	*context = calloc(1, sizeof(**context));
	return *context ? WS_DEFLATE_OK : WS_DEFLATE_OUT_OF_MEMORY;
}

void ws_deflate_compressor_destroy(ws_deflate_compressor *context) {
	if (!context)
		return;
	if (context->stream) {
		mz_deflateEnd(context->stream);
		free(context->stream);
	}
	free(context);
}

void ws_deflate_decompressor_destroy(ws_deflate_decompressor *context) {
	if (!context)
		return;
	if (context->stream) {
		mz_inflateEnd(context->stream);
		free(context->stream);
	}
	free(context);
}

ws_deflate_status ws_deflate_compressor_reset(ws_deflate_compressor *context) {
	if (!context)
		return WS_DEFLATE_INVALID_ARGUMENT;
	if (!context->stream)
		return WS_DEFLATE_OK;
	return mz_deflateReset(context->stream) == MZ_OK ? WS_DEFLATE_OK
	                                                 : WS_DEFLATE_CODEC_ERROR;
}

ws_deflate_status ws_deflate_decompressor_reset(
    ws_deflate_decompressor *context) {
	if (!context)
		return WS_DEFLATE_INVALID_ARGUMENT;
	if (!context->stream)
		return WS_DEFLATE_OK;
	return mz_inflateReset(context->stream) == MZ_OK ? WS_DEFLATE_OK
	                                                 : WS_DEFLATE_CODEC_ERROR;
}

ws_deflate_status ws_deflate_compress(ws_deflate_compressor *context,
                                      uint8_t const *input,
                                      size_t input_size,
                                      bool no_context_takeover,
                                      uint8_t **output,
                                      size_t *output_size) {
	static uint8_t const tail[] = {0x00, 0x00, 0xff, 0xff};
	static uint8_t const empty_input = 0;
	uint8_t *buffer = NULL;
	size_t capacity = 0;
	size_t used = 0;
	size_t consumed = 0;
	ws_deflate_status status;
	int result = MZ_OK;

	if (!context || (!input && input_size) || !output || !output_size)
		return WS_DEFLATE_INVALID_ARGUMENT;
	*output = NULL;
	*output_size = 0;
	status = ws_deflate_init_stream(&context->stream, true);
	if (status != WS_DEFLATE_OK)
		return status;

	do {
		size_t remaining = input_size - consumed;
		mz_uint input_chunk =
		    (mz_uint)(remaining > WS_DEFLATE_CHUNK ? WS_DEFLATE_CHUNK : remaining);
		status = ws_deflate_grow(&buffer, &capacity, used, SIZE_MAX);
		if (status != WS_DEFLATE_OK)
			goto fail;
		context->stream->next_in = input ? input + consumed : &empty_input;
		context->stream->avail_in = input_chunk;
		context->stream->next_out = buffer + used;
		context->stream->avail_out =
		    (mz_uint)((capacity - used) > WS_DEFLATE_CHUNK ? WS_DEFLATE_CHUNK
		                                                   : capacity - used);
		result = mz_deflate(context->stream, consumed + input_chunk == input_size
		                                         ? MZ_SYNC_FLUSH
		                                         : MZ_NO_FLUSH);
		consumed += input_chunk - context->stream->avail_in;
		used = (size_t)(context->stream->next_out - buffer);
		if (result != MZ_OK)
			break;
	} while (consumed < input_size || context->stream->avail_out == 0);

	if (result != MZ_OK || consumed != input_size || used < sizeof(tail) ||
	    memcmp(buffer + used - sizeof(tail), tail, sizeof(tail)) != 0) {
		status = WS_DEFLATE_CODEC_ERROR;
		goto fail;
	}
	used -= sizeof(tail);
	if (!used) {
		free(buffer);
		buffer = NULL;
	}
	if (no_context_takeover &&
	    ws_deflate_compressor_reset(context) != WS_DEFLATE_OK) {
		status = WS_DEFLATE_CODEC_ERROR;
		goto fail;
	}
	*output = buffer;
	*output_size = used;
	return WS_DEFLATE_OK;

fail:
	free(buffer);
	return status;
}

ws_deflate_status ws_deflate_decompress(ws_deflate_decompressor *context,
                                        uint8_t const *input,
                                        size_t input_size,
                                        size_t output_limit,
                                        bool no_context_takeover,
                                        uint8_t **output,
                                        size_t *output_size) {
	static uint8_t const tail[] = {0x00, 0x00, 0xff, 0xff};
	uint8_t *buffer = NULL;
	size_t capacity = 0;
	size_t used = 0;
	size_t consumed = 0;
	size_t tail_consumed = 0;
	ws_deflate_status status;
	int result = MZ_OK;

	if (!context || (!input && input_size) || !output || !output_size)
		return WS_DEFLATE_INVALID_ARGUMENT;
	*output = NULL;
	*output_size = 0;
	status = ws_deflate_init_stream(&context->stream, false);
	if (status != WS_DEFLATE_OK)
		return status;

	for (;;) {
		uint8_t const *source;
		size_t source_left;
		uint8_t probe;
		mz_uint output_available;
		uint8_t *output_next;

		if (consumed < input_size) {
			source = input + consumed;
			source_left = input_size - consumed;
		} else {
			source = tail + tail_consumed;
			source_left = sizeof(tail) - tail_consumed;
		}

		if (used == capacity && capacity < output_limit) {
			status = ws_deflate_grow(&buffer, &capacity, used, output_limit);
			if (status != WS_DEFLATE_OK)
				goto fail;
		}
		if (used < capacity) {
			output_next = buffer + used;
			output_available =
			    (mz_uint)((capacity - used) > WS_DEFLATE_CHUNK ? WS_DEFLATE_CHUNK
			                                                   : capacity - used);
		} else {
			output_next = &probe;
			output_available = 1;
		}

		context->stream->next_in = source;
		context->stream->avail_in =
		    (mz_uint)(source_left > WS_DEFLATE_CHUNK ? WS_DEFLATE_CHUNK
		                                             : source_left);
		context->stream->next_out = output_next;
		context->stream->avail_out = output_available;
		result = mz_inflate(context->stream, MZ_SYNC_FLUSH);
		if (consumed < input_size)
			consumed +=
			    (source_left > WS_DEFLATE_CHUNK ? WS_DEFLATE_CHUNK : source_left) -
			    context->stream->avail_in;
		else
			tail_consumed +=
			    (source_left > WS_DEFLATE_CHUNK ? WS_DEFLATE_CHUNK : source_left) -
			    context->stream->avail_in;

		if (output_next == &probe) {
			if (context->stream->avail_out == 0) {
				status = WS_DEFLATE_OUTPUT_LIMIT;
				goto fail;
			}
		} else {
			used += output_available - context->stream->avail_out;
		}

		if (result != MZ_OK && result != MZ_BUF_ERROR) {
			status = WS_DEFLATE_CODEC_ERROR;
			goto fail;
		}
		if (consumed == input_size && tail_consumed == sizeof(tail))
			break;
		if (result == MZ_BUF_ERROR && context->stream->avail_in != 0) {
			status = WS_DEFLATE_CODEC_ERROR;
			goto fail;
		}
	}

	if (!used) {
		free(buffer);
		buffer = NULL;
	}
	if (no_context_takeover &&
	    ws_deflate_decompressor_reset(context) != WS_DEFLATE_OK) {
		status = WS_DEFLATE_CODEC_ERROR;
		goto fail;
	}
	*output = buffer;
	*output_size = used;
	return WS_DEFLATE_OK;

fail:
	free(buffer);
	return status;
}

void ws_deflate_free(void *buffer) {
	free(buffer);
}
