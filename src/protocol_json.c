#include "protocol_json.h"

#include <stdlib.h>
#include <string.h>

#define JSON_MAX_RECURSION 64
#include <json.h>

typedef struct json_builder {
	char *data;
	size_t size;
	size_t capacity;
	neurosdk_error_e error;
} json_builder_t;

static bool json_utf8_valid(unsigned char const *data, size_t size) {
	size_t i = 0;
	while (i < size) {
		unsigned char first = data[i++];
		if (first <= 0x7F)
			continue;

		size_t continuation_count;
		unsigned char second_min = 0x80;
		unsigned char second_max = 0xBF;
		if (first >= 0xC2 && first <= 0xDF) {
			continuation_count = 1;
		} else if (first >= 0xE0 && first <= 0xEF) {
			continuation_count = 2;
			if (first == 0xE0)
				second_min = 0xA0;
			else if (first == 0xED)
				second_max = 0x9F;
		} else if (first >= 0xF0 && first <= 0xF4) {
			continuation_count = 3;
			if (first == 0xF0)
				second_min = 0x90;
			else if (first == 0xF4)
				second_max = 0x8F;
		} else {
			return false;
		}

		if (continuation_count > size - i || data[i] < second_min ||
		    data[i] > second_max)
			return false;
		for (size_t j = 1; j < continuation_count; j++) {
			if (data[i + j] < 0x80 || data[i + j] > 0xBF)
				return false;
		}
		i += continuation_count;
	}
	return true;
}

bool protocol_json_validate_text(char const *json, size_t size) {
	if (!json || !json_utf8_valid((unsigned char const *)json, size))
		return false;
	bool in_string = false;
	bool escaped = false;
	for (size_t i = 0; i < size; i++) {
		unsigned char byte = (unsigned char)json[i];
		if (!in_string) {
			if (byte == '"')
				in_string = true;
			continue;
		}
		if (escaped) {
			escaped = false;
			continue;
		}
		if (byte == '\\')
			escaped = true;
		else if (byte == '"')
			in_string = false;
		else if (byte < 0x20)
			return false;
	}
	return true;
}

static bool json_builder_reserve(json_builder_t *builder, size_t additional) {
	if (additional > PROTOCOL_MESSAGE_MAX_SIZE - builder->size) {
		builder->error = NeuroSDK_InvalidMessage;
		return false;
	}
	size_t required = builder->size + additional;
	if (required < builder->capacity)
		return true;

	size_t capacity = builder->capacity ? builder->capacity : 256;
	while (capacity <= required) {
		if (capacity > PROTOCOL_MESSAGE_MAX_SIZE / 2) {
			capacity = PROTOCOL_MESSAGE_MAX_SIZE + 1;
			break;
		}
		capacity *= 2;
	}
	char *data = realloc(builder->data, capacity);
	if (!data) {
		builder->error = NeuroSDK_OutOfMemory;
		return false;
	}
	builder->data = data;
	builder->capacity = capacity;
	return true;
}

static bool json_builder_append_n(json_builder_t *builder,
	                              char const *value,
	                              size_t length) {
	if (!json_builder_reserve(builder, length))
		return false;
	memcpy(builder->data + builder->size, value, length);
	builder->size += length;
	builder->data[builder->size] = '\0';
	return true;
}

#define JSON_APPEND_LITERAL(builder, literal) \
	json_builder_append_n((builder), (literal), sizeof(literal) - 1)

