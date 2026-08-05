#include <neurosdk.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 5105)
#endif
#include "tinycthread.h"  // IWYU pragma: keep
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifndef unreachable
#if defined(__GNUC__)
#define unreachable() (__builtin_unreachable())
#elif defined(_MSC_VER)
#define unreachable() (__assume(false))
#else
[[noreturn]] inline void unreachable_impl() { }
#define unreachable() (unreachable_impl())
#endif
#endif

#include <json.h>
#include "protocol_json.h"
#include "ws_client.h"

#define ENVIRONMENT_VARIABLE_NAME "NEURO_SDK_WS_URL"
#define MESSAGE_QUEUE_SIZE 10

static char const *neurosdk_getenv(char const *name) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
	char const *value = getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
	return value;
}

static char *duplicate_string(char const *value) {
	size_t size = strlen(value) + 1;
	char *copy = malloc(size);
	if (copy)
		memcpy(copy, value, size);
	return copy;
}

static char *duplicate_string_n(char const *value, size_t length) {
	if (length == SIZE_MAX)
		return NULL;
	char *copy = malloc(length + 1);
	if (copy) {
		memcpy(copy, value, length);
		copy[length] = '\0';
	}
	return copy;
}

static bool json_string_equals(json_string_t const *value,
                               char const *expected,
                               size_t expected_length) {
	return value && value->string_size == expected_length &&
	       memcmp(value->string, expected, expected_length) == 0;
}

#define JSON_STRING_EQUALS(value, literal) \
	json_string_equals((value), (literal), sizeof(literal) - 1)

static bool json_string_contains_nul(json_string_t const *value) {
	return memchr(value->string, '\0', value->string_size) != NULL;
}

#ifndef LIB_VERSION
#error "LIB_VERSION is not defined!"
#endif
#ifndef LIB_BUILD_HASH
#error "LIB_BUILD_HASH is not defined!"
#endif
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define LOG_DEBUG(context, ...)                                        \
	if (context->debug_prints && aprintf(&context->logm, __VA_ARGS__)) { \
		context->callback_log(NeuroSDK_Severity_Debug, context->logm,      \
		                      context->user_data);                         \
		free(context->logm);                                               \
	}

#define LOG_INFO(context, ...)                                              \
	if (context->validation_layers && aprintf(&context->logm, __VA_ARGS__)) { \
		context->callback_log(NeuroSDK_Severity_Info, context->logm,            \
		                      context->user_data);                              \
		free(context->logm);                                                    \
	}

#define LOG_WARN(context, ...)                                              \
	if (context->validation_layers && aprintf(&context->logm, __VA_ARGS__)) { \
		context->callback_log(NeuroSDK_Severity_Warn, context->logm,            \
		                      context->user_data);                              \
		free(context->logm);                                                    \
	}

#define LOG_ERROR(context, ...)                                             \
	if (context->validation_layers && aprintf(&context->logm, __VA_ARGS__)) { \
		context->callback_log(NeuroSDK_Severity_Error, context->logm,           \
		                      context->user_data);                              \
		free(context->logm);                                                    \
	}

