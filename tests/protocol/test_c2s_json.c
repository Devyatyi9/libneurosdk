#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/neurosdk.c"

static int failures;

static void check(bool condition, int line, char const *expression) {
	if (!condition) {
		fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, line, expression);
		failures++;
	}
}

#define CHECK(condition) check((condition), __LINE__, #condition)

static json_value_t *object_get(json_object_t const *object, char const *name) {
	for (json_object_element_t *element = object->start; element;
	     element = element->next) {
		if (element->name->string_size == strlen(name) &&
		    memcmp(element->name->string, name, element->name->string_size) == 0)
			return element->value;
	}
	return NULL;
}

static void check_string_value(json_value_t *value, char const *expected) {
	CHECK(value != NULL);
	if (!value || value->type != json_type_string) {
		CHECK(value && value->type == json_type_string);
		return;
	}
	json_string_t *string = (json_string_t *)value->payload;
	CHECK(string->string_size == strlen(expected));
	CHECK(memcmp(string->string, expected, string->string_size) == 0);
}

static json_value_t *build_and_parse(context_t const *context,
                                     neurosdk_message_t const *message,
                                     char **json) {
	CHECK(build_c2s_json(context, message, json) == NeuroSDK_None);
	if (!*json)
		return NULL;
	json_value_t *root = json_parse(*json, strlen(*json));
	CHECK(root != NULL);
	CHECK(root && root->type == json_type_object);
	return root;
}

static void check_json(context_t const *context,
                       neurosdk_message_t const *message,
                       char const *expected) {
	char *json = NULL;
	CHECK(build_c2s_json(context, message, &json) == NeuroSDK_None);
	CHECK(json != NULL);
	if (json) {
		if (strcmp(json, expected) != 0)
			fprintf(stderr, "actual:   %s\nexpected: %s\n", json, expected);
		CHECK(strcmp(json, expected) == 0);
		json_value_t *value = json_parse(json, strlen(json));
		CHECK(value != NULL);
		free(value);
	}
	free(json);
}

static void test_all_message_kinds(void) {
	context_t context = {.game_name = "game\"\\\n"};
	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_Startup};
	check_json(&context, &message,
	           "{\"command\":\"startup\",\"game\":\"game\\\"\\\\\\n\"}");

	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_Context,
	    .value.context = {.message = "\b\f\r\t\1", .silent = true}};
	check_json(&context, &message,
	           "{\"command\":\"context\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"message\":\"\\b\\f\\r\\t\\u0001\","
	           "\"silent\":true}}");

	neurosdk_action_t actions[] = {
	    {.name = "jump\"", .description = NULL, .json_schema = NULL},
	    {.name = "look",
	     .description = "left\\right",
	     .json_schema = "{ \"type\" : \"object\" }"},
	};
	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionsRegister,
	    .value.actions_register = {.actions = actions, .actions_len = 2}};
	check_json(&context, &message,
	           "{\"command\":\"actions/register\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"actions\":[{\"name\":\"jump\\\"\","
	           "\"description\":\"\",\"schema\":{}},{\"name\":\"look\","
	           "\"description\":\"left\\\\right\",\"schema\":{ \"type\" : "
	           "\"object\" }}]}}");

	char *names[] = {"one\"", "two\\"};
	message =
	    (neurosdk_message_t){.kind = NeuroSDK_MessageKind_ActionsUnregister,
	                         .value.actions_unregister = {.action_names = names,
	                                                      .action_names_len = 2}};
	check_json(&context, &message,
	           "{\"command\":\"actions/unregister\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"action_names\":[\"one\\\"\",\"two\\\\\"]}}");

	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionsForce,
	    .value.actions_force = {.state = NULL,
	                            .query = "why\"?",
	                            .ephemeral_context = true,
	                            .action_names = names,
	                            .action_names_len = 2,
	                            .priority = NeuroSDK_Priority_Critical}};
	check_json(&context, &message,
	           "{\"command\":\"actions/force\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"state\":null,\"query\":\"why\\\"?\","
	           "\"ephemeral_context\":true,\"action_names\":[\"one\\\"\","
	           "\"two\\\\\"],\"priority\":\"critical\"}}");
	message.value.actions_force.state = "ready\nnow";
	message.value.actions_force.ephemeral_context = false;
	message.value.actions_force.priority = NeuroSDK_Priority_Medium;
	check_json(&context, &message,
	           "{\"command\":\"actions/force\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"state\":\"ready\\nnow\",\"query\":\"why\\\"?\","
	           "\"ephemeral_context\":false,\"action_names\":[\"one\\\"\","
	           "\"two\\\\\"],\"priority\":\"medium\"}}");

	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionResult,
	    .value.action_result = {.id = "id\"", .success = false, .message = NULL}};
	check_json(&context, &message,
	           "{\"command\":\"action/result\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"id\":\"id\\\"\",\"success\":false,"
	           "\"message\":null}}");
	message.value.action_result.message = "done\\\n";
	message.value.action_result.success = true;
	check_json(&context, &message,
	           "{\"command\":\"action/result\",\"game\":\"game\\\"\\\\\\n\","
	           "\"data\":{\"id\":\"id\\\"\",\"success\":true,"
	           "\"message\":\"done\\\\\\n\"}}");
}