static bool json_builder_append_string(json_builder_t *builder,
	                                   char const *value) {
	static char const hex[] = "0123456789ABCDEF";
	if (!value) {
		builder->error = NeuroSDK_InvalidMessage;
		return false;
	}
	size_t length = strlen(value);
	if (!json_utf8_valid((unsigned char const *)value, length)) {
		builder->error = NeuroSDK_InvalidMessage;
		return false;
	}
	if (!JSON_APPEND_LITERAL(builder, "\""))
		return false;

	for (size_t i = 0; i < length; i++) {
		unsigned char byte = (unsigned char)value[i];
		char escape[6] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0x0F]};
		char const *short_escape = NULL;
		switch (byte) {
			case '"': short_escape = "\\\""; break;
			case '\\': short_escape = "\\\\"; break;
			case '\b': short_escape = "\\b"; break;
			case '\f': short_escape = "\\f"; break;
			case '\n': short_escape = "\\n"; break;
			case '\r': short_escape = "\\r"; break;
			case '\t': short_escape = "\\t"; break;
			default: break;
		}
		if (short_escape) {
			if (!json_builder_append_n(builder, short_escape, 2))
				return false;
		} else if (byte < 0x20) {
			if (!json_builder_append_n(builder, escape, sizeof(escape)))
				return false;
		} else if (!json_builder_append_n(builder, value + i, 1)) {
			return false;
		}
	}
	return JSON_APPEND_LITERAL(builder, "\"");
}

static bool json_builder_append_string_array(json_builder_t *builder,
	                                         char **values,
	                                         int count) {
	if (count < 0 || (count > 0 && !values) ||
	    !JSON_APPEND_LITERAL(builder, "[")) {
		if (!builder->error)
			builder->error = NeuroSDK_InvalidMessage;
		return false;
	}
	for (int i = 0; i < count; i++) {
		if ((i && !JSON_APPEND_LITERAL(builder, ",")) ||
		    !json_builder_append_string(builder, values[i]))
			return false;
	}
	return JSON_APPEND_LITERAL(builder, "]");
}

static neurosdk_error_e json_schema_validate(char const *schema) {
	size_t length = strlen(schema);
	if (!json_utf8_valid((unsigned char const *)schema, length))
		return NeuroSDK_InvalidMessage;
	json_parse_result_t result = {0};
	json_value_t *value = json_parse_ex(schema, length, json_parse_flags_default,
	                                  NULL, NULL, &result);
	if (!value)
		return result.error == json_parse_error_allocator_failed
		           ? NeuroSDK_OutOfMemory
		           : NeuroSDK_InvalidMessage;
	bool is_object = value->type == json_type_object;
	free(value);
	return is_object ? NeuroSDK_None : NeuroSDK_InvalidMessage;
}

