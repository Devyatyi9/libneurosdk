#ifndef NEUROSDK_PROTOCOL_JSON_H
#define NEUROSDK_PROTOCOL_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include <neurosdk.h>

#define PROTOCOL_MESSAGE_MAX_SIZE (16U * 1024U * 1024U)

bool protocol_json_validate_text(char const *json, size_t size);

neurosdk_error_e protocol_json_build_c2s(char const *game_name,
	                                     neurosdk_message_t const *message,
	                                     char **result);

#endif