static void test_utf8_and_validation(void) {
	static char const utf8[] = "\320\277\321\200\320\270\320\262\320\265\321\202";
	context_t context = {.game_name = utf8};
	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_Context,
	                              .value.context = {.message = (char *)utf8}};
	char expected[256];
	snprintf(expected, sizeof(expected),
	         "{\"command\":\"context\",\"game\":\"%s\",\"data\":{"
	         "\"message\":\"%s\",\"silent\":false}}",
	         utf8, utf8);
	check_json(&context, &message, expected);

	unsigned char invalid_utf8[] = {0xC0, 0xAF, '\0'};
	message.value.context.message = (char *)invalid_utf8;
	char *json = NULL;
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
	CHECK(json == NULL);

	neurosdk_action_t action = {.name = "test", .json_schema = "[]"};
	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionsRegister,
	    .value.actions_register = {.actions = &action, .actions_len = 1}};
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
	action.json_schema = "{\"broken\":}";
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
	action.json_schema = "{\"type\":\"object\"} trailing";
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);

	char *names[] = {NULL};
	message =
	    (neurosdk_message_t){.kind = NeuroSDK_MessageKind_ActionsUnregister,
	                         .value.actions_unregister = {.action_names = names,
	                                                      .action_names_len = 1}};
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
}

static void test_utf8_semantic_round_trip(void) {
	static char utf8[] =
	    "\302\242 \342\202\254 \360\237\224\245 \344\270\255\346\226\207";
	context_t context = {.game_name = utf8};
	char *json = NULL;

	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_Context,
	                              .value.context = {.message = utf8}};
	json_value_t *root = build_and_parse(&context, &message, &json);
	if (root) {
		json_object_t *root_object = (json_object_t *)root->payload;
		check_string_value(object_get(root_object, "game"), utf8);
		json_value_t *data = object_get(root_object, "data");
		CHECK(data && data->type == json_type_object);
		if (data && data->type == json_type_object)
			check_string_value(object_get((json_object_t *)data->payload, "message"),
			                   utf8);
	}
	free(root);
	free(json);

	neurosdk_action_t action = {.name = utf8,
	                            .description = utf8,
	                            .json_schema = "{\"type\":\"object\"}"};
	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionsRegister,
	    .value.actions_register = {.actions = &action, .actions_len = 1}};
	json = NULL;
	root = build_and_parse(&context, &message, &json);
	if (root) {
		json_value_t *data = object_get((json_object_t *)root->payload, "data");
		json_value_t *actions =
		    data && data->type == json_type_object
		        ? object_get((json_object_t *)data->payload, "actions")
		        : NULL;
		CHECK(actions && actions->type == json_type_array);
		if (actions && actions->type == json_type_array) {
			json_array_t *array = (json_array_t *)actions->payload;
			CHECK(array->start && array->start->value->type == json_type_object);
			if (array->start && array->start->value->type == json_type_object) {
				json_object_t *encoded_action =
				    (json_object_t *)array->start->value->payload;
				check_string_value(object_get(encoded_action, "name"), utf8);
				check_string_value(object_get(encoded_action, "description"), utf8);
			}
		}
	}
	free(root);
	free(json);

	char *names[] = {utf8};
	message =
	    (neurosdk_message_t){.kind = NeuroSDK_MessageKind_ActionsForce,
	                         .value.actions_force = {.state = utf8,
	                                                 .query = utf8,
	                                                 .action_names = names,
	                                                 .action_names_len = 1}};
	json = NULL;
	root = build_and_parse(&context, &message, &json);
	if (root) {
		json_value_t *data = object_get((json_object_t *)root->payload, "data");
		CHECK(data && data->type == json_type_object);
		if (data && data->type == json_type_object) {
			json_object_t *data_object = (json_object_t *)data->payload;
			check_string_value(object_get(data_object, "state"), utf8);
			check_string_value(object_get(data_object, "query"), utf8);
			json_value_t *action_names = object_get(data_object, "action_names");
			CHECK(action_names && action_names->type == json_type_array);
			if (action_names && action_names->type == json_type_array) {
				json_array_t *array = (json_array_t *)action_names->payload;
				CHECK(array->start != NULL);
				if (array->start)
					check_string_value(array->start->value, utf8);
			}
		}
	}
	free(root);
	free(json);

	message = (neurosdk_message_t){
	    .kind = NeuroSDK_MessageKind_ActionResult,
	    .value.action_result = {.id = utf8, .success = true, .message = utf8}};
	json = NULL;
	root = build_and_parse(&context, &message, &json);
	if (root) {
		json_value_t *data = object_get((json_object_t *)root->payload, "data");
		CHECK(data && data->type == json_type_object);
		if (data && data->type == json_type_object) {
			json_object_t *data_object = (json_object_t *)data->payload;
			check_string_value(object_get(data_object, "id"), utf8);
			check_string_value(object_get(data_object, "message"), utf8);
		}
	}
	free(root);
	free(json);
}

static void test_size_limit(void) {
	context_t context = {.game_name = "game"};
	char *large = malloc(PROTOCOL_MESSAGE_MAX_SIZE);
	CHECK(large != NULL);
	if (!large)
		return;
	memset(large, 'a', PROTOCOL_MESSAGE_MAX_SIZE - 1);
	large[PROTOCOL_MESSAGE_MAX_SIZE - 1] = '\0';
	neurosdk_message_t message = {.kind = NeuroSDK_MessageKind_Context,
	                              .value.context = {.message = large}};
	char *json = NULL;
	CHECK(build_c2s_json(&context, &message, &json) == NeuroSDK_InvalidMessage);
	CHECK(json == NULL);
	free(large);
}

int main(void) {
	test_all_message_kinds();
	test_utf8_and_validation();
	test_utf8_semantic_round_trip();
	test_size_limit();
	if (failures) {
		fprintf(stderr, "%d C2S JSON test(s) failed\n", failures);
		return 1;
	}
	puts("C2S JSON tests passed");
	return 0;
}