neurosdk_error_e protocol_json_build_c2s(char const *game_name,
	                                     neurosdk_message_t const *message,
	                                     char **result) {
	json_builder_t builder = {0};
	*result = NULL;

#define APPEND_LITERAL(literal)                       \
	do {                                                 \
		if (!JSON_APPEND_LITERAL(&builder, (literal)))      \
			goto failure;                                      \
	} while (0)
#define APPEND_STRING(value)                         \
	do {                                                 \
		if (!json_builder_append_string(&builder, (value))) \
			goto failure;                                      \
	} while (0)

	APPEND_LITERAL("{\"command\":");
	switch (message->kind) {
		case NeuroSDK_MessageKind_Startup:
			APPEND_LITERAL("\"startup\",\"game\":");
			APPEND_STRING(game_name);
			break;
		case NeuroSDK_MessageKind_Context:
			if (!message->value.context.message)
				goto invalid_message;
			APPEND_LITERAL("\"context\",\"game\":");
			APPEND_STRING(game_name);
			APPEND_LITERAL(",\"data\":{\"message\":");
			APPEND_STRING(message->value.context.message);
			APPEND_LITERAL(",\"silent\":");
			if (message->value.context.silent)
				APPEND_LITERAL("true");
			else
				APPEND_LITERAL("false");
			APPEND_LITERAL("}");
			break;
		case NeuroSDK_MessageKind_ActionsRegister: {
			int count = message->value.actions_register.actions_len;
			if (count < 0 || (count > 0 && !message->value.actions_register.actions))
				goto invalid_message;
			APPEND_LITERAL("\"actions/register\",\"game\":");
			APPEND_STRING(game_name);
			APPEND_LITERAL(",\"data\":{\"actions\":[");
			for (int i = 0; i < count; i++) {
				neurosdk_action_t const *action =
				    &message->value.actions_register.actions[i];
				char const *schema = action->json_schema ? action->json_schema : "{}";
				if (!action->name)
					goto invalid_message;
				builder.error = json_schema_validate(schema);
				if (builder.error != NeuroSDK_None)
					goto failure;
				if (i)
					APPEND_LITERAL(",");
				APPEND_LITERAL("{\"name\":");
				APPEND_STRING(action->name);
				APPEND_LITERAL(",\"description\":");
				APPEND_STRING(action->description ? action->description : "");
				APPEND_LITERAL(",\"schema\":");
				if (!json_builder_append_n(&builder, schema, strlen(schema)))
					goto failure;
				APPEND_LITERAL("}");
			}
			APPEND_LITERAL("]}");
		} break;
		case NeuroSDK_MessageKind_ActionsUnregister:
			APPEND_LITERAL("\"actions/unregister\",\"game\":");
			APPEND_STRING(game_name);
			APPEND_LITERAL(",\"data\":{\"action_names\":");
			if (!json_builder_append_string_array(
			        &builder, message->value.actions_unregister.action_names,
			        message->value.actions_unregister.action_names_len))
				goto failure;
			APPEND_LITERAL("}");
			break;
		case NeuroSDK_MessageKind_ActionsForce: {
			char **names = message->value.actions_force.action_names;
			int count = message->value.actions_force.action_names_len;
			if (!message->value.actions_force.query || !names || count <= 0)
				goto invalid_message;
			char const *priority = "low";
			if (message->value.actions_force.priority == NeuroSDK_Priority_Medium)
				priority = "medium";
			else if (message->value.actions_force.priority == NeuroSDK_Priority_High)
				priority = "high";
			else if (message->value.actions_force.priority == NeuroSDK_Priority_Critical)
				priority = "critical";
			APPEND_LITERAL("\"actions/force\",\"game\":");
			APPEND_STRING(game_name);
			APPEND_LITERAL(",\"data\":{\"state\":");
			if (message->value.actions_force.state)
				APPEND_STRING(message->value.actions_force.state);
			else
				APPEND_LITERAL("null");
			APPEND_LITERAL(",\"query\":");
			APPEND_STRING(message->value.actions_force.query);
			APPEND_LITERAL(",\"ephemeral_context\":");
			if (message->value.actions_force.ephemeral_context)
				APPEND_LITERAL("true");
			else
				APPEND_LITERAL("false");
			APPEND_LITERAL(",\"action_names\":");
			if (!json_builder_append_string_array(&builder, names, count))
				goto failure;
			APPEND_LITERAL(",\"priority\":");
			APPEND_STRING(priority);
			APPEND_LITERAL("}");
		} break;
		case NeuroSDK_MessageKind_ActionResult:
			if (!message->value.action_result.id)
				goto invalid_message;
			APPEND_LITERAL("\"action/result\",\"game\":");
			APPEND_STRING(game_name);
			APPEND_LITERAL(",\"data\":{\"id\":");
			APPEND_STRING(message->value.action_result.id);
			APPEND_LITERAL(",\"success\":");
			if (message->value.action_result.success)
				APPEND_LITERAL("true");
			else
				APPEND_LITERAL("false");
			APPEND_LITERAL(",\"message\":");
			if (message->value.action_result.message)
				APPEND_STRING(message->value.action_result.message);
			else
				APPEND_LITERAL("null");
			APPEND_LITERAL("}");
			break;
		case NeuroSDK_MessageKind_Action:
		case NeuroSDK_MessageKind_StartupAcknowledgement:
		case NeuroSDK_MessageKind_ActionsReregisterAll:
			free(builder.data);
			return NeuroSDK_CommandNotAvailable;
		default:
			free(builder.data);
			return NeuroSDK_UnknownCommand;
	}
	APPEND_LITERAL("}");
	*result = builder.data;
#undef APPEND_STRING
#undef APPEND_LITERAL
	return NeuroSDK_None;

invalid_message:
	builder.error = NeuroSDK_InvalidMessage;
failure:
	free(builder.data);
#undef APPEND_STRING
#undef APPEND_LITERAL
	return builder.error ? builder.error : NeuroSDK_OutOfMemory;
}