static void default_logger(neurosdk_severity_e severity,
                           char *message,
                           void *user_data) {
	(void)user_data;
#ifdef _WIN32
	HANDLE h_console = GetStdHandle(STD_OUTPUT_HANDLE);

	CONSOLE_SCREEN_BUFFER_INFO console_info;
	GetConsoleScreenBufferInfo(h_console, &console_info);
	WORD original_color = console_info.wAttributes;
#define RED \
	SetConsoleTextAttribute(h_console, FOREGROUND_RED | FOREGROUND_INTENSITY)
#define YELLOW             \
	SetConsoleTextAttribute( \
	    h_console, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define BLUE \
	SetConsoleTextAttribute(h_console, FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define GRAY SetConsoleTextAttribute(h_console, FOREGROUND_INTENSITY)
#define RESET SetConsoleTextAttribute(h_console, original_color)
#else
#define RED printf("\033[31;1m")
#define YELLOW printf("\033[33;1m")
#define BLUE printf("\033[34;1m")
#define GRAY printf("\033[90m")
#define RESET printf("\033[0m")
#endif
	if (severity == NeuroSDK_Severity_Debug) {
		GRAY;
		printf("NeuroSDK Validation Layer: DEBUG: ");
	} else if (severity == NeuroSDK_Severity_Info) {
		BLUE;
		printf("NeuroSDK Validation Layer: INFO: ");
	} else if (severity == NeuroSDK_Severity_Warn) {
		YELLOW;
		printf("NeuroSDK Validation Layer: WARN: ");
	} else if (severity == NeuroSDK_Severity_Error) {
		RED;
		printf("NeuroSDK Validation Layer: ERROR: ");
	} else {
		unreachable();
	}

	printf("%s\n", message);

	RESET;
}

typedef struct context {
	char const *game_name;
	int poll_ms;

	void *user_data;

	neurosdk_callback_log_t callback_log;
	char *logm;

	neurosdk_error_e conn_err;
	bool connected;

	neurosdk_message_t *message_queue;
	int message_queue_size;
	int message_queue_cap;

	ws_t *ws;

	mtx_t out_mtx;
	char **pending_messages;
	int pending_messages_size;
	int pending_messages_cap;

	bool debug_prints : 1;
	bool validation_layers : 1;
} context_t;

#if defined(_MSC_VER)
static int vasprintf(char **strp, const char *fmt, va_list ap) {
	va_list ap_copy;
	int formattedLength, actualLength;
	size_t requiredSize;
	*strp = NULL;
	va_copy(ap_copy, ap);
	formattedLength = _vscprintf(fmt, ap_copy);
	va_end(ap_copy);
	if (formattedLength < 0) {
		return -1;
	}
	requiredSize = ((size_t)formattedLength) + 1;
	*strp = (char *)malloc(requiredSize);
	if (*strp == NULL) {
		errno = ENOMEM;
		return -1;
	}
	actualLength = vsnprintf_s(*strp, requiredSize, requiredSize - 1, fmt, ap);
	if (actualLength != formattedLength) {
		free(*strp);
		*strp = NULL;
		errno = 1;
		return -1;
	}
	return formattedLength;
}
#endif

static int aprintf(char **strp, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int bytes = vasprintf(strp, fmt, args);
	va_end(args);
	return bytes;
}

NEUROSDK_EXPORT char const *neurosdk_version(void) {
	return STR(LIB_VERSION);
}
NEUROSDK_EXPORT char const *neurosdk_git_hash(void) {
	return STR(LIB_BUILD_HASH);
}

NEUROSDK_EXPORT char const *neurosdk_error_string(neurosdk_error_e err) {
	switch (err) {
		case NeuroSDK_None:
			return "None.";
		case NeuroSDK_Internal:
			return "An internal error occurred.";
		case NeuroSDK_Uninitialized:
			return "Component is not initialized.";
		case NeuroSDK_NoGameName:
			return "Game name is missing.";
		case NeuroSDK_OutOfMemory:
			return "Memory allocation failed.";
		case NeuroSDK_NoURL:
			return "No URL provided.";
		case NeuroSDK_ConnectionError:
			return "Failed to establish a connection.";
		case NeuroSDK_MessageQueueFull:
			return "Message queue is full.";
		case NeuroSDK_ReceivedBinary:
			return "Unexpected binary data received.";
		case NeuroSDK_InvalidJSON:
			return "Received malformed JSON.";
		case NeuroSDK_UnknownCommand:
			return "Unknown command received.";
		case NeuroSDK_InvalidMessage:
			return "Message format is invalid.";
		case NeuroSDK_CommandNotAvailable:
			return "The requested command is not available in this context.";
		case NeuroSDK_SendFailed:
			return "Failed to send message.";
		default:
			return "Unknown error code.";
	}
}

static void message_cleanup(neurosdk_message_t *msg) {
	if (!msg)
		return;
	if (msg->kind == NeuroSDK_MessageKind_Action) {
		free(msg->value.action.id);
		free(msg->value.action.name);
		free(msg->value.action.data);
	} else if (msg->kind == NeuroSDK_MessageKind_StartupAcknowledgement) {
		free(msg->value.startup_acknowledgement.session_id);
		free(msg->value.startup_acknowledgement.character_id);
		free(msg->value.startup_acknowledgement.display_name);
	}
	memset(msg, 0, sizeof(*msg));
}

static neurosdk_error_e parse_s2c_json(context_t *ctx,
                                       neurosdk_message_t *msg,
                                       char const *json,
                                       size_t len) {
	if (!ctx) {
		return NeuroSDK_Uninitialized;
	}
	if (!json || len == 0) {
		LOG_ERROR(ctx, "[parse_s2c_json] Provided JSON is empty or null.");
		return NeuroSDK_InvalidJSON;
	}
	if (!protocol_json_validate_text(json, len)) {
		LOG_ERROR(ctx,
		          "[parse_s2c_json] Provided JSON is not valid UTF-8 JSON text.");
		return NeuroSDK_InvalidJSON;
	}

	neurosdk_error_e res = NeuroSDK_None;
	neurosdk_message_t parsed = {0};
	json_parse_result_t parse_result = {0};
	json_value_t *root = json_parse_ex(json, len, json_parse_flags_default, NULL,
	                                   NULL, &parse_result);
	if (!root) {
		if (parse_result.error == json_parse_error_allocator_failed)
			return NeuroSDK_OutOfMemory;
		LOG_ERROR(ctx, "[parse_s2c_json] Could not parse message: invalid JSON.");
		return NeuroSDK_InvalidJSON;
	}
	if (root->type != json_type_object) {
		LOG_ERROR(ctx, "[parse_s2c_json] Parsed JSON root is not an object.");
		res = NeuroSDK_InvalidMessage;
		goto cleanup;
	}

	json_object_t *root_obj = (json_object_t *)root->payload;
	json_object_element_t *root_elem = root_obj->start;

	neurosdk_message_kind_e kind = 0xFFFF;
	bool command_found = false;
	size_t root_data_count = 0;

	while (root_elem) {
		if (JSON_STRING_EQUALS(root_elem->name, "command")) {
			if (command_found) {
				LOG_ERROR(ctx, "[parse_s2c_json] Duplicate 'command' field.");
				res = NeuroSDK_InvalidMessage;
				goto cleanup;
			}
			command_found = true;
			if (root_elem->value->type != json_type_string) {
				LOG_ERROR(ctx, "[parse_s2c_json] 'command' field is not a string.");
				res = NeuroSDK_InvalidMessage;
				goto cleanup;
			}
			json_string_t *value_str = (json_string_t *)root_elem->value->payload;
			if (json_string_contains_nul(value_str)) {
				LOG_ERROR(ctx, "[parse_s2c_json] 'command' contains a null character.");
				res = NeuroSDK_InvalidMessage;
				goto cleanup;
			}

			if (JSON_STRING_EQUALS(value_str, "action")) {
				kind = NeuroSDK_MessageKind_Action;
			} else if (JSON_STRING_EQUALS(value_str, "startup")) {
				kind = NeuroSDK_MessageKind_StartupAcknowledgement;
			} else if (JSON_STRING_EQUALS(value_str, "actions/reregister_all")) {
				kind = NeuroSDK_MessageKind_ActionsReregisterAll;
			} else {
				LOG_ERROR(ctx, "[parse_s2c_json] Unknown command '%s'.",
				          value_str->string);
				res = NeuroSDK_UnknownCommand;
				goto cleanup;
			}
		} else if (JSON_STRING_EQUALS(root_elem->name, "data")) {
			root_data_count++;
		}
		root_elem = root_elem->next;
	}

	if (kind == 0xFFFF) {
		LOG_ERROR(ctx, "[parse_s2c_json] Missing or invalid 'command' field.");
		res = NeuroSDK_InvalidMessage;
		goto cleanup;
	}

	if (kind == NeuroSDK_MessageKind_ActionsReregisterAll) {
		if (root_data_count != 0) {
			LOG_ERROR(ctx,
			          "[parse_s2c_json] 'actions/reregister_all' must not contain "
			          "a root 'data' field.");
			res = NeuroSDK_InvalidMessage;
			goto cleanup;
		}
		parsed.kind = kind;
		*msg = parsed;
	} else if (kind == NeuroSDK_MessageKind_StartupAcknowledgement) {
		if (root_data_count != 1) {
			LOG_ERROR(
			    ctx,
			    "[parse_s2c_json] Startup acknowledgement requires exactly one root "
			    "'data' field.");
			res = NeuroSDK_InvalidMessage;
			goto cleanup;
		}

		root_elem = root_obj->start;
		while (root_elem) {
			if (JSON_STRING_EQUALS(root_elem->name, "data")) {
				if (root_elem->value->type != json_type_object) {
					LOG_ERROR(ctx, "[parse_s2c_json] Startup 'data' must be an object.");
					res = NeuroSDK_InvalidMessage;
					goto cleanup;
				}
				json_object_t *data_obj = (json_object_t *)root_elem->value->payload;
				json_object_element_t *data_elem = data_obj->start;
				json_object_t *session_obj = NULL;
				bool session_found = false;
				while (data_elem) {
					if (JSON_STRING_EQUALS(data_elem->name, "session")) {
						if (session_found || data_elem->value->type != json_type_object) {
							LOG_ERROR(ctx,
							          "[parse_s2c_json] Startup requires exactly one object "
							          "'session' field.");
							res = NeuroSDK_InvalidMessage;
							goto cleanup;
						}
						session_found = true;
						session_obj = (json_object_t *)data_elem->value->payload;
					}
					data_elem = data_elem->next;
				}
				if (!session_found) {
					LOG_ERROR(ctx, "[parse_s2c_json] Startup is missing 'session'.");
					res = NeuroSDK_InvalidMessage;
					goto cleanup;
				}

				char *session_id = NULL, *character_id = NULL, *display_name = NULL;
				bool session_id_found = false, character_id_found = false,
				     display_name_found = false;
				json_object_element_t *session_elem = session_obj->start;
				while (session_elem) {
					json_string_t *value = NULL;
					char **destination = NULL;
					bool *found = NULL;
					if (JSON_STRING_EQUALS(session_elem->name, "sessionId")) {
						found = &session_id_found;
						destination = &session_id;
					} else if (JSON_STRING_EQUALS(session_elem->name, "characterId")) {
						found = &character_id_found;
						destination = &character_id;
					} else if (JSON_STRING_EQUALS(session_elem->name, "displayName")) {
						found = &display_name_found;
						destination = &display_name;
					}
					if (found) {
						if (*found || session_elem->value->type != json_type_string) {
							LOG_ERROR(ctx,
							          "[parse_s2c_json] Startup session fields must be "
							          "unique strings.");
							res = NeuroSDK_InvalidMessage;
							goto startup_cleanup;
						}
						*found = true;
						value = (json_string_t *)session_elem->value->payload;
						if (json_string_contains_nul(value)) {
							LOG_ERROR(ctx,
							          "[parse_s2c_json] Startup session strings must not "
							          "contain null characters.");
							res = NeuroSDK_InvalidMessage;
							goto startup_cleanup;
						}
						*destination =
						    duplicate_string_n(value->string, value->string_size);
						if (!*destination) {
							res = NeuroSDK_OutOfMemory;
							goto startup_cleanup;
						}
					}
					session_elem = session_elem->next;
				}
				if (!session_id_found || !character_id_found || !display_name_found) {
					LOG_ERROR(ctx,
					          "[parse_s2c_json] Startup session is missing required "
					          "fields.");
					res = NeuroSDK_InvalidMessage;
					goto startup_cleanup;
				}
				parsed.kind = kind;
				parsed.value.startup_acknowledgement =
				    (neurosdk_message_startup_acknowledgement_t){
				        .session_id = session_id,
				        .character_id = character_id,
				        .display_name = display_name,
				    };
				*msg = parsed;
				goto cleanup;
			startup_cleanup:
				free(session_id);
				free(character_id);
				free(display_name);
				goto cleanup;
			}
			root_elem = root_elem->next;
		}
	} else if (kind == NeuroSDK_MessageKind_Action) {
		if (root_data_count != 1) {
			LOG_ERROR(
			    ctx,
			    "[parse_s2c_json] Action requires exactly one root 'data' field.");
			res = NeuroSDK_InvalidMessage;
			goto cleanup;
		}
		root_elem = root_obj->start;
		while (root_elem) {
			if (JSON_STRING_EQUALS(root_elem->name, "data")) {
				if (root_elem->value->type != json_type_object) {
					LOG_ERROR(ctx, "[parse_s2c_json] 'data' field is not an object.");
					res = NeuroSDK_InvalidMessage;
					goto cleanup;
				}
				json_object_t *data_obj = (json_object_t *)root_elem->value->payload;
				json_object_element_t *obj_root = data_obj->start;

				char *id = NULL, *name = NULL, *data = NULL;
				bool id_found = false, name_found = false, action_data_found = false;

				while (obj_root) {
					if (JSON_STRING_EQUALS(obj_root->name, "id")) {
						if (id_found) {
							LOG_ERROR(ctx, "[parse_s2c_json] Duplicate action 'id' field.");
							res = NeuroSDK_InvalidMessage;
							goto parse_cleanup;
						}
						id_found = true;
						if (obj_root->value->type != json_type_string) {
							LOG_ERROR(ctx, "[parse_s2c_json] 'id' field must be a string.");
							res = NeuroSDK_InvalidMessage;
							goto parse_cleanup;
						}
						json_string_t *str = (json_string_t *)obj_root->value->payload;
						if (json_string_contains_nul(str)) {
							LOG_ERROR(ctx,
							          "[parse_s2c_json] 'id' contains a null character.");
							res = NeuroSDK_InvalidMessage;
							goto parse_cleanup;
						}
						id = duplicate_string_n(str->string, str->string_size);
						if (!id) {
							res = NeuroSDK_OutOfMemory;
							goto parse_cleanup;
						}
					} else if (JSON_STRING_EQUALS(obj_root->name, "name")) {
						if (name_found) {
							LOG_ERROR(ctx, "[parse_s2c_json] Duplicate action 'name' field.");
							res = NeuroSDK_InvalidMessage;
							goto parse_cleanup;
						}
						name_found = true;
						if (obj_root->value->type != json_type_string) {
							LOG_ERROR(ctx, "[parse_s2c_json] 'name' field must be a string.");
							res = NeuroSDK_InvalidMessage;
							goto parse_cleanup;
						}
						json_string_t *str = (json_string_t *)obj_root->value->payload;
						if (json_string_contains_nul(str)) {
							LOG_ERROR(ctx,
							          "[parse_s2c_json] 'name' contains a null character.");
							res = NeuroSDK_InvalidMessage;
							goto parse_cleanup;
						}
						name = duplicate_string_n(str->string, str->string_size);
						if (!name) {
							res = NeuroSDK_OutOfMemory;
							goto parse_cleanup;
						}
					} else if (JSON_STRING_EQUALS(obj_root->name, "data")) {
						if (action_data_found) {
							LOG_ERROR(ctx, "[parse_s2c_json] Duplicate action 'data' field.");
							res = NeuroSDK_InvalidMessage;
							goto parse_cleanup;
						}
						action_data_found = true;
						if (obj_root->value->type == json_type_null) {
							data = NULL;
						} else if (obj_root->value->type == json_type_string) {
							json_string_t *str = (json_string_t *)obj_root->value->payload;
							if (json_string_contains_nul(str)) {
								LOG_ERROR(ctx,
								          "[parse_s2c_json] action 'data' contains a null "
								          "character.");
								res = NeuroSDK_InvalidMessage;
								goto parse_cleanup;
							}
							data = duplicate_string_n(str->string, str->string_size);
							if (!data) {
								res = NeuroSDK_OutOfMemory;
								goto parse_cleanup;
							}
						} else {
							LOG_ERROR(
							    ctx,
							    "[parse_s2c_json] 'data' field must be a string or null.");
							res = NeuroSDK_InvalidMessage;
							goto parse_cleanup;
						}
					}
					obj_root = obj_root->next;
				}

				if (!id || !name) {
					LOG_ERROR(ctx,
					          "[parse_s2c_json] 'data' object for 'action' must contain "
					          "'id' and 'name'.");
					res = NeuroSDK_InvalidMessage;
					goto parse_cleanup;
				}

				parsed.kind = NeuroSDK_MessageKind_Action;
				parsed.value.action = (neurosdk_message_action_t){
				    .id = id,
				    .name = name,
				    .data = data,
				};
				*msg = parsed;
				goto cleanup;
			parse_cleanup:
				if (id)
					free(id);
				if (name)
					free(name);
				if (data)
					free(data);
				goto cleanup;
			}
			root_elem = root_elem->next;
		}
	} else {
		LOG_ERROR(ctx, "[parse_s2c_json] Received an unhandled S2C command.");
		unreachable();
	}

cleanup:
	free(root);
	return res;
}

static void ws_on_open(ws_t *ws, void *userdata) {
	(void)ws;
	context_t *ctx = (context_t *)userdata;
	LOG_INFO(ctx, "WebSocket connection opened successfully.");
	ctx->connected = true;
}

static void ws_on_message(ws_t *ws,
                          char const *data,
                          size_t len,
                          int binary,
                          void *userdata) {
	(void)ws;
	context_t *ctx = (context_t *)userdata;

	if (binary) {
		LOG_ERROR(ctx, "Received binary (non-plaintext) data from server!");
		ctx->conn_err = NeuroSDK_ReceivedBinary;
		return;
	}
	if (len > PROTOCOL_MESSAGE_MAX_SIZE) {
		LOG_ERROR(ctx, "Received protocol JSON exceeds the message size limit.");
		ctx->conn_err = NeuroSDK_InvalidJSON;
		return;
	}
	neurosdk_message_t msg = {0};
	LOG_DEBUG(ctx, "Received message: %.*s", (int)len, data);
	ctx->conn_err = parse_s2c_json(ctx, &msg, data, len);
	if (!ctx->conn_err) {
		if (ctx->message_queue_size == ctx->message_queue_cap) {
			LOG_ERROR(ctx, "Message queue is full! (NeuroSDK_MessageQueueFull).");
			ctx->conn_err = NeuroSDK_MessageQueueFull;
			message_cleanup(&msg);
			return;
		}
		ctx->message_queue[ctx->message_queue_size++] = msg;
	}
}

static void ws_on_close(ws_t *ws,
                        uint16_t code,
                        char const *reason,
                        size_t reason_len,
                        void *userdata) {
	(void)ws;
	context_t *ctx = (context_t *)userdata;
	LOG_WARN(ctx,
	         "Connection closed (code=%u, reason=%.*s). Marking as "
	         "disconnected.",
	         code, (int)reason_len, reason);
	ctx->connected = false;
}

static void ws_on_error(ws_t *ws, char const *msg, void *userdata) {
	(void)ws;
	context_t *ctx = (context_t *)userdata;
	LOG_WARN(ctx, "WebSocket error: %s", msg);
	ctx->connected = false;
	ctx->conn_err = NeuroSDK_ConnectionError;
}

NEUROSDK_EXPORT neurosdk_error_e
neurosdk_context_create(neurosdk_context_t *ctx,
                        neurosdk_context_create_desc_t *desc) {
	neurosdk_error_e res = NeuroSDK_None;
	context_t *context = malloc(sizeof(context_t));
	if (!context) {
		return NeuroSDK_OutOfMemory;
	}
	memset(context, 0, sizeof(*context));

	if (!desc->game_name || !strlen(desc->game_name)) {
		free(context);
		return NeuroSDK_NoGameName;
	}
	context->game_name = duplicate_string(desc->game_name);
	if (!context->game_name) {
		free(context);
		return NeuroSDK_OutOfMemory;
	}
	context->poll_ms = desc->poll_ms;

	context->user_data = desc->user_data;
	context->callback_log = desc->callback_log;
	if (!context->callback_log) {
		context->callback_log = default_logger;
	}

	context->debug_prints = desc->flags & NeuroSDK_ContextCreateFlags_DebugPrints;
	context->validation_layers =
	    desc->flags & NeuroSDK_ContextCreateFlags_ValidationLayers;

	context->pending_messages_cap = MESSAGE_QUEUE_SIZE;
	context->pending_messages_size = 0;
	context->pending_messages =
	    malloc(context->pending_messages_cap * sizeof(char *));
	if (!context->pending_messages) {
		free((void *)context->game_name);
		free(context);
		return NeuroSDK_OutOfMemory;
	}

	context->message_queue_cap = MESSAGE_QUEUE_SIZE;
	context->message_queue_size = 0;
	context->message_queue =
	    malloc(context->message_queue_cap * sizeof(neurosdk_message_t));
	if (!context->message_queue) {
		free(context->pending_messages);
		free((void *)context->game_name);
		free(context);
		return NeuroSDK_OutOfMemory;
	}

	char const *fetched_url = desc->url;
	if (!fetched_url) {
		fetched_url = neurosdk_getenv(ENVIRONMENT_VARIABLE_NAME);
	}
	if (!fetched_url) {
		res = NeuroSDK_NoURL;
		goto cleanup;
	}

	ws_callbacks_t cbs = {
	    .on_open = ws_on_open,
	    .on_message = ws_on_message,
	    .on_close = ws_on_close,
	    .on_error = ws_on_error,
	    .userdata = context,
	};

	if (mtx_init(&context->out_mtx, mtx_plain) != thrd_success) {
		res = NeuroSDK_Internal;
		goto cleanup;
	}

	if (ws_connect(&context->ws, fetched_url, cbs) != 0) {
		res = NeuroSDK_ConnectionError;
		goto cleanup_mtx;
	}

	for (int i = 0; i < 10 && !context->connected; i++) {
		ws_poll(context->ws, 300);
	}
	if (!context->connected) {
		res = NeuroSDK_ConnectionError;
		goto cleanup_ws;
	}

	(*ctx) = (neurosdk_context_t)context;
	return res;

cleanup_ws:
	ws_destroy(context->ws);
cleanup_mtx:
	mtx_destroy(&context->out_mtx);
cleanup:
	free(context->pending_messages);
	free(context->message_queue);
	free((void *)context->game_name);
	free(context);
	return res;
}

NEUROSDK_EXPORT neurosdk_error_e
neurosdk_context_destroy(neurosdk_context_t *ctx) {
	if (!ctx || !(*ctx)) {
		return NeuroSDK_Uninitialized;
	}
	context_t *context = (context_t *)(*ctx);

	LOG_DEBUG(context, "Destroying NeuroSDK context.");

	ws_destroy(context->ws);

	mtx_lock(&context->out_mtx);
	for (int i = 0; i < context->pending_messages_size; i++) {
		free(context->pending_messages[i]);
	}
	context->pending_messages_size = 0;
	mtx_unlock(&context->out_mtx);
	mtx_destroy(&context->out_mtx);

	free(context->pending_messages);
	for (int i = 0; i < context->message_queue_size; i++) {
		message_cleanup(&context->message_queue[i]);
	}
	free(context->message_queue);
	free((void *)context->game_name);
	free(context);

	*ctx = NULL;
	return NeuroSDK_None;
}

NEUROSDK_EXPORT neurosdk_error_e
neurosdk_context_poll(neurosdk_context_t *ctx,
                      OUT neurosdk_message_t **messages,
                      OUT int *count) {
	if (!ctx || !(*ctx)) {
		return NeuroSDK_Uninitialized;
	}
	context_t *context = (context_t *)(*ctx);

	if (!context->ws) {
		LOG_ERROR(context,
		          "neurosdk_context_poll called but 'ws' is NULL. Context may "
		          "be uninitialized.");
		return NeuroSDK_Uninitialized;
	}

	LOG_DEBUG(context, "Polling context for new messages.");

	context->conn_err = NeuroSDK_None;
	ws_poll(context->ws, context->poll_ms);

	/* Flush any queued outgoing messages */
	mtx_lock(&context->out_mtx);
	for (int i = 0; i < context->pending_messages_size; i++) {
		char *msg = context->pending_messages[i];
		LOG_DEBUG(context, "Sending message: %s", msg);
		ws_send(context->ws, msg, strlen(msg));
		free(msg);
	}
	context->pending_messages_size = 0;
	mtx_unlock(&context->out_mtx);

	if (context->conn_err) {
		LOG_ERROR(context, "Connection error during poll: %s",
		          neurosdk_error_string(context->conn_err));
		return context->conn_err;
	}

	*messages = context->message_queue;
	*count = context->message_queue_size;
	context->message_queue_size = 0;

	return NeuroSDK_None;
}

NEUROSDK_EXPORT neurosdk_error_e
neurosdk_context_send(neurosdk_context_t *ctx, neurosdk_message_t *msg) {
	if (!ctx || !(*ctx)) {
		return NeuroSDK_Uninitialized;
	}
	context_t *context = (context_t *)(*ctx);
	if (!context->ws) {
		LOG_ERROR(context, "neurosdk_context_send: invalid context (ws is NULL).");
		return NeuroSDK_Uninitialized;
	}
	if (!neurosdk_context_connected(ctx)) {
		LOG_ERROR(context,
		          "neurosdk_context_send: cannot send message because we are "
		          "not connected.");
		return NeuroSDK_ConnectionError;
	}

	char *str = NULL;
	neurosdk_error_e build_error =
	    protocol_json_build_c2s(context->game_name, msg, &str);
	if (build_error != NeuroSDK_None)
		return build_error;
	size_t bytes = strlen(str);

	LOG_DEBUG(context, "Queueing message for send: %s (%zu bytes)", str, bytes);

	mtx_lock(&context->out_mtx);
	if (context->pending_messages_size < context->pending_messages_cap) {
		context->pending_messages[context->pending_messages_size++] = str;
	} else {
		mtx_unlock(&context->out_mtx);
		LOG_ERROR(context, "Out of memory: pending messages buffer is full.");
		free(str);
		return NeuroSDK_OutOfMemory;
	}
	mtx_unlock(&context->out_mtx);

	return NeuroSDK_None;
}

NEUROSDK_EXPORT bool neurosdk_context_connected(neurosdk_context_t *ctx) {
	if (!ctx || !(*ctx)) {
		return false;
	}
	return ((context_t *)*ctx)->connected;
}

NEUROSDK_EXPORT neurosdk_error_e
neurosdk_message_destroy(neurosdk_message_t *msg) {
	if (!msg) {
		return NeuroSDK_Uninitialized;
	}
	if (msg->kind == NeuroSDK_MessageKind_Action ||
	    msg->kind == NeuroSDK_MessageKind_StartupAcknowledgement ||
	    msg->kind == NeuroSDK_MessageKind_ActionsReregisterAll) {
		message_cleanup(msg);
	} else {
		return NeuroSDK_UnknownCommand;
	}
	return NeuroSDK_None;
}
